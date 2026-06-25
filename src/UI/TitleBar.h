#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace ofs::ui {

// Result of a single title-bar render: which caption button the user clicked this frame, if any.
// The title bar itself performs no window operations (it stays free of any platform/SDL dependency);
// the caller (OfsApp) maps the action onto the native window.
enum class TitleBarAction {
    None,
    Minimize,
    ToggleMaximize,
    Close,
};

// One entry the command palette can list and invoke. Plain POD view (no SDL/service types) so the
// title bar stays dependency-free; the caller (OfsApp) builds these from the command registry each
// frame and maps an invoked index back onto a command id. All pointers must outlive the render call
// (frame-scratch is fine). `shortcut` is a pre-formatted hint like "Ctrl+S" or "" for unbound.
struct TitleBarCommand {
    const char *title = "";
    const char *group = "";
    const char *icon = ""; // category glyph (ICON_* from UI/Icons.h), "" = none
    const char *shortcut = "";
    // Search-only aliases (Command::keywords), folded into the fuzzy haystack below title/group but
    // never rendered — a synonym reaches the command without polluting its label.
    const char *keywords = "";
    // Transient frecency sort key from CommandRegistry::frecency() (0 = unused this session). On an
    // empty query the palette lists commands by this descending; on a query it breaks score ties.
    uint64_t frecency = 0;
};

// Persistent palette state owned by the caller (OfsApp). Moved out of the TU so the
// no-global-UI-state rule is satisfied (was file-scope `gPalette` in the prototype).
struct CommandPaletteState {
    bool justOpened = false;
    std::string query; // std::string-bound so a CJK query isn't truncated at the UTF-8 byte cap
    int selected = 0;
};

// Geometry the caller needs to drive a borderless window's drag/resize hit test. Coordinates are in
// logical points, which (with the SDL3 backend) match SDL window coordinates. The drag region is the
// title bar strip from x = 0 up to (but not including) buttonsLeftX.
struct TitleBarResult {
    TitleBarAction action = TitleBarAction::None;
    float height = 0.0f;
    float buttonsLeftX = 0.0f;
    // Carve-out for the clickable search box inside the drag strip. Equal values = no carve-out.
    float searchLeftX = 0.0f;
    float searchRightX = 0.0f;
    // Index into the `commands` array the user invoked this frame (Enter or click), or -1 if none.
    int invokedCommand = -1;
};

// Custom in-viewport title bar. `paletteState` must be owned by the caller and persisted across
// frames (was `gPalette` in the prototype). `requestOpen` latches to open the palette this frame.
TitleBarResult renderTitleBar(const char *appName, const char *projectTitle, bool isMaximized,
                              std::span<const TitleBarCommand> commands, bool requestOpen,
                              CommandPaletteState &paletteState);

} // namespace ofs::ui
