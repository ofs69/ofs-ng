#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Shipped runtime assets (fonts, the simulator model, base themes, shipped translations) are packed
// into a single data.pak staged next to the executable (cmake/PackAssets.cmake). ofs::res is the one
// reader for them: it replaces the former loose-file copies and the bespoke OfsEmbedded{Themes,Simulator}
// header embeds. Names are archive-relative and mirror the source tree, e.g. "data/fonts/lucide.ttf",
// "data/simulator.glb", "data/themes/dark.json", "lang/de.toml".
//
// In a dev build the staged zip may be absent; every lookup then falls back to reading the same name
// straight from the source tree (OFS_ASSETS_FALLBACK_DIR), so running from a fresh build tree still
// works. Intended for main-thread load-time use; calls are internally serialized.
namespace ofs::res {

// Reads an asset into a heap buffer. Returns nullopt (and logs) if absent in both the archive and the
// dev fallback.
std::optional<std::vector<std::byte>> read(std::string_view name);

// Convenience wrapper around read() for text assets (themes, TOML).
std::optional<std::string> readText(std::string_view name);

// Archive-relative names of every entry whose name starts with `prefix` (e.g. "lang/"). Used to
// enumerate shipped resources such as the available translations.
std::vector<std::string> list(std::string_view prefix);

} // namespace ofs::res
