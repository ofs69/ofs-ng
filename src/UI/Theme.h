#pragma once
#include "UI/Gradient.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imnodes.h"
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ofs::theme {
// Schema version stamped into theme files. Bumped on incompatible schema changes; load()/
// importFromFile() refuse a file newer than this rather than misreading it. There is no
// migration path for older versions — omitted keys simply fall back to base defaults.
inline constexpr int kThemeSchemaVersion = 2;
} // namespace ofs::theme

// AppCol extends ImGuiCol. Values in [0, ImGuiCol_COUNT) are ImGui built-ins;
// values in [ImGuiCol_COUNT, AppCol_COUNT) are ofs-specific custom slots.
//
// Axis slots are ordered to match StandardAxis enum order (L0=0 … S9=19).
// standardAxisColor / standardAxisColorDim use arithmetic into these blocks.
enum AppCol : int {
    // Base axis colors
    AppCol_AxisL0 = ImGuiCol_COUNT,
    AppCol_AxisL1,
    AppCol_AxisL2,
    AppCol_AxisR0,
    AppCol_AxisR1,
    AppCol_AxisR2,
    AppCol_AxisV0,
    AppCol_AxisV1,
    AppCol_AxisA0,
    AppCol_AxisA1,
    AppCol_AxisS0,
    AppCol_AxisS1,
    AppCol_AxisS2,
    AppCol_AxisS3,
    AppCol_AxisS4,
    AppCol_AxisS5,
    AppCol_AxisS6,
    AppCol_AxisS7,
    AppCol_AxisS8,
    AppCol_AxisS9,
    // Darkened axis colors (for region bands)
    AppCol_AxisDimL0,
    AppCol_AxisDimL1,
    AppCol_AxisDimL2,
    AppCol_AxisDimR0,
    AppCol_AxisDimR1,
    AppCol_AxisDimR2,
    AppCol_AxisDimV0,
    AppCol_AxisDimV1,
    AppCol_AxisDimA0,
    AppCol_AxisDimA1,
    AppCol_AxisDimS0,
    AppCol_AxisDimS1,
    AppCol_AxisDimS2,
    AppCol_AxisDimS3,
    AppCol_AxisDimS4,
    AppCol_AxisDimS5,
    AppCol_AxisDimS6,
    AppCol_AxisDimS7,
    AppCol_AxisDimS8,
    AppCol_AxisDimS9,
    // Text
    AppCol_TextShadow,
    // BandBar
    AppCol_BandBarStripe,
    AppCol_BandBarText,
    // Neutral fill for processing-region bands (axis-agnostic; assignment shown via badges)
    AppCol_RegionBand,
    // Bookmarks
    AppCol_Bookmark,
    AppCol_BookmarkDot,
    AppCol_BookmarkDotHovered,
    // Video controls
    AppCol_PlayCursor,
    AppCol_HudBg,
    // Timeline strip
    AppCol_StripBg,
    AppCol_StripActiveBg,
    AppCol_StripHoverBg,
    AppCol_StripSeparator,
    AppCol_StripDivider,
    AppCol_LockIndicator,
    // Timeline script line
    AppCol_ScriptLineBgTop,
    AppCol_ScriptLineBgBottom,
    AppCol_ScriptLineHoverBg,
    AppCol_Waveform, // audio waveform envelope drawn behind the script lines; opaque, high-contrast vs the track
    AppCol_GridLine,
    AppCol_GridLineMid,
    // Timeline actions
    AppCol_SelectedLine,
    AppCol_DragPreview,
    AppCol_DragPreviewOutline,
    AppCol_SelectionBox,
    AppCol_SelectionBoxFill,
    // Action script-line/point colors (black contrast outline + red point, green when selected)
    AppCol_TimelineOutline,
    AppCol_TimelinePoint,
    AppCol_TimelinePointSelected,
    // Overlay lines
    AppCol_OverlayLineMajor,
    AppCol_OverlayLineMinor,
    // Tempo overlay measure (downbeat) line color
    AppCol_TempoMeasureLine,
    // Processing
    AppCol_ProcessingPanelBg,
    AppCol_NodeIO,
    AppCol_NodeDiscrete,
    AppCol_NodeFunctional,
    AppCol_NodeMath,
    AppCol_LinkDiscrete,
    AppCol_LinkFunctional,
    // Simulator 3D views
    AppCol_SimViewBg,
    AppCol_SimTintTop,
    AppCol_SimTintSide,
    AppCol_SimDivider,
    AppCol_SimCrosshair,
    AppCol_SimArcRef,
    AppCol_SimArc,
    // Simulator 2D bar
    AppCol_Sim2DFront,
    AppCol_Sim2DBack,
    AppCol_Sim2DBorder,
    AppCol_Sim2DText,
    AppCol_Sim2DLines,
    AppCol_Sim2DIndicator,
    // Video controls timeline cursors
    AppCol_TimelineCursorOuter,
    AppCol_TimelineCursorInner,
    // Video controls timeline seek bar fill
    AppCol_VideoTimelineFill,
    AppCol_TimelineVisibleRegionFill,
    AppCol_TimelineVisibleRegionBorder,
    // Bookmark dot outline
    AppCol_BookmarkOutline,
    // Heatmap shader zero-speed base color (blends into background at low speed)
    AppCol_HeatmapBase,
    // Menu bar unsaved-changes indicator
    AppCol_UnsavedIndicator,
    // Script timeline cursors
    AppCol_ScriptSeekCursor,
    AppCol_ScriptPlayCursor,
    // Semantic status colors (e.g. positive/negative deltas, valid/invalid input, notifications)
    AppCol_Success,
    AppCol_Warning,
    AppCol_Error,
    AppCol_COUNT,
};

