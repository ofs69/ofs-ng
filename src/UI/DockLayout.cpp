#include "UI/DockLayout.h"
#include "Format/AppSettings.h" // kDefaultFontSizeBase — the font size the split ratios are authored against
#include "imgui.h"
#include "imgui_internal.h" // DockBuilder* API — not part of the public imgui.h API.
#include <algorithm>

namespace ofs::ui {

namespace {
// The split ratios below were authored at 100% DPI and the reference font size (kDefaultFontSizeBase).
// The viewport is a fixed pixel count (see the DPI model in Application: io.DisplaySize is physical
// pixels, and DPI is applied as a content scale via style.FontScaleDpi + ScaleAllSizes), so a constant
// ratio yields a constant pixel slot while the text and widgets inside it grow by the DPI scale *and*
// by the user's chosen font size. A panel sized to hold fixed content (a text column, a compact widget
// strip) therefore clips at higher DPI or a larger font. Scale such a panel's ratio by the combined UI
// scale so it keeps roughly the content capacity it was authored for; clamp so an extreme scale can't
// starve the flexible video/timeline area. Flexible panels (video, timeline canvas, simulator view)
// hold resolution-driven content, not text, so they keep their bare authored ratio.
float fixedPanelRatio(float baseRatio, float uiScale, float maxRatio) {
    return std::clamp(baseRatio * uiScale, baseRatio, maxRatio);
}

// Combined UI scale relative to the authored baseline: DPI content scale times the font size relative
// to the reference. Drives the fixed-panel sizing so the default layout adapts to both DPI and the
// user's font-size preference. Mirrors OfsApp::layoutContentScale (which feeds saved-layout scaling),
// reading the live style instead of AppSettings — the two are kept in lockstep each frame.
float layoutUiScale() {
    const ImGuiStyle &style = ImGui::GetStyle();
    float dpi = style.FontScaleDpi;
    if (dpi <= 0.f)
        dpi = 1.f;
    const float fontFactor = style.FontSizeBase > 0.f ? style.FontSizeBase / ofs::kDefaultFontSizeBase : 1.f;
    return dpi * fontFactor;
}

// Discards any existing arrangement and builds the hardcoded default split layout.
void buildDefault(ImGuiID dockspaceId, ImGuiViewport *viewport) {
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->Size);

    // The live content scale combines DPI (FontScaleDpi, set each frame from the display scale) with the
    // user's font size; it is already current by the time this runs (first frame / explicit reset).
    const float uiScale = layoutUiScale();

    ImGuiID dockMainId = dockspaceId;
    ImGuiID dockIdBottom = ImGui::DockBuilderSplitNode(dockMainId, ImGuiDir_Down, 0.30f, nullptr, &dockMainId);
    // The right column holds text-heavy panels (Statistics label/value rows, Tool Options); its width
    // must grow with the content scale or those rows clip. Cap at half the viewport so the video keeps
    // the larger share even at very high DPI / font size.
    ImGuiID dockIdRight = ImGui::DockBuilderSplitNode(dockMainId, ImGuiDir_Right, fixedPanelRatio(0.2f, uiScale, 0.5f),
                                                      nullptr, &dockMainId);
    // The right column is split into three rows, top to bottom: Statistics, Tool Options, and the
    // Simulator. Peel each off the bottom in turn so the remaining top portion keeps splitting: Simulator
    // (bottom quarter), then Tool Options (a slim middle band), leaving Statistics with the top.
    ImGuiID dockIdStatistics = dockIdRight;
    ImGuiID dockIdSimulator =
        ImGui::DockBuilderSplitNode(dockIdStatistics, ImGuiDir_Down, 0.35f, nullptr, &dockIdStatistics);
    ImGuiID dockIdToolOptions =
        ImGui::DockBuilderSplitNode(dockIdStatistics, ImGuiDir_Down, 0.55f, nullptr, &dockIdStatistics);
    ImGuiID dockIdTimeline = dockIdBottom;
    // The Video Controls window holds only two compact widget rows (transport/seek/time +
    // volume/bookmarks/speed), so it needs far less height than the timeline above it. With the
    // layout locked (the intended clean presentation) the node sheds its tab bar, so both rows —
    // the second carries the bookmark/chapter band — fit in this slim slot. The rows are fixed-height
    // (frame height), so the slot must grow with the content scale or the second row clips at high DPI.
    ImGuiID dockIdTimelineControls = ImGui::DockBuilderSplitNode(
        dockIdTimeline, ImGuiDir_Down, fixedPanelRatio(0.28f, uiScale, 0.6f), nullptr, &dockIdTimeline);

