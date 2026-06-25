#include "Localization/StringsGenerated.h"
#include "Localization/TrKey.h"
#include "Localization/Translator.h"
#include "Util/PathUtil.h"
#include <algorithm>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>

using ofs::loc::Translator;

static uint32_t idx(Tr key) {
    return static_cast<uint32_t>(key);
}

namespace {
// Write `contents` to <prefPath>/lang/<id>.toml (the user override location
// resolveLanguageFile() searches first) and return its path.
std::filesystem::path writeLangFile(std::string_view id, std::string_view contents) {
    const auto dir = ofs::util::getPrefPath() / "lang";
    std::filesystem::create_directories(dir);
    const auto path = dir / (std::string(id) + ".toml");
    std::ofstream(path, std::ios::binary) << contents;
    return path;
}
} // namespace

TEST_CASE("loadDefaults yields the baked-in English strings") {
    auto &tr = Translator::instance();
    tr.loadDefaults();
    CHECK(std::string(tr.translation[idx(Tr::PrefTitle)]) == ofs::loc::gen::Default[idx(Tr::PrefTitle)]);
    CHECK(tr.activeLanguage() == "en"); // loadDefaults() marks English active
}

TEST_CASE("load(\"en\") and load(\"\") both select built-in English") {
    auto &tr = Translator::instance();
    CHECK(tr.load("en"));
    CHECK(tr.load(""));
}

TEST_CASE("load on a missing language fails and leaves state intact") {
    auto &tr = Translator::instance();
    tr.loadDefaults();
    const std::string before = tr.activeLanguage();
    CHECK_FALSE(tr.load("zz_nonexistent"));
    CHECK(tr.activeLanguage() == before); // never half-applied
}

TEST_CASE("available() always includes English and is sorted") {
    auto langs = Translator::instance().available();
    REQUIRE_FALSE(langs.empty());
    CHECK(std::find(langs.begin(), langs.end(), "en") != langs.end());
    CHECK(std::is_sorted(langs.begin(), langs.end()));
}

TEST_CASE("exportCatalog writes a catalog file") {
    auto path = std::filesystem::temp_directory_path() / "ofs_catalog.toml";
    CHECK(Translator::instance().exportCatalog(path));
    CHECK(std::filesystem::exists(path));
    std::filesystem::remove(path);
}

TEST_CASE("exportCatalog creates missing parent dirs") {
    auto path = std::filesystem::temp_directory_path() / "ofs_export_dir" / "lang" / "ja.toml";
    std::filesystem::remove_all(path.parent_path().parent_path());
    CHECK(Translator::instance().exportCatalog(path));
    CHECK(std::filesystem::exists(path));
    std::filesystem::remove_all(path.parent_path().parent_path());
}

// A catalog covering every acceptance branch in load(): a plain accepted string, a
// placeholder string whose mask matches, a placeholder mismatch (kept English), a
// whitespace-only value (kept English), an unknown key, a non-table top-level node,
// and a deliberately malformed format string (accepted because it has no {N} tokens).
// Keys are real catalog entries (Preferences window); the values are test fixtures.
constexpr std::string_view kCatalog = R"(stray = 123

[PrefTitle]
translation = "Einstellungen"

[PrefTabApplication]
translation = "   "

[PrefThemeSaved]
translation = "Thema {0} gespeichert"

[PrefThemeImported]
translation = "kaputt"

[PrefDelete]
translation = "}{"

[ZzzUnknownKey]
translation = "ignored"
)";

TEST_CASE("load() applies a user TOML and validates each entry") {
    auto &tr = Translator::instance();
    writeLangFile("tt", kCatalog);
    REQUIRE(tr.load("tt"));
    CHECK(tr.activeLanguage() == "tt");

    CHECK(std::string(tr.translation[idx(Tr::PrefTitle)]) == "Einstellungen");              // plain accept
    CHECK(std::string(tr.translation[idx(Tr::PrefThemeSaved)]) == "Thema {0} gespeichert"); // {0} mask matches
    // Mismatched placeholders ({0} required, none supplied) and whitespace-only values fall back to English.
    CHECK(std::string(tr.translation[idx(Tr::PrefThemeImported)]) ==
          ofs::loc::gen::Default[idx(Tr::PrefThemeImported)]);
    CHECK(std::string(tr.translation[idx(Tr::PrefTabApplication)]) ==
          ofs::loc::gen::Default[idx(Tr::PrefTabApplication)]);

    tr.loadDefaults(); // restore English for the rest of the suite
}

TEST_CASE("activeLanguageCode reads [_meta].iso639, never the filename") {
    auto &tr = Translator::instance();

    tr.loadDefaults();
    CHECK(tr.activeLanguageCode() == "en"); // built-in English

    // The id stem is "jp_marker" but the file declares "ja" — the code follows the file, not the name.
    writeLangFile("jp_marker", "[_meta]\niso639 = \"ja\"\n\n[PrefTitle]\ntranslation = \"設定\"\n");
    REQUIRE(tr.load("jp_marker"));
    CHECK(tr.activeLanguageCode() == "ja");

    // A file that declares no code falls back to "en" (so plugins show their neutral catalog).
    writeLangFile("nometa", "[PrefTitle]\ntranslation = \"X\"\n");
    REQUIRE(tr.load("nometa"));
    CHECK(tr.activeLanguageCode() == "en");

    tr.loadDefaults();
}

