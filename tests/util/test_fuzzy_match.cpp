#include <doctest/doctest.h>

#include "Util/FuzzyMatch.h"

using ofs::util::fuzzyMatch;
using ofs::util::fuzzyMatchAny;

TEST_CASE("empty needle matches everything with score 0") {
    const auto r = fuzzyMatch("", "anything at all");
    CHECK(r.matched);
    CHECK(r.score == 0);
}

TEST_CASE("subsequence match, not just substring") {
    CHECK(fuzzyMatch("gcp", "Go to Chapter: Prologue").matched); // g..c..p in order
    CHECK_FALSE(fuzzyMatch("pcg", "Go to Chapter: Prologue").matched);
}

TEST_CASE("case-insensitive (ASCII)") {
    CHECK(fuzzyMatch("SAVE", "Save Project").matched);
    CHECK(fuzzyMatch("save", "SAVE PROJECT").matched);
}

TEST_CASE("query spaces are ignored (multi-word search)") {
    CHECK(fuzzyMatch("go chap", "Go to Chapter").matched);
}

TEST_CASE("non-match returns false with score 0") {
    const auto r = fuzzyMatch("zzz", "Save Project");
    CHECK_FALSE(r.matched);
    CHECK(r.score == 0);
}

TEST_CASE("scoring rewards consecutive over scattered") {
    const auto consec = fuzzyMatch("save", "Save Project");
    const auto scattered = fuzzyMatch("set", "Save Export Tool"); // s..e..t across words
    REQUIRE(consec.matched);
    REQUIRE(scattered.matched);
    CHECK(consec.score > scattered.score);
}

TEST_CASE("scoring rewards prefix / word-boundary hits") {
    // Same letters, but one starts at a word boundary and the other lands mid-word.
    const auto boundary = fuzzyMatch("c", "Go: Chapter");
    const auto midword = fuzzyMatch("c", "Indicator");
    REQUIRE(boundary.matched);
    REQUIRE(midword.matched);
    CHECK(boundary.score > midword.score);
}

TEST_CASE("UTF-8: CJK codepoints match by codepoint") {
    CHECK(fuzzyMatch("\xe7\xab\xa0", "\xe7\xab\xa0\xe8\x8a\x82").matched); // 章 in 章節
    CHECK_FALSE(fuzzyMatch("\xe7\xab\xa0", "\xe8\x8a\x82").matched);       // 章 not in 節
}

TEST_CASE("diacritic-insensitive: ASCII query matches accented text") {
    // "offnen" should match both precomposed forms of "Öffnen".
    CHECK(fuzzyMatch("offnen", "\xc3\x96"
                               "ffnen")
              .matched); // Ö = U+00D6
    CHECK(fuzzyMatch("offnen", "\xc3\xb6"
                               "ffnen")
              .matched);                              // ö = U+00F6
    CHECK(fuzzyMatch("cafe", "Caf\xc3\xa9").matched); // é = U+00E9
    // Expanding ligatures collapse to a SINGLE base letter (ß → s, not "ss"): the matcher works
    // codepoint-to-codepoint and cannot expand one source char into two. So "strase" matches but the
    // double-s spelling "strasse" does not — a documented limitation, asserted here so it stays honest.
    CHECK(fuzzyMatch("strase", "Stra\xc3\x9f"
                               "e")
              .matched);
    CHECK_FALSE(fuzzyMatch("strasse", "Stra\xc3\x9f"
                                      "e")
                    .matched);
}

TEST_CASE("diacritic-insensitive across Latin Extended-A") {
    CHECK(fuzzyMatch("zluto", "\xc5\xbd"
                              "lu\xc5\xa5o")
              .matched); // Žluťo → zluto
}

TEST_CASE("Latin-1 accented letters fold to their base letter") {
    // One representative per Latin-1 Supplement base group not already exercised above (a/e/o/y are
    // covered by the Öffnen/Café cases); each pins a distinct latinBase() arm.
    CHECK(fuzzyMatch("a", "\xc3\x86").matched); // Æ U+00C6 ligature → a
    CHECK(fuzzyMatch("c", "\xc3\x87").matched); // Ç U+00C7 → c
    CHECK(fuzzyMatch("i", "\xc3\x8c").matched); // Ì U+00CC → i
    CHECK(fuzzyMatch("d", "\xc3\x90").matched); // Ð U+00D0 eth → d
    CHECK(fuzzyMatch("n", "\xc3\x91").matched); // Ñ U+00D1 → n
    CHECK(fuzzyMatch("u", "\xc3\x99").matched); // Ù U+00D9 → u
}

