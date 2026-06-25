#pragma once

// Translator — runtime localization table.
//
// Owns the array of resolved `const char*` translations (indexed by Tr value) and the contiguous
// backing blob. A plain lookup (TrKey -> const char*) is a single array index, safe every frame.
// Loading or switching a language parses a TOML file, validates it, builds one new blob, and
// atomically swaps it in; on any failure the current language stays fully intact.
//
// Lifecycle uses the codebase's Meyers-singleton pattern (cf. FrameAllocator::instance(),
// getPrefPath()). All load()/swap operations run on the main thread between frames; workers never
// touch the Translator (per the threading model).

#include "Localization/StringsGenerated.h"
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ofs::loc {

class Translator {
  public:
    static Translator &instance();

    // Reset every entry to the baked-in English default and clear the active-file state.
    void loadDefaults();

    // Parse + validate lang/<id>.toml and atomically swap it in. "" or "en" loads the built-in
    // English defaults. Returns false (leaving current state untouched) on missing file, parse
    // error, or any failure — never a half-applied language.
    bool load(std::string_view languageId);

    // Discover selectable languages: shipped lang/ (next to the binary) + user lang/ under the
    // pref path, deduped by stem (pref-path wins), plus the always-present built-in "en". Sorted.
    [[nodiscard]] std::vector<std::string> available() const;

    // Write a complete strings-shaped catalog (every key annotated) pre-filled with the active
    // language's translations so the user exports the current language and patches it. Untranslated
    // keys get an empty translation field (the English source still shows). Canonical (Tr enum) order.
    [[nodiscard]] bool exportCatalog(const std::filesystem::path &path) const;

    // Merge an existing translation file against the current source: keep existing translations,
    // add keys introduced since, drop keys that no longer exist, refresh reference fields.
    [[nodiscard]] bool refreshTranslation(const std::filesystem::path &path) const;

    // If a file-backed language is active, re-load() it when its mtime changes. Off the normal
    // path: the caller only invokes this when the opt-in live-reload setting is enabled.
    void pollReload();

    [[nodiscard]] const std::string &activeLanguage() const { return activeLanguageId_; }

    // The active language's ISO 639 code ("en", "ja", …). Declared per file in its [_meta] iso639
    // field (built-in English is "en"); this is what plugins receive to drive their own catalogs.
    [[nodiscard]] const std::string &activeLanguageCode() const { return activeLanguageCode_; }

    // Indexed by Tr value. Public so the trLookup() hot path is a single array index.
    std::array<const char *, gen::Count> translation{};

  private:
    Translator();

    std::vector<char> stringData_;         // backing blob; translation[i] points into this or Default[i]
    std::string activeLanguageId_;         // "" / "en" = built-in English
    std::string activeLanguageCode_;       // ISO 639 code from [_meta].iso639 ("en" for built-in English)
    std::filesystem::path activeFilePath_; // empty when English
    std::filesystem::file_time_type lastWriteTime_{};
};

} // namespace ofs::loc