TEST_CASE("exportCatalog round-trips the active language code via [_meta]") {
    auto &tr = Translator::instance();
    writeLangFile("src", "[_meta]\niso639 = \"ja\"\n\n[PrefTitle]\ntranslation = \"設定\"\n");
    REQUIRE(tr.load("src"));

    // Export into the pref lang/ dir so load() (which searches there first) can read it straight back.
    const auto dest = ofs::util::getPrefPath() / "lang" / "exported.toml";
    REQUIRE(tr.exportCatalog(dest));
    tr.load(""); // exportCatalog reads active state; clear it before re-loading the exported file

    // Loading the exported catalog back yields the same code — the export carried [_meta] through.
    REQUIRE(tr.load("exported"));
    CHECK(tr.activeLanguageCode() == "ja");

    std::filesystem::remove(dest);
    tr.loadDefaults();
}

TEST_CASE("exportCatalog embeds the active language's translations") {
    auto &tr = Translator::instance();
    writeLangFile("tt", kCatalog);
    REQUIRE(tr.load("tt"));

    // Export the active language into the user lang dir, then load it back: a format-independent
    // round-trip proving the translations were carried into the exported catalog.
    const auto path = ofs::util::getPrefPath() / "lang" / "exported.toml";
    REQUIRE(tr.exportCatalog(path));
    REQUIRE(tr.load("exported"));
    CHECK(std::string(tr.translation[idx(Tr::PrefTitle)]) == "Einstellungen");
    // A key untranslated in "tt" stays empty in the export, so it still falls back to English.
    CHECK(std::string(tr.translation[idx(Tr::PrefTabApplication)]) ==
          ofs::loc::gen::Default[idx(Tr::PrefTabApplication)]);

    std::filesystem::remove(path);
    tr.loadDefaults();
}

TEST_CASE("load() returns false on a malformed TOML and keeps state intact") {
    auto &tr = Translator::instance();
    tr.loadDefaults();
    writeLangFile("bad", "[Ok\ntranslation = ");
    const std::string before = tr.activeLanguage();
    CHECK_FALSE(tr.load("bad"));
    CHECK(tr.activeLanguage() == before);
}

TEST_CASE("trLookup/trId/trFormat resolve through the active table") {
    auto &tr = Translator::instance();
    writeLangFile("tt", kCatalog);
    REQUIRE(tr.load("tt"));

    CHECK(std::string(ofs::loc::trLookup(idx(Tr::PrefTitle))) == "Einstellungen");
    CHECK(std::string(ofs::loc::trLookup(9999)) == ""); // out of range -> empty
    CHECK(std::string(Str::PrefTitle.id("stable")) == "Einstellungen###stable");
    CHECK(std::string(Str::PrefThemeSaved.fmt(5)) == "Thema 5 gespeichert");
    // The "}{" translation is a malformed format string: trFormat must swallow the
    // fmt exception and return the raw string rather than crash.
    CHECK(std::string(Str::PrefDelete.fmt()) == "}{");

    tr.loadDefaults();
}

TEST_CASE("refreshTranslation merges against current source") {
    auto path = writeLangFile("tt", kCatalog);
    CHECK(Translator::instance().refreshTranslation(path));
    // Existing translations survive the round-trip.
    CHECK(Translator::instance().load("tt"));
    CHECK(std::string(Translator::instance().translation[idx(Tr::PrefTitle)]) == "Einstellungen");
    Translator::instance().loadDefaults();
}

TEST_CASE("refreshTranslation fails on a malformed file") {
    auto path = writeLangFile("bad", "= = =");
    CHECK_FALSE(Translator::instance().refreshTranslation(path));
}

TEST_CASE("pollReload reloads a file-backed language when its mtime changes") {
    auto &tr = Translator::instance();
    auto path = writeLangFile("tt", kCatalog);
    REQUIRE(tr.load("tt"));

    // Bump the file's mtime forward so pollReload sees a change and re-loads.
    auto t = std::filesystem::last_write_time(path);
    std::filesystem::last_write_time(path, t + std::chrono::seconds(2));
    tr.pollReload();
    CHECK(tr.activeLanguage() == "tt");

    tr.loadDefaults();
    tr.pollReload(); // English has no backing file: early return, no crash
    CHECK(tr.activeLanguage() == "en");
}

TEST_CASE("available() discovers user TOML files on disk") {
    writeLangFile("tt", kCatalog);
    auto langs = Translator::instance().available();
    CHECK(std::find(langs.begin(), langs.end(), "tt") != langs.end());
    Translator::instance().loadDefaults();
}