TEST_CASE("Latin Extended-A letter groups each fold to their base letter") {
    // One codepoint per contiguous upper/lower group in latinBase(); z/t are covered by Žluťo above.
    CHECK(fuzzyMatch("a", "\xc4\x80").matched); // Ā U+0100 → a
    CHECK(fuzzyMatch("c", "\xc4\x86").matched); // Ć U+0106 → c
    CHECK(fuzzyMatch("d", "\xc4\x8e").matched); // Ď U+010E → d
    CHECK(fuzzyMatch("e", "\xc4\x92").matched); // Ē U+0112 → e
    CHECK(fuzzyMatch("g", "\xc4\x9c").matched); // Ĝ U+011C → g
    CHECK(fuzzyMatch("h", "\xc4\xa4").matched); // Ĥ U+0124 → h
    CHECK(fuzzyMatch("i", "\xc4\xa8").matched); // Ĩ U+0128 → i
    CHECK(fuzzyMatch("j", "\xc4\xb4").matched); // Ĵ U+0134 → j
    CHECK(fuzzyMatch("k", "\xc4\xb6").matched); // Ķ U+0136 → k
    CHECK(fuzzyMatch("l", "\xc4\xb9").matched); // Ĺ U+0139 → l
    CHECK(fuzzyMatch("n", "\xc5\x83").matched); // Ń U+0143 → n
    CHECK(fuzzyMatch("o", "\xc5\x8c").matched); // Ō U+014C → o
    CHECK(fuzzyMatch("r", "\xc5\x94").matched); // Ŕ U+0154 → r
    CHECK(fuzzyMatch("s", "\xc5\x9a").matched); // Ś U+015A → s
    CHECK(fuzzyMatch("u", "\xc5\xa8").matched); // Ũ U+0168 → u
    CHECK(fuzzyMatch("w", "\xc5\xb4").matched); // Ŵ U+0174 → w
    CHECK(fuzzyMatch("y", "\xc5\xb6").matched); // Ŷ U+0176 → y
    CHECK(fuzzyMatch("s", "\xc5\xbf").matched); // ſ U+017F long s → s
}

TEST_CASE("4-byte UTF-8 codepoints decode and match (astral plane)") {
    // U+1F600 😀 exercises the 4-byte decodeUtf8 arm; matches itself by codepoint, and a different
    // emoji (U+1F601 😁 = F0 9F 98 81) does not.
    CHECK(fuzzyMatch("\xf0\x9f\x98\x80", "\xf0\x9f\x98\x80").matched);
    CHECK_FALSE(fuzzyMatch("\xf0\x9f\x98\x80", "\xf0\x9f\x98\x81").matched);
}

TEST_CASE("combining marks (NFD) fold the same as precomposed") {
    // "o" + U+0308 combining diaeresis == "ö"; the combining mark is skipped, base 'o' matches.
    CHECK(fuzzyMatch("o", "o\xcc\x88").matched);
    CHECK(fuzzyMatch("offnen", "O\xcc\x88"
                               "ffnen")
              .matched);
}

TEST_CASE("fuzzyMatchAny returns the best score across fields") {
    const auto r = fuzzyMatchAny("blur", {"Generate", "Gaussian Blur", "Modify"});
    CHECK(r.matched);
    // The best field ("Gaussian Blur") should out-score a non-match; matched at all is the contract.
    CHECK(r.score > 0);
}

TEST_CASE("fuzzyMatchAny is false when no field matches") {
    CHECK_FALSE(fuzzyMatchAny("xyz", {"Generate", "Modify", "Combine"}).matched);
}

TEST_CASE("match positions report haystack byte offsets") {
    int offs[8] = {};
    int n = 0;
    const auto r = fuzzyMatch("sp", "Save Project", offs, 8, &n);
    REQUIRE(r.matched);
    REQUIRE(n == 2);
    CHECK(offs[0] == 0); // 'S' at byte 0
    CHECK(offs[1] == 5); // 'P' at byte 5 (after "Save ")
}

TEST_CASE("invalid UTF-8 does not crash and degrades to bytes") {
    const auto r = fuzzyMatch("a", "\xff\xfe"
                                   "abc"); // leading invalid bytes
    CHECK(r.matched);
}
