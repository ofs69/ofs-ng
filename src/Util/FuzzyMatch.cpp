#include "Util/FuzzyMatch.h"

#include <cstddef>
#include <cstdint>

namespace ofs::util {
namespace {

// Decode one UTF-8 codepoint from `s` starting at byte `i`; set `next` to the byte after it. An
// invalid lead byte or truncated sequence consumes a single byte and returns it raw, so malformed
// input degrades gracefully instead of looping or crashing.
uint32_t decodeUtf8(std::string_view s, size_t i, size_t &next) {
    const auto c0 = static_cast<unsigned char>(s[i]);
    if (c0 < 0x80) {
        next = i + 1;
        return c0;
    }
    const auto cont = [&](size_t k) { return k < s.size() && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80; };
    if ((c0 & 0xE0) == 0xC0 && cont(i + 1)) {
        next = i + 2;
        return ((c0 & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
    }
    if ((c0 & 0xF0) == 0xE0 && cont(i + 1) && cont(i + 2)) {
        next = i + 3;
        return ((c0 & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
               (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
    }
    if ((c0 & 0xF8) == 0xF0 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
        next = i + 4;
        return ((c0 & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
               ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
    }
    next = i + 1;
    return c0;
}

// Base ASCII lowercase letter for an accented Latin letter (Latin-1 Supplement + Latin Extended-A),
// or 0 if the codepoint is not a handled accented letter. Ligatures collapse pragmatically to a single
// letter (Æ/æ→a, Œ/œ→o, ß→s, Þ/þ thorn→t) since the matcher works codepoint-to-codepoint and cannot
// expand one source char into two. Non-letters in these ranges (× ÷) return 0 and stay unchanged.
uint32_t latinBase(uint32_t cp) {
    switch (cp) {
    case 0xC0:
    case 0xC1:
    case 0xC2:
    case 0xC3:
    case 0xC4:
    case 0xC5:
    case 0xC6:
    case 0xE0:
    case 0xE1:
    case 0xE2:
    case 0xE3:
    case 0xE4:
    case 0xE5:
    case 0xE6:
        return 'a';
    case 0xC7:
    case 0xE7:
        return 'c';
    case 0xC8:
    case 0xC9:
    case 0xCA:
    case 0xCB:
    case 0xE8:
    case 0xE9:
    case 0xEA:
    case 0xEB:
        return 'e';
    case 0xCC:
    case 0xCD:
    case 0xCE:
    case 0xCF:
    case 0xEC:
    case 0xED:
    case 0xEE:
    case 0xEF:
        return 'i';
    case 0xD0:
    case 0xF0:
        return 'd';
    case 0xD1:
    case 0xF1:
        return 'n';
    case 0xD2:
    case 0xD3:
    case 0xD4:
    case 0xD5:
    case 0xD6:
    case 0xD8:
    case 0xF2:
    case 0xF3:
    case 0xF4:
    case 0xF5:
    case 0xF6:
    case 0xF8:
        return 'o';
    case 0xD9:
    case 0xDA:
    case 0xDB:
    case 0xDC:
    case 0xF9:
    case 0xFA:
    case 0xFB:
    case 0xFC:
        return 'u';
    case 0xDD:
    case 0xFD:
    case 0xFF:
        return 'y';
    case 0xDE:
    case 0xFE:
        return 't';
    case 0xDF:
        return 's';
    default:
        break;
    }
    // Latin Extended-A — contiguous upper/lower letter groups.
    if (cp >= 0x100 && cp <= 0x105)
        return 'a';
    if (cp >= 0x106 && cp <= 0x10D)
        return 'c';
    if (cp >= 0x10E && cp <= 0x111)
        return 'd';
    if (cp >= 0x112 && cp <= 0x11B)
        return 'e';
    if (cp >= 0x11C && cp <= 0x123)
        return 'g';
    if (cp >= 0x124 && cp <= 0x127)
        return 'h';
    if (cp >= 0x128 && cp <= 0x131)
        return 'i';
    if (cp >= 0x134 && cp <= 0x135)
        return 'j';
    if (cp >= 0x136 && cp <= 0x138)
        return 'k';
    if (cp >= 0x139 && cp <= 0x142)
        return 'l';
    if (cp >= 0x143 && cp <= 0x14B)
        return 'n';
    if (cp >= 0x14C && cp <= 0x153)
        return 'o';
    if (cp >= 0x154 && cp <= 0x159)
        return 'r';
    if (cp >= 0x15A && cp <= 0x161)
        return 's';
    if (cp >= 0x162 && cp <= 0x167)
        return 't';
    if (cp >= 0x168 && cp <= 0x173)
        return 'u';
    if (cp >= 0x174 && cp <= 0x175)
        return 'w';
    if (cp >= 0x176 && cp <= 0x178)
        return 'y';
    if (cp >= 0x179 && cp <= 0x17E)
        return 'z';
    if (cp == 0x17F)
        return 's'; // ſ long s
    return 0;
}

// Fold a codepoint for matching: ASCII/Latin case-fold + diacritic-strip to a base letter. Returns 0
// for combining diacritical marks (U+0300..U+036F) so the caller skips them — decomposed (NFD) input
// then folds the same as precomposed. CJK and other scripts have no case and pass through unchanged.
uint32_t foldCodepoint(uint32_t cp) {
    if (cp < 0x80)
        return (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
    if (cp >= 0x300 && cp <= 0x36F)
        return 0;
    if (const uint32_t b = latinBase(cp))
        return b;
    return cp;
}

// Word-boundary characters: a match right after one earns a bonus (so "gc" ranks "Go: Chapter" high).
bool isSeparator(uint32_t folded) {
    return folded == ' ' || folded == ':' || folded == '/' || folded == '-' || folded == '_';
}

FuzzyResult matchImpl(std::string_view needle, std::string_view haystack, int *outOffsets, int maxMatches,
                      int *outCount) {
    if (outCount)
        *outCount = 0;

    int score = 0;
    bool any = false;
    int matchCount = 0;

    size_t hByte = 0;       // byte cursor into haystack (persists across needle codepoints)
    int hIdx = 0;           // significant-codepoint index into haystack
    int prevMatchIdx = -2;  // codepoint index of the previous matched haystack cp (consecutive bonus)
    uint32_t prevHayCp = 0; // last significant haystack cp consumed (boundary bonus); 0 = string start

    size_t nByte = 0;
    while (nByte < needle.size()) {
        size_t nNext = nByte;
        const uint32_t nf = foldCodepoint(decodeUtf8(needle, nByte, nNext));
        nByte = nNext;
        if (nf == 0 || nf == ' ') // skip combining marks and spaces in the query (multi-word search)
            continue;
        any = true;

        bool found = false;
        while (hByte < haystack.size()) {
            size_t hNext = hByte;
            const uint32_t hf = foldCodepoint(decodeUtf8(haystack, hByte, hNext));
            const size_t startByte = hByte;
            hByte = hNext;
            if (hf == 0) // combining mark in haystack: skip, no index/boundary contribution
                continue;
            if (hf == nf) {
                score += 10;
                if (hIdx == prevMatchIdx + 1)
                    score += 15; // consecutive run
                if (hIdx == 0)
                    score += 10; // prefix
                else if (isSeparator(prevHayCp))
                    score += 8; // word boundary
                if (outOffsets && matchCount < maxMatches)
                    outOffsets[matchCount] = static_cast<int>(startByte);
                ++matchCount;
                prevMatchIdx = hIdx;
                prevHayCp = hf;
                ++hIdx;
                found = true;
                break;
            }
            prevHayCp = hf;
            ++hIdx;
        }
        if (!found)
            return {.matched = false, .score = 0};
    }

    if (outCount)
        // Report only what we actually wrote: the loop above stops filling outOffsets at maxMatches, so a
        // caller iterating *outCount entries would otherwise read past the offsets buffer.
        *outCount = (outOffsets && matchCount > maxMatches) ? maxMatches : matchCount;
    return {.matched = true, .score = any ? score : 0};
}

} // namespace

FuzzyResult fuzzyMatch(std::string_view needle, std::string_view haystack) {
    return matchImpl(needle, haystack, nullptr, 0, nullptr);
}

FuzzyResult fuzzyMatch(std::string_view needle, std::string_view haystack, int *outMatchByteOffsets, int maxMatches,
                       int *outMatchCount) {
    return matchImpl(needle, haystack, outMatchByteOffsets, maxMatches, outMatchCount);
}

FuzzyResult fuzzyMatchAny(std::string_view needle, std::initializer_list<std::string_view> haystacks) {
    FuzzyResult best;
    for (const auto h : haystacks) {
        const FuzzyResult r = matchImpl(needle, h, nullptr, 0, nullptr);
        if (r.matched && (!best.matched || r.score > best.score))
            best = r;
    }
    return best;
}

} // namespace ofs::util