    // The Processing panel renders into this same node via the "###video_player" shared id slug, so
    // only one window is ever docked here — one window per node keeps a hidden tab bar from being reset.
    ImGui::DockBuilderDockWindow("Video Player###video_player", dockMainId);
    ImGui::DockBuilderDockWindow("Timeline###timeline", dockIdTimeline);
    ImGui::DockBuilderDockWindow("Video Controls###video_controls", dockIdTimelineControls);
    ImGui::DockBuilderDockWindow("Statistics###statistics", dockIdStatistics);
    ImGui::DockBuilderDockWindow("Tool Options###tool_options", dockIdToolOptions);
    ImGui::DockBuilderDockWindow("Simulator###Simulator", dockIdSimulator);
    ImGui::DockBuilderFinish(dockspaceId);
}

// Per-node chrome hidden while locked: the whole tab/title bar (NoTabBar), the top-left
// window/docking-menu triangle, and the close button — a clean chrome-less look. These are *local*
// node flags (LocalFlagsTransferMask_), so the dockspace's shared flags don't reach leaf nodes — we
// set them on every node directly. They live in SavedFlagsMask_, so a saved layout may bake them in,
// but re-applying every frame from the live lock state overrides that.
constexpr ImGuiDockNodeFlags kLockedNodeFlags =
    ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoWindowMenuButton | ImGuiDockNodeFlags_NoCloseButton;

void applyLockToAllNodes(bool locked) {
    ImGuiContext &g = *ImGui::GetCurrentContext();
    for (int i = 0; i < g.DockContext.Nodes.Data.Size; ++i) {
        auto *node = static_cast<ImGuiDockNode *>(g.DockContext.Nodes.Data[i].val_p);
        if (node == nullptr)
            continue;
        if (locked)
            node->LocalFlags |= kLockedNodeFlags;
        else
            node->LocalFlags &= ~kLockedNodeFlags;
        node->UpdateMergedFlags();
    }
}
} // namespace

void beginDockspace(bool locked) {
    ImGuiID dockspaceId = ImGui::GetID("MyDockSpace");
    ImGuiViewport *viewport = ImGui::GetMainViewport();

    if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr)
        buildDefault(dockspaceId, viewport);

    // Enforce the locked chrome on every node before DockSpace() folds LocalFlags into MergedFlags.
    applyLockToAllNodes(locked);

    // AutoHideTabBar is a shared dockspace flag (SharedFlagsInheritMask_ is ~0, so every child node
    // inherits it): any node holding a single window drops its tab bar, leaving only a small corner
    // triangle to reveal it for undocking. This keeps the unlocked layout as clean and space-efficient
    // as the locked one — a slim slot (e.g. the Video Controls strip) no longer loses a row to a tab
    // bar it doesn't need. When locked, the per-node NoTabBar from applyLockToAllNodes takes over.
    ImGuiDockNodeFlags flags = ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_AutoHideTabBar;
    if (locked)
        // NoUndocking pins docked windows in place; NoDocking (a shared flag inherited by every child
        // node) stops floating windows from being dropped into the layout — together the arrangement is
        // fully frozen. The two live in different enums (public imgui.h vs. imgui_internal.h), so cast
        // both to the ImGuiDockNodeFlags integer type to avoid a cross-enum-OR clang-tidy error.
        flags |= static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoUndocking) |
                 static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoDocking);
    ImGui::DockSpaceOverViewport(dockspaceId, viewport, flags);

    // HACK (re-evaluate on every imgui upgrade): DockSpace() hardcodes its dock-node host window flags
    // (imgui.cpp: "ChildWindow | DockNodeHost | …") with no native way to add NoNavInputs — neither the
    // window class, the dock-node flags, nor any SetNextWindowFlags reaches them. Dragging a dock-node
    // splitter focuses such a host; without NoNavInputs it then keeps io.NavActive — hence
    // io.WantCaptureKeyboard (imgui.cpp: "io.NavActive && NavEnableKeyboard") — true after the drag ends,
    // silently swallowing the app's keyboard shortcuts (the BindingSystem keyboardCaptured gate) until the
    // user clicks elsewhere. The splitter's FocusWindow runs inside DockSpaceOverViewport above, so
    // g.NavWindow is the focused host by the time we get here: forcing NoNavInputs onto it makes the next
    // NavUpdate zero io.NavActive (`!(NavWindow->Flags & NoNavInputs)`). Hosts carry only mouse-driven
    // splitters/tab bars and don't propagate flags to the windows docked inside them, so this is safe. A
    // native opt-out in a future imgui would replace this.
    ImGuiContext &g = *ImGui::GetCurrentContext();
    if (g.NavWindow != nullptr && (g.NavWindow->Flags & ImGuiWindowFlags_DockNodeHost))
        g.NavWindow->Flags |= ImGuiWindowFlags_NoNavInputs;
}

void applyDefaultLayout() {
    buildDefault(ImGui::GetID("MyDockSpace"), ImGui::GetMainViewport());
}

void applyLayoutIni(const std::string &ini, float scaleFactor) {
    // ApplyAll runs synchronously inside LoadIniSettingsFromMemory and builds the live dock nodes from
    // the parsed settings, so their SizeRefs exist to scale by the time it returns.
    ImGui::LoadIniSettingsFromMemory(ini.data(), ini.size());
    if (scaleFactor <= 0.f || scaleFactor == 1.f)
        return;

    // Scale every node's reference size. DockNodeTreeUpdatePosSize honors a split child's SizeRef
    // absolutely when its sibling's subtree holds the central node, handing the central side the
    // remainder (imgui.cpp, "use explicit size from the other, and remainder for the central node"). So
    // scaling uniformly grows the peripheral panels by the factor while the central editor absorbs the
    // slack — it is not the no-op that uniform scaling would be without a central node. Splits with no
    // central node on either side distribute by ratio, which a uniform scale leaves unchanged (the
    // intended behavior: a panel column's internal proportions are preserved as the column resizes).
    ImGuiContext &g = *ImGui::GetCurrentContext();
    for (int i = 0; i < g.DockContext.Nodes.Data.Size; ++i) {
        auto *node = static_cast<ImGuiDockNode *>(g.DockContext.Nodes.Data[i].val_p);
        if (node == nullptr)
            continue;
        node->SizeRef.x *= scaleFactor;
        node->SizeRef.y *= scaleFactor;
    }
}

} // namespace ofs::ui
