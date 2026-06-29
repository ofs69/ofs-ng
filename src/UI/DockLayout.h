#pragma once

#include <string>

namespace ofs::ui {

// Builds the default docked layout on first run, then submits the dockspace over the main viewport.
// When `locked`, the dockspace is submitted with ImGuiDockNodeFlags_NoUndocking so windows can't be
// dragged out of their dock node (splitter resizing still works). Uses the imgui_internal
// DockBuilder* API (not part of the public imgui.h API), so it lives in its own translation unit
// rather than in OfsApp.
void beginDockspace(bool locked);

// Rebuild the hardcoded default arrangement immediately, discarding the current one. Call between
// frames (e.g. at the top of onUpdate, before windows are submitted) to factory-reset the layout.
void applyDefaultLayout();

// Load a saved layout's ImGui ini, then DPI-correct it. The ini stores absolute node pixel sizes
// captured at some content scale; `scaleFactor` is current/savedScale. With factor 1 (or non-positive,
// i.e. an unknown save scale) the layout is applied verbatim. Otherwise every dock node's reference
// size is scaled by the factor: the dockspace's central node absorbs the remainder, so the peripheral
// panels grow/shrink with the content while the central editor area takes up the slack. Call between
// frames, like applyDefaultLayout().
void applyLayoutIni(const std::string &ini, float scaleFactor);

} // namespace ofs::ui