// AppVar extends ImGuiStyleVar exactly as AppCol extends ImGuiCol. Values in
// [0, ImGuiStyleVar_COUNT) are ImGui built-in style vars; values in
// [ImGuiStyleVar_COUNT, AppVar_COUNT) are ofs-specific custom geometry/metrics.
// The slot order MUST match kCustomVarNames (Theme.cpp) and the appVars keys
// emitted by tools/gen_theme.py.
enum AppVar : int {
    // Timeline cursor widths
    AppVar_ScriptSeekCursorWidth = ImGuiStyleVar_COUNT,
    AppVar_ScriptPlayCursorWidth,
    // Timeline grid/overlay coarse-line widths (fine lines are fixed at 1px)
    AppVar_GridLineMidWidth,
    AppVar_OverlayLineMajorWidth,
    // Timeline action script-line stroke width; the contrast outline behind it derives from this
    AppVar_TimelineLineWidth,
    // Audio waveform envelope height as a fraction of the lane half-height (<1 leaves an edge margin)
    AppVar_WaveformScale,
    // Simulator 2D bar: thickness/border/line widths are now derived from the content-space
    // OverlayAnchor::widthNorm (they scale with zoom), so only the opacity remains a theme var.
    AppVar_SimGlobalOpacity,
    // imnodes geometry mirror (copied into Theme::nodes by apply(); DPI-scaled in Application)
    AppVar_NodeGridSpacing,
    AppVar_NodeCornerRounding,
    AppVar_NodePadding, // ImVec2
    AppVar_NodeBorderThickness,
    AppVar_NodeLinkThickness,
    AppVar_NodePinRadius,
    AppVar_COUNT,
};

