#include "Format/Funscript.h"
#include "Util/PathUtil.h"
#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>

// These tests guard the one invariant of src/Util/PathUtil.h: every std::string
// that holds a path is UTF-8, and std::filesystem::path is the lossless carrier.
// On Windows path::string() re-encodes to the active ANSI codepage and silently
// drops anything outside it, which is exactly the failure mode we want to catch.
//
// This file is deliberately pure ASCII -- the non-ASCII test strings are built
// from explicit Unicode code points via the utf8()/wide() helpers below. That way
// the bytes are well-defined no matter how MSVC reads the source codepage (the
// project does not pass /utf-8, and a raw multibyte byte would also trip C4828
// under /WX). Code points used:
//   Jose  : J o s U+00E9                         (e-acute, Latin-1)
//   Cjk   : U+65E5 U+672C U+8A9E                  ("Nihongo", CJK)
//   Cyril : U+041F U+0440 U+0438 U+0432 U+0435 U+0442  ("Privet", Cyrillic)
//   Umlaut: u U+00FC b e r _ U+00E4 U+00F6 U+00DF (German umlauts + sharp-s)
//   Emoji : e m o j i _ U+1F3B5 _ c l i p         (musical note; astral / surrogate pair)

namespace {

// Encode a sequence of Unicode code points to a UTF-8 std::string -- the canonical
// representation every path-bearing std::string in the codebase uses.
std::string utf8(std::initializer_list<char32_t> cps) {
    std::string out;
    for (char32_t c : cps) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// Encode BMP code points to a UTF-16 std::wstring (wchar_t is UTF-16 on Windows).
// Only used for the BMP-only CJK sample, so one wchar_t per code point.
std::wstring wide(std::initializer_list<char32_t> cps) {
    std::wstring out;
    for (char32_t c : cps)
        out += static_cast<wchar_t>(c);
    return out;
}

// A spread of scripts that all live outside the typical Windows-1252 ANSI codepage
// (the umlauts/accents are inside 1252 but remain useful round-trip sanity checks),
// so path::string() would mangle at least the CJK/Cyrillic/emoji entries.
const std::string kJose = utf8({'J', 'o', 's', 0x00E9});
const std::string kCjk = utf8({0x65E5, 0x672C, 0x8A9E});
const std::string kCyrillic = utf8({0x041F, 0x0440, 0x0438, 0x0432, 0x0435, 0x0442});
const std::string kUmlaut = utf8({'u', 0x00FC, 'b', 'e', 'r', '_', 0x00E4, 0x00F6, 0x00DF});
const std::string kEmoji = utf8({'e', 'm', 'o', 'j', 'i', '_', 0x1F3B5, '_', 'c', 'l', 'i', 'p'});

const std::string kNames[] = {kJose, kCjk, kCyrillic, kUmlaut, kEmoji};

} // namespace

TEST_CASE("fromUtf8/toUtf8 round-trips non-ASCII strings losslessly") {
    for (const auto &name : kNames) {
        CAPTURE(name);
        const std::filesystem::path p = ofs::util::fromUtf8(name);
        // The bytes that survive the path carrier must be byte-identical to the input.
        CHECK(ofs::util::toUtf8(p) == name);
    }
}

TEST_CASE("fromUtf8 preserves non-ASCII in the wide path representation") {
    // The path must NOT collapse to the ANSI question-mark replacement that
    // path::string() produces for out-of-codepage characters.
    const std::filesystem::path p = ofs::util::fromUtf8(kCjk + ".funscript");
    // The native wide string keeps the three CJK code points plus the extension.
    CHECK(p.wstring() == wide({0x65E5, 0x672C, 0x8A9E, '.', 'f', 'u', 'n', 's', 'c', 'r', 'i', 'p', 't'}));
    // No information was lost on the narrow boundary.
    CHECK(ofs::util::toUtf8(p) == kCjk + ".funscript");
}

TEST_CASE("toUtf8 composes for nested non-ASCII directory + filename") {
    const std::filesystem::path dir = ofs::util::fromUtf8(kCjk);
    // operator/ on a path arg (not a narrow string) stays in the wide domain -- safe.
    const std::filesystem::path full = dir / ofs::util::fromUtf8(kJose + ".funscript");
    const std::string utf8Path = ofs::util::toUtf8(full);
    CHECK(utf8Path.find(kCjk) != std::string::npos);
    CHECK(utf8Path.find(kJose + ".funscript") != std::string::npos);
}

TEST_CASE("filesystem round-trip through a non-ASCII directory and filename") {
    // getPrefPath() resolves to a temp subdir for test binaries (OFS_TEST_PREF_SUBDIR),
    // so this never touches real user data and is safe to create/delete.
    const std::filesystem::path root = ofs::util::getPrefPath() / ofs::util::fromUtf8("path_test_" + kCjk);
    std::error_code ec;
    std::filesystem::remove_all(root, ec); // clean any leftover from a prior run

    for (const auto &name : kNames) {
        CAPTURE(name);
        const std::filesystem::path sub = root / ofs::util::fromUtf8(name);
        REQUIRE(std::filesystem::create_directories(sub, ec));

        const std::filesystem::path file = sub / ofs::util::fromUtf8(name + ".funscript");

        // Save a funscript via the real Format API (uses std::ofstream(path), the wide
        // overload on MSVC), then confirm it lands on disk under the non-ASCII name.
        ofs::VectorSet<ofs::ScriptAxisAction> acts;
        acts.insert({1.5, 40});
        acts.insert({3.0, 80});
        const ofs::Funscript out = ofs::Funscript::fromActions(acts);
        REQUIRE(out.save(file));

        // exists() takes a path (wide) -- the file the OS created must be found again.
        CHECK(std::filesystem::exists(file));

        // Load it back and verify the payload survived the non-ASCII path unchanged.
        const auto loaded = ofs::Funscript::load(file);
        REQUIRE(loaded.has_value());
        const auto roundTripped = loaded->toActions();
        REQUIRE(roundTripped.size() == 2);
        CHECK(roundTripped[0].at == doctest::Approx(1.5));
        CHECK(roundTripped[0].pos == 40);
        CHECK(roundTripped[1].at == doctest::Approx(3.0));
        CHECK(roundTripped[1].pos == 80);

        // The directory iterator must report the same UTF-8 bytes we wrote.
        bool found = false;
        for (const auto &entry : std::filesystem::directory_iterator(sub)) {
            if (ofs::util::toUtf8(entry.path()) == ofs::util::toUtf8(file))
                found = true;
        }
        CHECK(found);
    }

    std::filesystem::remove_all(root, ec);
}

TEST_CASE("getCrashDir is a 'crashes' subfolder of the pref path") {
    // Derived purely from getPrefPath(), which is redirected to a temp dir for tests.
    const std::filesystem::path crash = ofs::util::getCrashDir();
    CHECK(crash.filename() == "crashes");
    CHECK(crash.parent_path() == ofs::util::getPrefPath());
}

TEST_CASE("getBasePath resolves to an existing executable directory") {
    // SDL_GetBasePath returns the running test binary's directory; it must exist and
    // be decodable as UTF-8 (the helper decodes SDL's UTF-8, never the ANSI codepage).
    const std::filesystem::path base = ofs::util::getBasePath();
    REQUIRE_FALSE(base.empty());
    CHECK(std::filesystem::exists(base));
    CHECK(std::filesystem::is_directory(base));
    // Round-trips through the UTF-8 boundary without loss.
    CHECK(ofs::util::fromUtf8(ofs::util::toUtf8(base)) == base);
}

TEST_CASE("fileUri builds a file:/// URI and normalizes separators") {
    // A Windows-style path: every backslash must become a forward slash, and the
    // result must carry the file:/// scheme exactly once.
    const std::filesystem::path win = ofs::util::fromUtf8("C:\\Users\\me\\clip.funscript");
    const std::string uri = ofs::util::fileUri(win);
    CHECK(uri.rfind("file:///", 0) == 0);       // starts with the scheme
    CHECK(uri.find('\\') == std::string::npos); // no backslashes survive
    CHECK(uri == "file:///C:/Users/me/clip.funscript");
}

TEST_CASE("fileUri preserves non-ASCII bytes verbatim (no codepage loss, no escaping)") {
    // The URI keeps raw UTF-8 -- SDL_OpenURL / the OS shell accept it. The non-ASCII
    // directory and filename bytes must appear unchanged (not '?' nor percent-encoded).
    const std::filesystem::path p = ofs::util::fromUtf8(kCjk) / ofs::util::fromUtf8(kJose + ".funscript");
    const std::string uri = ofs::util::fileUri(p);
    CHECK(uri.rfind("file:///", 0) == 0);
    CHECK(uri.find(kCjk) != std::string::npos);
    CHECK(uri.find(kJose + ".funscript") != std::string::npos);
    CHECK(uri.find('%') == std::string::npos); // raw UTF-8, not percent-encoded
}
