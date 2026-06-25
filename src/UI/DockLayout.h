#pragma once

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

} // namespace ofs::ui
