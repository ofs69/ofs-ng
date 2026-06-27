#pragma once

#include <string>

namespace ofs {

// ── Localization events (UI pushes; handled in the OfsApp composition root) ────
struct SetLanguageEvent {
    std::string languageId; // "" / "en" = built-in English
};

struct ExportCatalogEvent {
    std::string path; // destination .toml for a fresh annotated catalog
};

struct RefreshTranslationEvent {
    std::string path; // existing translation .toml to merge against the current source
};

} // namespace ofs