namespace ofs::theme {

namespace detail {
// Storage for the custom (>= ImGuiCol_COUNT) slots only.
inline ImColor gCustomColors[static_cast<int>(AppCol_COUNT) - static_cast<int>(ImGuiCol_COUNT)] = {};
} // namespace detail

// Mirrors ImGui::GetColorU32(ImGuiCol, float). For indices below ImGuiCol_COUNT
// this delegates to ImGui; for ofs-specific indices it reads gCustomColors.
inline ImU32 GetColorU32(AppCol idx, float alphaMul = 1.f) {
    if (idx < static_cast<AppCol>(ImGuiCol_COUNT))
        return ImGui::GetColorU32(static_cast<ImGuiCol>(idx), alphaMul);
    ImColor c = detail::gCustomColors[static_cast<int>(idx) - static_cast<int>(ImGuiCol_COUNT)];
    c.Value.w *= alphaMul;
    return c;
}

inline ImU32 GetColorU32(ImGuiCol idx, float alphaMul = 1.f) {
    return GetColorU32(static_cast<AppCol>(idx), alphaMul);
}

inline const ImVec4 &GetStyleColorVec4(AppCol idx) {
    if (idx < static_cast<AppCol>(ImGuiCol_COUNT))
        return ImGui::GetStyleColorVec4(static_cast<ImGuiCol>(idx));
    return detail::gCustomColors[static_cast<int>(idx) - static_cast<int>(ImGuiCol_COUNT)].Value;
}

inline const ImVec4 &GetStyleColorVec4(ImGuiCol idx) {
    return GetStyleColorVec4(static_cast<AppCol>(idx));
}

namespace detail {
// Storage for the custom (>= ImGuiStyleVar_COUNT) style-var slots only.
// Uniform ImVec2: float vars use .x; ImVec2 vars (e.g. NodePadding) use .x/.y.
inline ImVec2 gCustomVars[static_cast<int>(AppVar_COUNT) - static_cast<int>(ImGuiStyleVar_COUNT)] = {};
} // namespace detail

// Scalar accessor (.x for the slot). For indices below ImGuiStyleVar_COUNT this
// reads the live ImGui style via the GetStyleVarInfo metadata table; for
// ofs-specific indices it reads gCustomVars.
inline float GetStyleVar(AppVar idx) {
    if (idx < static_cast<AppVar>(ImGuiStyleVar_COUNT)) {
        const ImGuiStyleVarInfo *info = ImGui::GetStyleVarInfo(static_cast<ImGuiStyleVar>(idx));
        return static_cast<const float *>(info->GetVarPtr(&ImGui::GetStyle()))[0];
    }
    return detail::gCustomVars[static_cast<int>(idx) - static_cast<int>(ImGuiStyleVar_COUNT)].x;
}

inline float GetStyleVar(ImGuiStyleVar idx) {
    return GetStyleVar(static_cast<AppVar>(idx));
}

// Full vec2 accessor. Valid for ImVec2 slots (ImGui vec2 vars and custom vec2
// slots such as AppVar_NodePadding).
inline const ImVec2 &GetStyleVarVec2(AppVar idx) {
    if (idx < static_cast<AppVar>(ImGuiStyleVar_COUNT)) {
        const ImGuiStyleVarInfo *info = ImGui::GetStyleVarInfo(static_cast<ImGuiStyleVar>(idx));
        return *static_cast<const ImVec2 *>(info->GetVarPtr(&ImGui::GetStyle()));
    }
    return detail::gCustomVars[static_cast<int>(idx) - static_cast<int>(ImGuiStyleVar_COUNT)];
}

struct Theme {
    std::string name;                   // display name + file stem
    bool isDark = true;                 //
    ImU32 seed = 0;                     // M3 seed the base theme derives from (0 = hand-built)
    int version = kThemeSchemaVersion;  //
    ImColor colors[AppCol_COUNT] = {};  // ImGuiCol (0…) + AppCol custom slots
    ImVec2 vars[AppVar_COUNT] = {};     // ImGuiStyleVar (0…) + AppVar custom slots; float vars use .x
    ImNodesStyle nodes = {};            // full imnodes Col[] + geometry, owned verbatim
    ImGradient heatmapColors;           // speed → color (slow=0 … fast=1); line segments + heatmap shader
    float heatmapMaxSpeed = 400.0f;     // speed (pos-units/s) that maps to gradient t=1
    float backgroundAxisOpacity = 0.5f; // opacity of non-active axes drawn behind the timeline script line
};

// Applies a theme: writes ImGui colors+vars, App colors+vars, and the global
// imnodes style into the live contexts; rebuilds the heatmap LUT; caches the
// theme as the active one (getActive()); then invokes the post-apply hook.
// Writes UNSCALED (1× DPI) values — DPI scaling is the post-apply hook's job.
void apply(const Theme &theme);

// Post-apply hook: invoked at the tail of every apply(). Application sets this to
// re-capture the themed-unscaled style as the DPI base and re-scale.
void setPostApplyHook(std::function<void()> hook);

// Populate dst with dark/light defaults (colors + vars + nodes + gradient).
void makeDarkTheme(Theme *dst);
void makeLightTheme(Theme *dst);

// Returns the currently applied theme for in-place editing.
// After changing fields call apply() then save() to persist.
Theme &getActive();

// Lazily-built default themes (parsed from the shipped base JSON in the assets
// archive; see ofs::res). Used by the editor for per-field "reset to default".
// Lazy (not file-scope constants) to keep theme construction off static init.
const Theme &defaultDark();
const Theme &defaultLight();

// ── Named themes ─────────────────────────────────────────────────────────────
struct ThemeInfo {
    std::string name;
    bool shipped = false; // shipped base themes are read-only
    bool isDark = true;
};

// Shipped base themes ("Dark", "Light") + user themes under <pref>/themes/*.json.
std::vector<ThemeInfo> list();

// Load a theme by name: a shipped name resolves to the shipped JSON; otherwise the
// user file <pref>/themes/<name>.json. Returns false (dst unchanged) if not found.
bool load(std::string_view name, Theme *dst);

// Write a theme to <pref>/themes/<name>.json. Returns false on error.
bool saveAs(const Theme &theme, std::string_view name);

// Delete a user theme file. Shipped themes cannot be removed. Returns true if removed.
bool remove(std::string_view name);

// Export a theme to an arbitrary path (for sharing). Writes the same versioned JSON
// schema as the per-name user files. Returns false on IO/serialization error.
bool exportToFile(const Theme &theme, const std::filesystem::path &dest);

// Import a theme JSON from an arbitrary path into <pref>/themes/ so it shows up in
// list()/the dropdown. The installed name comes from the file's "name" field (or the
// file stem when absent), de-collided against the read-only shipped names. Returns the
// installed name on success, or std::nullopt on parse/IO error.
std::optional<std::string> importFromFile(const std::filesystem::path &src);

// Persist the active theme to its own user file (no-op for shipped read-only themes).
void save();

} // namespace ofs::theme
