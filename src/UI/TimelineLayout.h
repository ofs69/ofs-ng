#pragma once

// Shared timeline layout geometry — single source of truth for the renderer and the
// UI tests, so test coordinate helpers reference the real value instead of a copy.

namespace ofs::ui {

// Vertical padding (px) inside the timeline script-line area, above and below the 0–100 band.
inline constexpr float kScriptLineVMargin = 8.0f;

} // namespace ofs::ui
