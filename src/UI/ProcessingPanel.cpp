#include "ProcessingPanel.h"
#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/GraphPresetEvents.h"
#include "Core/ProcessingRegion.h"
#include "Core/ScriptProject.h"
#include "Core/StandardAxis.h"
#include "Localization/AxisNames.h"
#include "Localization/Translator.h"
#include "Services/EffectRegistry.h"
#include "Services/NodeCategory.h"
#include "Services/ScriptHeader.h"
#include "Services/ScriptNodeEvents.h"
#include "Services/ScriptRegistry.h"
#include "UI/AxisColors.h"
#include "UI/Glyphs.h"
#include "UI/Icons.h"
#include "UI/ImGuiHelpers.h"
#include "UI/Modals.h"
#include "UI/ProcessingGraphOps.h"
#include "Util/FileUtil.h"
#include "Util/FrameAllocator.h"
#include "Util/FuzzyMatch.h"
#include "Util/PathUtil.h"
#include "Util/Resources.h"
#include "Util/TimeUtil.h"
#include "imgui.h"
#include "imgui_internal.h" // GetCurrentWindow — to clear NoNavInputs on the imnodes canvas child
#include "imgui_stdlib.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <functional>
#include <imnodes.h>
#include <optional>
#include <string_view>
#include <unordered_set>

namespace ofs {

// The pin cap must fit GraphId's slot field, or a node's higher pins would alias the next owner's ids.
// Pinned here (the only TU that sees both constants) so raising OFS_MAX_NODE_PINS past 63 fails the build.
static_assert(OFS_MAX_NODE_PINS <= GraphId::kMaxSlot, "OFS_MAX_NODE_PINS exceeds GraphId slot capacity");

ProcessingPanel::ProcessingPanel() {
    m_editorCtx = ImNodes::EditorContextCreate();
}

ProcessingPanel::~ProcessingPanel() {
    if (m_editorCtx)
        ImNodes::EditorContextFree(m_editorCtx);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

// Resolve a user-typed script name during a modal's per-frame render: trim surrounding
// whitespace, flag invalid path characters, and ensure a single ".cs" extension. The
// resolved name is written into the frame arena (valid until the next FrameAllocator::reset),
// so the render path allocates no heap strings.
struct ResolvedScriptName {
    const char *finalName; // trimmed name + ".cs", in the frame arena ("" when empty)
    bool empty;
    bool badChars;
};

static ResolvedScriptName resolveScriptName(std::string_view raw) {
    std::string_view t = raw;
    const auto first = t.find_first_not_of(" \t");
    t = (first == std::string_view::npos) ? std::string_view{} : t.substr(first, t.find_last_not_of(" \t") - first + 1);
    const bool badChars = t.find_first_of("/\\:*?\"<>|") != std::string_view::npos;
    const bool empty = t.empty();
    auto endsWithCs = [](std::string_view s) {
        return s.size() >= 3 && s[s.size() - 3] == '.' &&
               std::tolower(static_cast<unsigned char>(s[s.size() - 2])) == 'c' &&
               std::tolower(static_cast<unsigned char>(s[s.size() - 1])) == 's';
    };
    const char *finalName = empty ? "" : (endsWithCs(t) ? fmtScratch("{}", t) : fmtScratch("{}.cs", t));
    return {.finalName = finalName, .empty = empty, .badChars = badChars};
}

// Localized verb + the symbolic formula. The (A + B) notation and the operand letters stay literal —
// they're math symbols (matching the node's A/B pins), not translatable words. Frame-arena result.
static const char *mathNodeLabel(GraphNodeType t) {
    switch (t) {
    case GraphNodeType::Add:
        return fmtScratch("{}  (A + B)", Str::ProcMathAdd.sv());
    case GraphNodeType::Subtract:
        return fmtScratch("{}  (A " GLYPH_MINUS " B)", Str::ProcMathSubtract.sv());
    case GraphNodeType::Multiply:
        return fmtScratch("{}  (A " GLYPH_MULTIPLY " B)", Str::ProcMathMultiply.sv());
    case GraphNodeType::Divide:
        return fmtScratch("{}  (A " GLYPH_DIVIDE " B)", Str::ProcMathDivide.sv());
    case GraphNodeType::Constant:
        return Str::ProcNodeConstant.c_str();
    default:
        return "";
    }
}

// Stable, language-independent ###id suffix for a math add-menu item, so the translated label can't
// change the widget identity (and the UI tests can target it by id regardless of UI language).
static const char *mathNodeMenuId(GraphNodeType t) {
    switch (t) {
    case GraphNodeType::Add:
        return "procadd_add";
    case GraphNodeType::Subtract:
        return "procadd_sub";
    case GraphNodeType::Multiply:
        return "procadd_mul";
    case GraphNodeType::Divide:
        return "procadd_div";
    case GraphNodeType::Constant:
        return "procadd_const";
    default:
        return "procadd_x";
    }
}

// The native nodes offered under the add-menu's Math group, in display order. mathNodeLabel/mathNodeIcon
// are the single source for their text/glyph (shared with the node-title renderer).
inline constexpr GraphNodeType kMathMenuTypes[] = {GraphNodeType::Add, GraphNodeType::Subtract, GraphNodeType::Multiply,
                                                   GraphNodeType::Divide, GraphNodeType::Constant};

// ── Node category presentation ───────────────────────────────────────────────
// Label and icon for an add-menu grouping bucket (the single source of truth — effects, scripts and
// plugin nodes all route through these, so the same word and glyph read in the picker and on the placed
// node's title bar). Generators synthesize a signal; modifiers reshape one input; combiners merge two
// or more. The enum is exhaustive, so there is no stringly-typed fallback to drift out of sync.
static const char *nodeCategoryLabel(NodeCategory cat) {
    switch (cat) {
    case NodeCategory::Generate:
        return Str::ProcCatGenerate.c_str();
    case NodeCategory::Modify:
        return Str::ProcCatModify.c_str();
    case NodeCategory::Combine:
        return Str::ProcCatCombine.c_str();
    }
    return "";
}

static const char *nodeCategoryIcon(NodeCategory cat) {
    switch (cat) {
    case NodeCategory::Generate:
        return ICON_AUDIO_WAVEFORM;
    case NodeCategory::Modify:
        return ICON_SLIDERS_HORIZONTAL;
    case NodeCategory::Combine:
        return ICON_CALCULATOR; // two or more signals in, one out
    }
    return ICON_BOX;
}

// Glyph for a curated plugin-node icon value (the public NodeIcon enum, mirrored as OfsNodeIcon). This is
// the host's half of the by-value icon contract: a plugin picks an enum, the renderer owns the glyph, so
// re-skinning a node icon never touches the ABI. An unknown (newer) value falls through to the box glyph.
static const char *nodeIconGlyph(OfsNodeIcon icon) {
    switch (icon) {
    case OfsNodeIconDefault:
        return ICON_BOX; // unreachable in practice — pluginNodeIcon resolves Default to the arity icon
    case OfsNodeIconWaveform:
        return ICON_AUDIO_WAVEFORM;
    case OfsNodeIconSliders:
        return ICON_SLIDERS_HORIZONTAL;
    case OfsNodeIconFilter:
        return ICON_FILTER;
    case OfsNodeIconCurve:
        return ICON_SPLINE;
    case OfsNodeIconActivity:
        return ICON_ACTIVITY;
    case OfsNodeIconGauge:
        return ICON_GAUGE;
    case OfsNodeIconMath:
        return ICON_SIGMA;
    case OfsNodeIconFunction:
        return ICON_FUNCTION_SQUARE;
    case OfsNodeIconMerge:
        return ICON_GIT_MERGE;
    case OfsNodeIconSplit:
        return ICON_SPLIT;
    case OfsNodeIconBlend:
        return ICON_BLEND;
    case OfsNodeIconCombine:
        return ICON_COMBINE;
    case OfsNodeIconScale:
        return ICON_SCALING;
    case OfsNodeIconRuler:
        return ICON_RULER;
    case OfsNodeIconMagnet:
        return ICON_MAGNET;
    case OfsNodeIconPercent:
        return ICON_PERCENT;
    case OfsNodeIconRepeat:
        return ICON_REPEAT;
    case OfsNodeIconShuffle:
        return ICON_SHUFFLE;
    case OfsNodeIconRandom:
        return ICON_DICES;
    case OfsNodeIconWand:
        return ICON_WAND;
    case OfsNodeIconTrend:
        return ICON_TRENDING_UP;
    case OfsNodeIconZap:
        return ICON_ZAP;
    case OfsNodeIconBars:
        return ICON_BAR_CHART;
    case OfsNodeIconMove:
        return ICON_MOVE_VERTICAL;
    }
    return ICON_BOX;
}

// Leaf glyph for a plugin node: the author-declared icon when it set one, else the arity-bucket icon so an
// undeclared node still reads as Generate/Modify/Combine.
static const char *pluginNodeIcon(const PluginNodeEntry &pn) {
    return pn.icon == OfsNodeIconDefault ? nodeCategoryIcon(nodeCategoryForInputs(pn.inputCount))
                                         : nodeIconGlyph(pn.icon);
}

// Add-menu glyph distinguishing a shipped first-party library script from a user (file-backed,
// editable) script at a glance — the hover tooltip still spells out which kind, but the icon makes
// the user's own scripts identifiable in the list without hovering.
static const char *scriptMenuIcon(bool library) {
    return library ? ICON_LIBRARY : ICON_BRACES;
}

static const char *mathNodeIcon(GraphNodeType t) {
    switch (t) {
    case GraphNodeType::Add:
        return ICON_PLUS;
    case GraphNodeType::Subtract:
        return ICON_MINUS;
    case GraphNodeType::Multiply:
        return ICON_X;
    case GraphNodeType::Divide:
        return ICON_SLASH;
    case GraphNodeType::Constant:
        return ICON_HASH;
    default:
        return ICON_CALCULATOR;
    }
}

// Hover/selected emphasis on a dynamic base color (a node category / link color that varies per
// node, so it can't be a static theme entry). Overlays the surface text color (onSurface) at a
// fixed opacity — the same Material state-layer the theme generator bakes into every static
// hover/selected slot (_state(base, "onSurface", op)). It lifts contrast in BOTH schemes
// (lightening on dark, darkening on light) without a per-theme branch, unlike a multiplicative
// brighten which over-shoots the deep dark-theme bases.
static ImU32 stateLayer(ImU32 base, float opacity) {
    const ImVec4 b = ImGui::ColorConvertU32ToFloat4(base);
    const ImVec4 &on = ofs::theme::GetStyleColorVec4(ImGuiCol_Text);
    return ImGui::ColorConvertFloat4ToU32(
        ImVec4(b.x + (on.x - b.x) * opacity, b.y + (on.y - b.y) * opacity, b.z + (on.z - b.z) * opacity, b.w));
}

// ── Script-file helpers ─────────────────────────────────────────────────────────

static std::filesystem::path scriptsDirPath() {
    auto dir = ofs::util::getPrefPath() / "scripts";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// Archive prefix of the shipped script library packed into data.pak (read through ofs::res).
static constexpr std::string_view kScriptLibPrefix = "data/scripts/lib/";

// On-disk path of a user script under <pref>/scripts, iff it exists. The shipped library lives in
// data.pak (ofs::res), not on disk, so a library-only name returns {} — only a user copy of the same
// name (a fork) has a path, which is what makes a node editable. Mirrors ScriptSystem::userScriptPath.
static std::filesystem::path resolveScriptPath(const std::string &fileName) {
    if (fileName.empty())
        return {};
    auto user = scriptsDirPath() / ofs::util::fromUtf8(fileName);
    std::error_code ec;
    if (std::filesystem::exists(user, ec))
        return user;
    return {};
}

// Read just the // !ofs: header for a script name (used when a file is picked, to set the node's
// signal/inputCount caches and thus its pin layout before the async compile completes). A user file
// shadows the shipped library packed in data.pak.
static ScriptHeader readScriptHeaderFile(const std::string &fileName) {
    if (fileName.empty())
        return {};
    if (auto path = resolveScriptPath(fileName); !path.empty()) {
        if (auto src = ofs::util::readFile(path))
            return parseScriptHeader(*src);
        return {};
    }
    if (auto src = ofs::res::readText(std::string(kScriptLibPrefix) + fileName))
        return parseScriptHeader(*src);
    return {};
}

// Scan the user scripts folder and the shipped library (packed in data.pak), parse each header, and
// produce the add-menu catalog. A user file shadows a same-named library file (the fork). Sorted by
// display name.
static void buildScriptCatalog(std::vector<ScriptCatalogEntry> &out) {
    out.clear();
    std::unordered_set<std::string> seen; // base names already listed (user shadows library)
    auto add = [&](std::string name, const std::string &src, bool library) {
        if (!seen.insert(name).second)
            return;
        const ScriptHeader h = parseScriptHeader(src);
        ScriptCatalogEntry e;
        e.displayName = h.name.empty() ? ofs::util::toUtf8(ofs::util::fromUtf8(name).stem()) : h.name;
        e.fileName = std::move(name);
        e.description = h.description;
        e.signal = static_cast<uint8_t>(h.signal);
        e.inputCount = h.inputCount();
        e.outputCount = h.outputCount();
        e.library = library;
        out.push_back(std::move(e));
    };
    // User folder first so a user file shadows a same-named library script (the fork).
    std::error_code ec;
    for (const auto &de : std::filesystem::directory_iterator(scriptsDirPath(), ec))
        if (de.is_regular_file() && de.path().extension() == ".cs")
            if (auto src = ofs::util::readFile(de.path()))
                add(ofs::util::toUtf8(de.path().filename()), *src, /*library=*/false);
    // Shipped library: archive entries look like "data/scripts/lib/Sine.cs".
    for (const auto &res : ofs::res::list(kScriptLibPrefix)) {
        std::string_view base = res;
        base.remove_prefix(kScriptLibPrefix.size());
        if (!base.ends_with(".cs"))
            continue;
        if (auto src = ofs::res::readText(res))
            add(std::string(base), *src, /*library=*/true);
    }
    std::ranges::sort(
        out, [](const ScriptCatalogEntry &a, const ScriptCatalogEntry &b) { return a.displayName < b.displayName; });
}

// Create a stub script file whose // !ofs: header matches the requested signal and input count, with
// a starter Eval body for that shape (the in-scope names match the wrapper in ScriptCompiler.cs).
// Returns false if the file already exists (caller should pick a different name) or on write failure.
static bool createScriptStub(const std::string &fileName, OfsSignalKind signal, int inputCount, int outputCount,
                             bool comments, std::string_view displayName, std::string_view description) {
    auto path = scriptsDirPath() / ofs::util::fromUtf8(fileName);
    std::error_code ec;
    if (std::filesystem::exists(path, ec))
        return false;
    return ofs::util::writeFile(path,
                                scriptStubText(signal, inputCount, comments, displayName, description, outputCount));
}

static bool syncPositions(ProcessingNodeGraph &graph) {
    bool changed = false;
    for (auto &node : graph.nodes) {
        auto pos = ImNodes::GetNodeEditorSpacePos(GraphId::nodeBody(node.id));
        if (std::abs(pos.x - node.posX) > 0.5f || std::abs(pos.y - node.posY) > 0.5f) {
            node.posX = pos.x;
            node.posY = pos.y;
            changed = true;
        }
    }
    return changed;
}

// Drop a link by id. Shared by the destroyed-link handler, the link context menu, and deleteSelected.
static void removeLink(ProcessingNodeGraph &graph, int linkId) {
    auto &links = graph.links;
    links.erase(std::ranges::remove_if(links, [linkId](const auto &l) { return l.id == linkId; }).begin(), links.end());
}

// Drop a node and every link touching it. Shared by the node context menu and deleteSelected. Input/
// Output nodes are the region's fixed endpoints; callers guard against deleting them.
static void removeNodeAndLinks(ProcessingNodeGraph &graph, int nodeId) {
    auto &links = graph.links;
    links.erase(
        std::ranges::remove_if(links, [nodeId](const auto &l) { return l.fromNode == nodeId || l.toNode == nodeId; })
            .begin(),
        links.end());
    auto &nodes = graph.nodes;
    nodes.erase(std::ranges::remove_if(nodes, [nodeId](const auto &nd) { return nd.id == nodeId; }).begin(),
                nodes.end());
}

// ── Node rendering ─────────────────────────────────────────────────────────────

// Node-param column widths, font-relative so they track font/DPI instead of staying raw pixels. These
// size the imnodes node body, whose param labels come from registry/plugin data
// (kept English for now), so a longer translation can't clip here — the conversion is purely for DPI.
static float nodeWidgetW() {
    return ImGui::GetFontSize() * 7.5f;
} // ≈120 px at the 16 px reference
static float nodeLabelW() {
    return ImGui::GetFontSize() * 6.0f;
} // ≈96 px at the 16 px reference

// The fixed inner content width every node body anchors to (the param-table width). Every node either
// fills this with a 2-column table or a spacer Dummy, and every output label right-aligns to it, so a
// node's width is a pure function of this font-relative constant — DPI-tracking, but never derived from
// the node's own measured size.
//
// Crucially the right-align target must NOT come from ImNodes::GetNodeDimensions() (the previous frame's
// measured rect). Positioning a node's content using its own prior width makes the width monotonic: it
// can only hold or grow, never shrink. On-screen that is a stable fixed point, but dragging a node off
// the canvas edge auto-pans the camera and pushes the *other* nodes off the LEFT edge, into negative
// window-relative coordinates — where ImGui's line-start rounding (IM_TRUNC) truncates toward zero
// instead of flooring, so each frame measures the label a little wider, the feedback latches it in, and
// the node grows without bound (Math nodes worst, as their narrow title lets the output drive width).
// Anchoring to an intrinsic constant breaks the loop entirely.
static float nodeContentW() {
    return nodeLabelW() + nodeWidgetW();
}

static bool beginNodeParamTable(const char *id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_None, {nodeContentW(), 0.f}))
        return false;
    ImGui::TableSetupColumn("##L", ImGuiTableColumnFlags_WidthFixed, nodeLabelW());
    ImGui::TableSetupColumn("##R", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

// formRow for a node param whose label is data-driven (an effect/script param name) and can exceed the
// fixed label column. Elide with "…" and surface the full name on hover, rather than letting the column
// silently clip it. Native param rows (short, localized) keep plain formRow.
static void nodeParamLabelRow(const char *label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    const float avail = nodeLabelW() - ImGui::GetStyle().CellPadding.x * 2.0f;
    const char *shown = ofs::ui::elide(label, avail);
    ImGui::TextUnformatted(shown);
    if (shown != label && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", label);
    ImGui::TableNextColumn();
}

// Right-align an output-pin label to the node's right content edge. imnodes lays pins out left-aligned,
// so without this the label floats near the left. The indent floors to 1px when the label already fills
// the content width: ImGui::Indent(0) would apply the default IndentSpacing instead of no indent.
static void renderRightAlignedOutput(int nodeId, int slot, const char *label, float contentW) {
    ImNodes::BeginOutputAttribute(GraphId::outPin(nodeId, slot));
    ImGui::Indent(std::max(1.0f, contentW - ImGui::CalcTextSize(label).x));
    ImGui::TextUnformatted(label);
    ImNodes::EndOutputAttribute();
}

static void renderRightAlignedOutput(int nodeId, int slot, const char *label) {
    renderRightAlignedOutput(nodeId, slot, label, nodeContentW());
}

static void renderOutputPin(int nodeId) {
    renderRightAlignedOutput(nodeId, 0, Str::ProcPinOut.c_str());
}

// Resolve the label for pin slot `k`: the declared pin name, or a "<prefix>{k}" index fallback when the
// def is absent / unnamed. The returned pointer is frame-arena allocated for the fallback case.
static const char *pinLabel(const std::vector<std::string> &names, int k, const char *prefix) {
    if (k < static_cast<int>(names.size()) && !names[static_cast<size_t>(k)].empty())
        return names[static_cast<size_t>(k)].c_str();
    return fmtScratch("{}{}", prefix, k);
}

// Render the output attribute as a row that also carries left-aligned content (drawn by `left`),
// keeping the "out" label flush to the node's right edge so it still sits beside the output pin.
// Used by the Script node to fold its action buttons and compile-status indicator onto the pin row
// instead of stacking them as separate full-width rows above it. Safe to host buttons here: imnodes
// only starts a link drag when the cursor is within PinHoverRadius of the pin (the node's right
// edge), and the left content renders far from it.
template <typename F> static void renderOutputPinRow(int nodeId, const char *label, F &&left) {
    ImNodes::BeginOutputAttribute(GraphId::outPin(nodeId, 0));
    const float rowStartX = ImGui::GetCursorPosX();
    left();
    ImGui::SameLine();
    const float target = rowStartX + nodeContentW() - ImGui::CalcTextSize(label).x;
    if (target > ImGui::GetCursorPosX())
        ImGui::SetCursorPosX(target);
    ImGui::TextUnformatted(label);
    ImNodes::EndOutputAttribute();
}

// Compact compile-status indicator: a single glyph stating the node's compile result at a glance.
// The error case is a button — clicking it raises a modal with the full compiler text, so a long
// error never has to wrap inside the cramped node body. `noScript` short-circuits to the idle hint.
static void renderCompileStatus(EventQueue &eq, const ProcessingGraphNode &node, const CompiledScript *cs,
                                bool noScript) {
    if (noScript) {
        ImGui::TextDisabled("%s", Str::ProcNoScriptSelected.c_str());
    } else if (cs == nullptr) {
        // Glyph-only (text in a tooltip) so the compiling state is the same compact width as the
        // resolved states below — a wider transient label would briefly widen the node mid-compile.
        ImGui::TextDisabled("%s", ICON_HOURGLASS);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Str::ProcCompiling.c_str());
    } else if (cs->ref.valid()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ofs::theme::GetStyleColorVec4(AppCol_Success));
        ImGui::TextUnformatted(ICON_CHECK_CIRCLE);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Str::ProcCompileOk.c_str());
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ofs::theme::GetStyleColorVec4(AppCol_Error));
        const bool clicked = ImGui::SmallButton(fmtScratch("{}##err{}", ICON_ALERT_TRIANGLE, node.id));
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Str::ProcViewError.c_str());
        if (clicked) {
            // Capture the text now (cs is transient); the modal body owns its copy and survives the frame.
            std::string err = cs->error.empty() ? std::string(Str::ProcCompileError.c_str()) : cs->error;
            showCustomModal(eq, {.title = std::string(Str::ProcCompileErrorTitle.c_str()),
                                 .severity = ModalSeverity::Error,
                                 .width = ImGui::GetFontSize() * 32.0f,
                                 .body = [text = std::move(err)]() -> bool {
                                     ImGui::BeginChild("##errtext",
                                                       {0.0f, ImGui::GetTextLineHeightWithSpacing() * 12.0f}, true);
                                     ImGui::PushTextWrapPos(0.0f);
                                     ImGui::TextUnformatted(text.c_str());
                                     ImGui::PopTextWrapPos();
                                     ImGui::EndChild();
                                     ImGui::Separator();
                                     return ImGui::Button(Str::ProcClose.id("proc_err_close"));
                                 }});
        }
    }
}

// Push an undo snapshot at the start of a drag gesture. ImGui guarantees IsItemActivated() is true
// only on the mouse-down frame, and DragBehaviorT cannot change the value on that frame (it resets
// the accumulator and returns false when is_just_activated). So a `snapshot = IsItemActivated()`
// read *inside* an `if (DragFloat(...))` block is always false — the value-change frames never
// coincide with activation. This must be checked separately, right after the drag widget. The
// event carries the still-unmodified region purely to mark the pre-edit state; onModifyRegion
// treats an identical region as a no-op, so it costs no re-eval.
static void snapshotOnDragStart(EventQueue &eq, int regionId, const ProcessingRegion &region) {
    if (ImGui::IsItemActivated())
        eq.push(ModifyRegionEvent{.regionId = regionId, .updatedRegion = region, .snapshot = true});
}

// Drop a freshly-built node into `region`'s graph at `screenPos`. The common add-node skeleton: copy the
// region, mint an id, convert the screen drop point into editor-space coords, let `configure` fill the
// node's type/params, auto-connect it to the pending drop pin (`linkPin`, -1 if none; `ignoresInput`
// suppresses the output-splice for a generator with no input), push the ModifyRegion, and pin the imnodes
// screen position. Returns the new node id. The per-type compile/open follow-ups stay with the caller.
static int placeNewNode(EventQueue &eq, const ProcessingRegion &region, int regionId, ImVec2 screenPos, int linkPin,
                        bool ignoresInput, const std::function<void(ProcessingGraphNode &)> &configure) {
    ProcessingRegion updated = region;
    const int nodeId = updated.nodeGraph.allocId();
    const ImVec2 editorPos = ImGui::GetWindowPos();
    ProcessingGraphNode node{.id = nodeId, .posX = screenPos.x - editorPos.x, .posY = screenPos.y - editorPos.y};
    configure(node);
    updated.nodeGraph.nodes.push_back(std::move(node));
    if (linkPin != -1)
        autoConnectNewNode(nodeId, linkPin, ignoresInput, updated.nodeGraph);
    eq.push(ModifyRegionEvent{.regionId = regionId, .updatedRegion = updated});
    ImNodes::SetNodeScreenSpacePos(GraphId::nodeBody(nodeId), screenPos);
    return nodeId;
}

static void renderEffectNodeBody(const ProcessingGraphNode &node, const EffectDefinition *def, EventQueue &eq,
                                 int regionId, const ProcessingRegion &region) {
    ImNodes::BeginStaticAttribute(GraphId::staticAttr(node.id));

    ImGui::TextUnformatted(def ? def->displayName.c_str() : node.effect.type.c_str());

    const bool hasRows = def && !def->paramDefs.empty();
    if (hasRows) {
        if (beginNodeParamTable("##params")) {

            if (def) {
                for (int pi = 0; pi < static_cast<int>(def->paramDefs.size()); ++pi) {
                    const auto &pd = def->paramDefs[static_cast<size_t>(pi)];
                    nodeParamLabelRow(pd.displayName.c_str());
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    const char *wid = fmtScratch("##{}", pd.key);
                    if (pd.type == EffectParamDef::Type::Float) {
                        float val = node.effect.params[static_cast<size_t>(pi)];
                        const bool clamp = pd.max > pd.min; // a real range was declared
                        const ImGuiSliderFlags fl = clamp ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
                        if (ImGui::DragFloat(wid, &val, 0.01f, clamp ? pd.min : 0.0f, clamp ? pd.max : 0.0f, "%.3f",
                                             fl)) {
                            ProcessingRegion updated = region;
                            for (auto &n : updated.nodeGraph.nodes)
                                if (n.id == node.id)
                                    n.effect.params[static_cast<size_t>(pi)] = val;
                            eq.push(
                                ModifyRegionEvent{.regionId = regionId, .updatedRegion = updated, .snapshot = false});
                        }
                    } else {
                        int val = static_cast<int>(node.effect.params[static_cast<size_t>(pi)]);
                        if (ImGui::DragInt(wid, &val, 1.0f, 0, 0)) {
                            ProcessingRegion updated = region;
                            for (auto &n : updated.nodeGraph.nodes)
                                if (n.id == node.id)
                                    n.effect.params[static_cast<size_t>(pi)] = static_cast<float>(val);
                            eq.push(
                                ModifyRegionEvent{.regionId = regionId, .updatedRegion = updated, .snapshot = false});
                        }
                    }
                    snapshotOnDragStart(eq, regionId, region);
                }
            }

            ImGui::EndTable();
        }
    }

    ImNodes::EndStaticAttribute();
}

void ProcessingPanel::renderNode(const ProcessingGraphNode &node, const EffectRegistryState &effectReg,
                                 const ScriptRegistryState &scriptReg, EventQueue &eq, int regionId,
                                 const ProcessingRegion &region, bool hot, bool pending, int &outSaveReqNodeId,
                                 int &outLoadReqNodeId) {
    ImU32 tBase = 0;
    if (node.type == GraphNodeType::Input || node.type == GraphNodeType::Output) {
        tBase = standardAxisColor(node.role);
    } else {
        AppCol titleCol = AppCol_NodeIO;
        if (node.type == GraphNodeType::Effect) {
            auto it = effectReg.effects.find(node.effect.type);
            if (it != effectReg.effects.end())
                titleCol = (it->second.kind == EffectKind::Functional) ? AppCol_LinkFunctional : AppCol_LinkDiscrete;
            else
                titleCol = AppCol_LinkDiscrete;
        } else if (node.type == GraphNodeType::PluginNode) {
            const bool pluginEnabled = effectReg.pluginNodes.contains(node.pluginNodeId);
            if (!pluginEnabled)
                titleCol = AppCol_NodeIO; // gray-ish for disabled
            else
                titleCol = node.pluginSignal == static_cast<uint8_t>(OfsSignalFunctional) ? AppCol_LinkFunctional
                                                                                          : AppCol_LinkDiscrete;
        } else if (node.type == GraphNodeType::Script) {
            const CompiledScript *cs = scriptReg.find(node.scriptFile);
            const bool resolved = cs && cs->ref.valid();
            if (!resolved)
                titleCol = AppCol_NodeIO; // gray when missing / uncompiled / errored
            else
                titleCol = node.scriptSignal == static_cast<uint8_t>(OfsSignalFunctional) ? AppCol_LinkFunctional
                                                                                          : AppCol_LinkDiscrete;
        } else if (isMathNode(node.type) || node.type == GraphNodeType::Constant) {
            titleCol = AppCol_NodeMath;
        } else if (node.type == GraphNodeType::Discretize) {
            titleCol = AppCol_LinkDiscrete; // output is always discrete
        } else if (node.type == GraphNodeType::Functionalize) {
            titleCol = AppCol_LinkFunctional; // output is always functional
        }
        tBase = ofs::theme::GetColorU32(titleCol);
    }
    // Match the generator's default node title-bar state layers (TitleBarHovered 0.10 /
    // TitleBarSelected 0.16), applied to the per-category base color.
    ImNodes::PushColorStyle(ImNodesCol_TitleBar, tBase);
    ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, stateLayer(tBase, 0.10f));
    ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, stateLayer(tBase, 0.16f));

    bool pushedOutline = false;
    if (hot) {
        float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f);
        ImNodes::PushColorStyle(ImNodesCol_NodeOutline,
                                ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.75f, 0.2f, pulse)));
        pushedOutline = true;
    } else if (pending) {
        ImNodes::PushColorStyle(ImNodesCol_NodeOutline,
                                ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.75f, 0.2f, 0.25f)));
        pushedOutline = true;
    }

    ImNodes::SetNodeEditorSpacePos(GraphId::nodeBody(node.id), {node.posX, node.posY});
    ImNodes::BeginNode(GraphId::nodeBody(node.id));

    switch (node.type) {
    case GraphNodeType::Input: {
        // An I/O node has no param body, so anchor the lone output label to the title width rather than
        // the standard body width — that keeps the Input node as compact as its Output-node mirror
        // (which auto-sizes to its title). Title width is intrinsic, so it stays position-stable.
        const char *title = fmtScratch("{}  {} {}", ICON_LOG_IN, standardAxisTag(node.role), Str::ProcPinIn.sv());
        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(title);
        ImNodes::EndNodeTitleBar();
        renderRightAlignedOutput(node.id, 0, Str::ProcPinSignal.c_str(), ImGui::CalcTextSize(title).x);
        break;
    }

    case GraphNodeType::Output: {
        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(fmtScratch("{}  {} {}", ICON_LOG_OUT, standardAxisTag(node.role), Str::ProcPinOut.sv()));
        ImNodes::EndNodeTitleBar();
        ImNodes::BeginInputAttribute(GraphId::inPin(node.id, 0));
        ImGui::TextUnformatted(Str::ProcPinSignal.c_str());
        ImNodes::EndInputAttribute();
        break;
    }

    case GraphNodeType::Effect: {
        auto defIt = effectReg.effects.find(node.effect.type);
        const EffectDefinition *def = (defIt != effectReg.effects.end()) ? &defIt->second : nullptr;

        ImNodes::BeginNodeTitleBar();
        {
            // Category prefix + the effect's own name in brackets, mirroring the Script node's
            // "Script  [name]" so a Denoise/RDP node is identifiable without reading the body.
            const char *catName = def ? nodeCategoryLabel(def->category) : Str::ProcNodeEffect.c_str();
            const char *catIcon = def ? nodeCategoryIcon(def->category) : ICON_BOX;
            const char *dispName = def ? def->displayName.c_str() : node.effect.type.c_str();
            ImGui::TextUnformatted(fmtScratch("{}  {}  [{}]", catIcon, catName, dispName));
        }
        ImNodes::EndNodeTitleBar();

        if (!def || !def->ignoresInput) {
            ImNodes::BeginInputAttribute(GraphId::inPin(node.id, 0));
            ImGui::TextUnformatted(Str::ProcPinIn.c_str());
            ImNodes::EndInputAttribute();
        }

        renderEffectNodeBody(node, def, eq, regionId, region);

        renderOutputPin(node.id);
        break;
    }

    case GraphNodeType::Add:
    case GraphNodeType::Subtract:
    case GraphNodeType::Multiply:
    case GraphNodeType::Divide:
        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(fmtScratch("{}  {}", mathNodeIcon(node.type), mathNodeLabel(node.type)));
        ImNodes::EndNodeTitleBar();

        ImNodes::BeginInputAttribute(GraphId::inPin(node.id, 0));
        ImGui::TextUnformatted("A");
        ImNodes::EndInputAttribute();

        ImNodes::BeginInputAttribute(GraphId::inPin(node.id, 1));
        {
            bool bConnected = region.nodeGraph.findLinkToPin(node.id, 1) != nullptr;
            ImGui::TextUnformatted(bConnected ? "B" : "B  = 50");
        }
        ImNodes::EndInputAttribute();

        ImNodes::BeginStaticAttribute(GraphId::staticAttr(node.id));
        ImGui::Dummy({nodeContentW(), 0.0f});
        ImNodes::EndStaticAttribute();

        renderOutputPin(node.id);
        break;

    case GraphNodeType::PluginNode: {
        const auto *entry = [&]() -> const PluginNodeEntry * {
            auto it = effectReg.pluginNodes.find(node.pluginNodeId);
            return it != effectReg.pluginNodes.end() ? &it->second : nullptr;
        }();
        const bool disabled = (entry == nullptr);
        // Pin counts: from the live def when enabled, else the persisted cache so a disabled plugin
        // still draws the right pins.
        const int inputCount = disabled ? node.pluginInputCount : entry->inputCount;
        const int outputCount = disabled ? node.pluginOutputCount : entry->outputCount;

        ImNodes::BeginNodeTitleBar();
        if (disabled)
            ImGui::TextDisabled("%s  %s", nodeCategoryIcon(nodeCategoryForInputs(inputCount)),
                                node.pluginNodeId.c_str());
        else
            ImGui::TextUnformatted(
                fmtScratch("{}  {}  [{}]", pluginNodeIcon(*entry), entry->displayName, entry->category));
        ImNodes::EndNodeTitleBar();

        // One input pin per declared input, labeled with the def's pin name (index label when the def is
        // absent). An unwired pin notes the 50 fallback the evaluator feeds it.
        for (int i = 0; i < inputCount; ++i) {
            ImNodes::BeginInputAttribute(GraphId::inPin(node.id, i));
            const char *pinName = (entry && i < static_cast<int>(entry->inputNames.size()))
                                      ? entry->inputNames[static_cast<size_t>(i)].c_str()
                                      : fmtScratch("{}{}", Str::ProcPinIn.sv(), i);
            const bool connected = region.nodeGraph.findLinkToPin(node.id, i) != nullptr;
            ImGui::TextUnformatted(connected ? pinName : fmtScratch("{}  = 50", pinName));
            ImNodes::EndInputAttribute();
        }

        ImNodes::BeginStaticAttribute(GraphId::staticAttr(node.id));
        if (disabled) {
            ImGui::TextDisabled("%s", Str::ProcPluginDisabled.c_str());
        } else if (entry->onNodeUi && effectReg.renderNodeUi) {
            // TState node: the managed wrapper runs the node's `ui` callback, drawing its widgets
            // into the 2-column param table here. On a change it writes the node's new state JSON into
            // `working`, which we fold into the region and re-eval through the existing ModifyRegionEvent
            // path.
            m_nodeUiBuffer = node.nodeState; // reuse the member's capacity instead of a per-frame allocation
            bool changed = false;
            if (beginNodeParamTable("##plugui")) {
                changed = effectReg.renderNodeUi(*entry, m_nodeUiBuffer, regionId, node.id);
                ImGui::EndTable();
            }
            if (changed) {
                ProcessingRegion updated = region;
                for (auto &n : updated.nodeGraph.nodes)
                    if (n.id == node.id)
                        n.nodeState = m_nodeUiBuffer;
                eq.push(ModifyRegionEvent{.regionId = regionId, .updatedRegion = updated, .snapshot = false});
            }
            snapshotOnDragStart(eq, regionId, region);
        } else {
            // No editable params (a stateless node carries no onNodeUi hook), so the body would have no
            // content. Anchor it to the standard width so the node matches its peers (see nodeContentW).
            ImGui::Dummy({nodeContentW(), 0.0f});
        }
        ImNodes::EndStaticAttribute();

        // One output pin per declared output, labeled with the def's name (index label when absent).
        static const std::vector<std::string> kNoNames;
        const std::vector<std::string> &outNames = entry ? entry->outputNames : kNoNames;
        for (int k = 0; k < outputCount; ++k)
            renderRightAlignedOutput(node.id, k, pinLabel(outNames, k, Str::ProcPinOut.c_str()));
        break;
    }

    case GraphNodeType::Constant: {
        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(fmtScratch("{}  {}", ICON_HASH, Str::ProcNodeConstant.sv()));
        ImNodes::EndNodeTitleBar();

        ImNodes::BeginStaticAttribute(GraphId::staticAttr(node.id));
        if (beginNodeParamTable("##constparams")) {

            ofs::ui::formRow(Str::ProcParamValue);
            float val = node.constantValue;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::DragFloat("##val", &val, 0.1f, 0.0f, 0.0f, "%.1f")) {
                ProcessingRegion updated = region;
                for (auto &n : updated.nodeGraph.nodes)
                    if (n.id == node.id)
                        n.constantValue = val;
                eq.push(ModifyRegionEvent{.regionId = regionId, .updatedRegion = updated, .snapshot = false});
            }
            snapshotOnDragStart(eq, regionId, region);

            ImGui::EndTable();
        }
        ImNodes::EndStaticAttribute();

        renderOutputPin(node.id);
        break;
    }

    case GraphNodeType::Discretize: {
        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(fmtScratch("{}  {}", ICON_BAR_CHART, Str::ProcNodeDiscretize.sv()));
        ImNodes::EndNodeTitleBar();

        ImNodes::BeginInputAttribute(GraphId::inPin(node.id, 0));
        ImGui::TextUnformatted(Str::ProcPinIn.c_str());
        ImNodes::EndInputAttribute();

        ImNodes::BeginStaticAttribute(GraphId::staticAttr(node.id));
        if (beginNodeParamTable("##discparams")) {
            ofs::ui::formRow(Str::ProcParamKeepActions);
            bool keep = !node.effect.params.empty() && node.effect.params[0] != 0.0f;
            if (ImGui::Checkbox("##keepactions", &keep)) {
                ProcessingRegion updated = region;
                for (auto &n : updated.nodeGraph.nodes)
                    if (n.id == node.id) {
                        if (n.effect.params.empty())
                            n.effect.params.resize(1);
                        n.effect.params[0] = keep ? 1.0f : 0.0f;
                    }
                eq.push(ModifyRegionEvent{.regionId = regionId, .updatedRegion = updated, .snapshot = true});
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", Str::ProcKeepActionTimes.c_str());

            ofs::ui::formRow("Hz");
            int nodeHz = node.effect.params.size() > 1 ? static_cast<int>(node.effect.params[1]) : 30;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::DragInt("##dischz", &nodeHz, 1.0f, 1, 120, "%d", ImGuiSliderFlags_AlwaysClamp)) {
                ProcessingRegion updated = region;
                for (auto &n : updated.nodeGraph.nodes)
                    if (n.id == node.id) {
                        if (n.effect.params.size() < 2)
                            n.effect.params.resize(2, 30.0f);
                        n.effect.params[1] = static_cast<float>(nodeHz);
                    }
                eq.push(ModifyRegionEvent{.regionId = regionId, .updatedRegion = updated, .snapshot = false});
            }
            snapshotOnDragStart(eq, regionId, region);
            ImGui::EndTable();
        }
        ImNodes::EndStaticAttribute();

        renderOutputPin(node.id);
        break;
    }

    case GraphNodeType::Functionalize: {
        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(fmtScratch("{}  {}", ICON_CHART_SPLINE, Str::ProcNodeFunctionalize.sv()));
        ImNodes::EndNodeTitleBar();

        ImNodes::BeginInputAttribute(GraphId::inPin(node.id, 0));
        ImGui::TextUnformatted(Str::ProcPinIn.c_str());
        ImNodes::EndInputAttribute();

        // No params; anchor the body to the standard width (see nodeContentW).
        ImNodes::BeginStaticAttribute(GraphId::staticAttr(node.id));
        ImGui::Dummy({nodeContentW(), 0.0f});
        ImNodes::EndStaticAttribute();

        renderOutputPin(node.id);
        break;
    }

    case GraphNodeType::Script: {
        const bool embedded = node.scriptEmbedded();
        // A graph-embedded node resolves by its source hash; a file node by its file name.
        const CompiledScript *cs = embedded ? scriptReg.findByHash(scriptContentHash(node.scriptEmbeddedSource))
                                            : scriptReg.find(node.scriptFile);

        ImNodes::BeginNodeTitleBar();
        if (embedded) {
            // Embedded nodes carry their code in the graph, not a file. scriptFile holds the suggested
            // fork name, so strip the ".cs" — there is no on-disk file for the extension to refer to.
            std::string_view stem = node.scriptFile;
            if (stem.ends_with(".cs"))
                stem.remove_suffix(3);
            ImGui::TextUnformatted(stem.empty() ? fmtScratch("{}  {}", ICON_BRACES, Str::ProcScript.sv())
                                                : fmtScratch("{}  {}  [{}]", ICON_BRACES, Str::ProcScript.sv(), stem));
        } else if (node.scriptFile.empty()) {
            ImGui::TextUnformatted(fmtScratch("{}  {}", ICON_BRACES, Str::ProcScript.sv()));
        } else {
            ImGui::TextUnformatted(fmtScratch("{}  {}  [{}]", ICON_BRACES, Str::ProcScript.sv(), node.scriptFile));
        }
        ImNodes::EndNodeTitleBar();

        // Pin labels come from the compiled script's declared names; fall back to index labels (in{i}/
        // out{k}) and the persisted pin-count caches when the artifact is absent (uncompiled / missing).
        static const std::vector<std::string> kNoScriptNames;
        const std::vector<std::string> &scriptInNames = cs ? cs->inputNames : kNoScriptNames;
        const std::vector<std::string> &scriptOutNames = cs ? cs->outputNames : kNoScriptNames;
        const int scriptOutputCount = std::max<int>(1, node.scriptOutputCount);
        for (int i = 0; i < node.scriptInputCount; ++i) {
            ImNodes::BeginInputAttribute(GraphId::inPin(node.id, i));
            const char *pinName = pinLabel(scriptInNames, i, Str::ProcPinIn.c_str());
            const bool connected = region.nodeGraph.findLinkToPin(node.id, i) != nullptr;
            ImGui::TextUnformatted(connected ? pinName : fmtScratch("{}  = 50", pinName));
            ImNodes::EndInputAttribute();
        }

        ImNodes::BeginStaticAttribute(GraphId::staticAttr(node.id));
        ImGui::PushItemWidth(nodeContentW());

        // Embedded nodes carry no body widgets here — their actions (fork/load) and compile status
        // share the output-pin row below, so the node stays compact. File nodes keep their picker.
        if (!embedded) {
            // ── File picker: a dropdown over <prefPath>/scripts/*.cs ──────────────
            const char *comboLabel = node.scriptFile.empty() ? Str::ProcSelectCs.c_str() : node.scriptFile.c_str();
            // The file list is cached and refreshed only when this node's combo opens, so the directory
            // is not scanned (nor strings allocated) every frame.
            const bool comboOpen = ImGui::BeginCombo(fmtScratch("##scriptfile{}", node.id), comboLabel);
            if (comboOpen) {
                if (m_scriptFileCacheNode != node.id) {
                    // Only the user folder: re-pointing a file node targets an editable user script. A
                    // shipped library script is added (as an embedded node) from the add menu, not here.
                    m_scriptFileCache.clear();
                    std::error_code ec;
                    for (const auto &de : std::filesystem::directory_iterator(scriptsDirPath(), ec))
                        if (de.is_regular_file() && de.path().extension() == ".cs")
                            m_scriptFileCache.push_back(ofs::util::toUtf8(de.path().filename()));
                    std::ranges::sort(m_scriptFileCache);
                    m_scriptFileCacheNode = node.id;
                }
                for (const auto &f : m_scriptFileCache) {
                    if (ImGui::Selectable(f.c_str(), f == node.scriptFile)) {
                        const ScriptHeader h = readScriptHeaderFile(f);
                        ProcessingRegion updated = region;
                        for (auto &n : updated.nodeGraph.nodes)
                            if (n.id == node.id) {
                                n.scriptFile = f;
                                n.scriptSignal = static_cast<uint8_t>(h.signal);
                                n.scriptInputCount = static_cast<uint8_t>(h.inputCount());
                                n.scriptOutputCount = static_cast<uint8_t>(h.outputCount());
                            }
                        eq.push(ModifyRegionEvent{.regionId = regionId, .updatedRegion = updated});
                        eq.push(CompileScriptEvent{.fileName = f});
                    }
                }
                ImGui::EndCombo();
            } else if (m_scriptFileCacheNode == node.id) {
                m_scriptFileCacheNode = -1; // closed — rescan on next open
            }
        } // end file-node picker

        // ── User params (declared via // !ofs:param) ──────────────────────────────
        // Size guard: effect.params is reconciled to the defs on compile (ScriptSystem); the brief
        // window before that — and an errored/missing artifact (empty params) — renders no widgets.
        const bool showParams = cs != nullptr && !cs->params.empty() && node.effect.params.size() == cs->params.size();
        if (showParams) {
            if (beginNodeParamTable("##scriptparams")) {
                for (int pi = 0; pi < static_cast<int>(cs->params.size()); ++pi) {
                    const auto &pd = cs->params[static_cast<size_t>(pi)];
                    nodeParamLabelRow(pd.name.c_str());
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    const char *wid = fmtScratch("##sp{}_{}", node.id, pi);
                    const bool clamp = pd.max > pd.min; // a real range was declared
                    const ImGuiSliderFlags fl = clamp ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
                    // Helper: fold a new value for slot pi into the region and push a re-eval. An atomic
                    // edit (checkbox/combo) snapshots immediately; a drag relies on snapshotOnDragStart.
                    const auto storeParam = [&](float v, bool atomic) {
                        ProcessingRegion updated = region;
                        for (auto &n : updated.nodeGraph.nodes)
                            if (n.id == node.id)
                                n.effect.params[static_cast<size_t>(pi)] = v;
                        eq.push(ModifyRegionEvent{.regionId = regionId, .updatedRegion = updated, .snapshot = atomic});
                    };
                    if (pd.type == OfsParamFloat) {
                        float val = node.effect.params[static_cast<size_t>(pi)];
                        if (ImGui::DragFloat(wid, &val, 0.01f, clamp ? pd.min : 0.0f, clamp ? pd.max : 0.0f, "%.3f",
                                             fl))
                            storeParam(val, false);
                        snapshotOnDragStart(eq, regionId, region);
                    } else if (pd.type == OfsParamBool) {
                        bool val = node.effect.params[static_cast<size_t>(pi)] != 0.0f;
                        if (ImGui::Checkbox(wid, &val))
                            storeParam(val ? 1.0f : 0.0f, true);
                    } else if (pd.type == OfsParamEnum) {
                        const int n = static_cast<int>(pd.enumLabels.size());
                        int val = static_cast<int>(std::lround(node.effect.params[static_cast<size_t>(pi)]));
                        val = std::clamp(val, 0, n > 0 ? n - 1 : 0);
                        const char **items =
                            ofs::FrameAllocator::instance().allocArray<const char *>(static_cast<size_t>(n));
                        for (int j = 0; j < n; ++j)
                            items[j] = pd.enumLabels[static_cast<size_t>(j)].c_str();
                        if (n > 0 && ImGui::Combo(wid, &val, items, n))
                            storeParam(static_cast<float>(val), true);
                    } else { // OfsParamInt
                        int val = static_cast<int>(node.effect.params[static_cast<size_t>(pi)]);
                        if (ImGui::DragInt(wid, &val, 1.0f, clamp ? static_cast<int>(pd.min) : 0,
                                           clamp ? static_cast<int>(pd.max) : 0, "%d", fl))
                            storeParam(static_cast<float>(val), false);
                        snapshotOnDragStart(eq, regionId, region);
                    }
                }
                ImGui::EndTable();
            }
        } else if (embedded) {
            // An embedded node has no file-picker combo, and while it is compiling (cs == nullptr) —
            // or if it errored / declares no params — the table above is skipped too, leaving the
            // static attribute empty. Anchor it to the standard width so there is no resize jump once
            // params appear (see nodeContentW).
            ImGui::Dummy({nodeContentW(), 0.0f});
        }

        ImGui::PopItemWidth();
        ImNodes::EndStaticAttribute();

        // Fold the node's action buttons and the compile-status indicator onto the first output-pin row,
        // keeping its label flush to the node's right edge beside the pin. Embedded and file nodes share
        // this layout; only the buttons differ (fork/load vs. edit/reload/folder + watch). Extra output
        // pins (a multi-output script) render as plain right-aligned rows below.
        renderOutputPinRow(node.id, pinLabel(scriptOutNames, 0, Str::ProcPinOut.c_str()), [&] {
            if (embedded) {
                // iconId(): "<icon> <translated label>###<stable id>" — the ###id keeps the widget
                // identity language-independent so a translated label can't reset its state.
                if (ImGui::SmallButton(Str::ProcSaveToFolder.iconId(ICON_SAVE, fmtScratch("fork{}", node.id))))
                    outSaveReqNodeId = node.id; // the caller opens the rename dialog (it owns the modal state)
                ImGui::SameLine();
                if (ImGui::SmallButton(Str::ProcLoadFromDisk.iconId(ICON_FOLDER_OPEN, fmtScratch("load{}", node.id))))
                    outLoadReqNodeId = node.id;
            } else {
                if (ImGui::SmallButton(fmtScratch("{}##edit{}", ICON_EDIT, node.id)) && !node.scriptFile.empty()) {
                    if (auto path = resolveScriptPath(node.scriptFile); !path.empty())
                        ofs::util::openInDefaultApp(path);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", Str::ProcEdit.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton(fmtScratch("{}##reload{}", ICON_REFRESH, node.id)) && !node.scriptFile.empty())
                    eq.push(CompileScriptEvent{.fileName = node.scriptFile});
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", Str::ProcReload.c_str());
                ImGui::SameLine();
                // Fork: copy the referenced file under a new name and repoint this node at the copy,
                // so the user can edit it without touching the original. Same modal as the embedded fork.
                ImGui::BeginDisabled(node.scriptFile.empty());
                if (ImGui::SmallButton(fmtScratch("{}##fork{}", ICON_GIT_FORK, node.id)))
                    outSaveReqNodeId = node.id;
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", Str::ProcSaveToFolder.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton(fmtScratch("{}##folder{}", ICON_FOLDER, node.id))) {
                    // Reveal the referenced script in the file browser (selected on Windows). With no
                    // file picked yet, fall back to just opening the scripts folder.
                    if (auto path = resolveScriptPath(node.scriptFile); !node.scriptFile.empty() && !path.empty())
                        ofs::util::revealInFileBrowser(path);
                    else
                        ofs::util::openInFileBrowser(scriptsDirPath());
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", Str::ProcFolder.c_str());

                // Auto-recompile when the file changes on disk (transient; ScriptSystem polls watched files).
                ImGui::SameLine();
                const bool noFile = node.scriptFile.empty();
                bool watch = node.scriptWatch;
                ImGui::BeginDisabled(noFile);
                if (ImGui::Checkbox(fmtScratch("{}##watch{}", ICON_FILE_CLOCK, node.id), &watch)) {
                    ProcessingRegion updated = region;
                    for (auto &n : updated.nodeGraph.nodes)
                        if (n.id == node.id)
                            n.scriptWatch = watch;
                    eq.push(ModifyRegionEvent{.regionId = regionId, .updatedRegion = updated});
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", Str::ProcWatchTip.c_str());
            }
            ImGui::SameLine();
            renderCompileStatus(eq, node, cs, /*noScript=*/!embedded && node.scriptFile.empty());
        });
        for (int k = 1; k < scriptOutputCount; ++k)
            renderRightAlignedOutput(node.id, k, pinLabel(scriptOutNames, k, Str::ProcPinOut.c_str()));
        break;
    }
    }

    ImNodes::EndNode();
    if (pushedOutline)
        ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();
}

// ── Main render ────────────────────────────────────────────────────────────────

void ProcessingPanel::render(const ScriptProject &project, EventQueue &eq, const EffectRegistryState &effectReg,
                             const ScriptRegistryState &scriptReg) {
    m_cursorInside = false;
    m_graphFocused = false;

    const int selId = project.procSelRegionId;

    auto regionIt = std::ranges::find_if(project.regions, [selId](const auto &r) { return r.id == selId; });
    if (regionIt == project.regions.end()) {
        return;
    }
    const ProcessingRegion &region = *regionIt;

    const float fs = ImGui::GetFontSize();
    ImGui::SetNextWindowSizeConstraints({fs * 18.0f, fs * 12.0f}, {FLT_MAX, FLT_MAX});
    // "###video_player" pins this to the same dock window as the Video Player (OfsApp renders only one
    // of the two per frame). Sharing one window keeps the center node single-window, so a hidden tab
    // bar survives; the visible label reads the translated "Processing". The id slug is language-
    // independent and must match the Video Player window's ###id exactly (VideoPlayerWindow.cpp, DockLayout.cpp).
    // NoNavInputs so the editor's unmodified arrow/Space shortcuts keep working while this panel has
    // focus, instead of being claimed by ImGui keyboard nav (see Application.cpp). The node graph's own
    // add-node menu is a separate popup window, so it stays keyboard-navigable.
    if (!ImGui::Begin(Str::ProcTitle.id("video_player"), nullptr,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoNavInputs)) {
        ImGui::End();
        return;
    }

    // Read by OfsApp's click-away deselect: is the cursor over this panel? ChildWindows so a hover over
    // the imnodes editor canvas (its own child window) still counts; AllowWhenBlocked* keep it true
    // mid node-drag or with a context popup open. Occlusion-aware on purpose — a click on a window
    // floating above the panel must not read as "inside", or it would suppress a legitimate deselect.
    m_cursorInside =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem |
                               ImGuiHoveredFlags_AllowWhenBlockedByPopup);

    // Sampled here, before any child widget runs this frame: an active InputText consumes Escape to
    // revert its edit and deactivates itself in the same frame, so by end-of-render (where the
    // Escape-to-close check lives) IsAnyItemActive() would already read false and we'd wrongly close.
    const bool itemActiveAtFrameStart = ImGui::IsAnyItemActive();

    // ── Pending state (read once, used in header and node rendering) ─────────
    int hotNodeId = -1;
    bool anyPending = false;
    for (size_t i = 0; i < kStandardAxisCount; ++i) {
        if (!region.axisRoles.test(i))
            continue;
        const auto &ax = project.axes[i];
        if (ax.pendingEval) {
            anyPending = true;
            int id = ax.pendingEval->currentNodeId.load();
            if (id != -1 && hotNodeId == -1)
                hotNodeId = id;
        }
    }

    renderHeader(project, eq, selId, region);

    const float footerH = ImGui::GetFrameHeightWithSpacing() + 4.0f;

    ImNodes::EditorContextSet(m_editorCtx);

    if (m_loadedRegionId != selId) {
        m_loadedRegionId = selId;
        ImNodes::EditorContextResetPanning({0.0f, 0.0f});
    }

    ImGui::BeginChild("##graph", {0.0f, -footerH}, false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // Suppress the add-node menu only when the cursor is over a graph element (a right-click there
    // belongs to the node/pin/link). The earlier global ImGui::IsAnyItemHovered() guard also tripped on
    // items the simulator's floating 3D overlay forces hovered via SetHoveredID — it sits over the
    // central dock window the panel shares, so its hijacked HoveredId silently blocked add-node on empty
    // canvas. imnodes' own hover queries must run outside the editor scope, so read them (last frame's
    // result) before BeginNodeEditor.
    int hoveredNodeId = 0, hoveredPinId = 0, hoveredLinkId = 0;
    const bool overNode = ImNodes::IsNodeHovered(&hoveredNodeId);
    const bool overPin = ImNodes::IsPinHovered(&hoveredPinId);
    const bool overLink = ImNodes::IsLinkHovered(&hoveredLinkId);
    const bool overGraphElement = overNode || overPin || overLink;

    // Capture the theme's window padding before BeginNodeEditor zeroes it (imnodes pushes
    // WindowPadding {0,0} for its canvas child); the context-menu popups restore it below.
    const ImVec2 stdWindowPad = ImGui::GetStyle().WindowPadding;

    ImNodes::BeginNodeEditor();

    // HACK (re-evaluate on every imgui/imnodes upgrade): imnodes renders the canvas into an internal child
    // window ("scrolling_region") that it creates *without* NoNavInputs, with no API to influence its
    // flags. Window nav flags don't inherit, so the panel's own NoNavInputs doesn't reach it: clicking the
    // canvas focuses that child, which raises io.NavActive and therefore io.WantCaptureKeyboard — even with
    // no widget active (NavActive:1, ActiveId:0) — and the BindingSystem keyboardCaptured gate then
    // swallows every shortcut (Ctrl+Z/Ctrl+Y died in the node editor). The canvas is mouse-driven and has
    // no nav of its own, so mark its window NoNavInputs. BeginChild rewrites window->Flags from its
    // argument each frame, so this must be re-applied every frame after BeginNodeEditor (the current window
    // here is the scrolling_region child). Mirrors the same dock-host workaround in DockLayout.cpp.
    ImGui::GetCurrentWindow()->Flags |= ImGuiWindowFlags_NoNavInputs;

    // Snap-create commits a link the moment a compatible pin is hovered rather than on a pixel-precise
    // release. Link-detach-with-drag-click is deliberately NOT enabled: dragging a connected pin to
    // tear off its link was more confusing than useful (a stray drag silently disconnected a node).
    // Links are removed explicitly instead (select + delete / context menu). Per-pin flags are captured
    // at BeginInput/OutputAttribute, so the push must wrap all node rendering; balanced by the pop
    // before EndNodeEditor.
    ImNodes::PushAttributeFlag(ImNodesAttributeFlags_EnableLinkCreationOnSnap);
    m_graphFocused = ImNodes::IsEditorHovered();

    // Hover, not focus, gates the add-node menu: a right-click on an unfocused panel both focuses the
    // window and must open the menu in the same click. IsEditorHovered() already respects z-order (it
    // uses ImGui::IsWindowHovered), so an occluding window suppresses it; requiring focus on top of
    // that swallowed the first click whenever another window held focus.
    const bool canvasHovered = ImNodes::IsEditorHovered();
    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (overNode || overPin) {
            // Right-click on a node (or one of its pins) → that node's context menu.
            m_ctxNodeId = GraphId::decode(overNode ? hoveredNodeId : hoveredPinId).owner;
            ImGui::OpenPopup("##nodectx");
        } else if (overLink) {
            m_ctxLinkId = GraphId::decode(hoveredLinkId).owner;
            ImGui::OpenPopup("##linkctx");
        } else {
            m_pendingLinkPin = -1;
            m_nodeFilter.clear();
            m_focusFilterNextFrame = true;
            ImGui::OpenPopup("##addnode");
        }
    }
    if (m_openAddNodeMenuNextFrame) {
        m_openAddNodeMenuNextFrame = false;
        m_nodeFilter.clear();
        m_focusFilterNextFrame = true;
        ImGui::OpenPopup("##addnode");
    }

    const AddNodeRequest addReq = renderAddNodeMenu(effectReg, stdWindowPad);
    const ImVec2 newNodePos{addReq.posX, addReq.posY};

    renderGraphContextMenus(region, eq, selId, stdWindowPad);

    int saveReqNodeId = -1; // set by an embedded Script node's "Save to scripts folder" button
    int loadReqNodeId = -1; // set by an embedded Script node's "Load from disk..." button
    for (const auto &node : region.nodeGraph.nodes) {
        bool hot = (node.id == hotNodeId);
        bool pending = anyPending && !hot;
        renderNode(node, effectReg, scriptReg, eq, selId, region, hot, pending, saveReqNodeId, loadReqNodeId);
    }
    if (saveReqNodeId >= 0) {
        if (const auto *n = region.nodeGraph.findNode(saveReqNodeId)) {
            m_saveScriptRegionId = selId;
            m_saveScriptNodeId = saveReqNodeId;
            if (n->scriptEmbeddedSource.empty()) {
                // File node fork: seed the modal with a "_copy" name and the file's current contents
                // so the clash check (byte-identical vs. differing) has the source to compare against.
                std::string_view stem = n->scriptFile;
                if (stem.ends_with(".cs"))
                    stem.remove_suffix(3);
                m_saveScriptName = std::string(stem) + "_copy.cs";
                auto src = ofs::util::readFile(resolveScriptPath(n->scriptFile));
                m_saveScriptSource = src ? std::move(*src) : std::string{};
            } else {
                m_saveScriptName = n->scriptFile.empty() ? std::string{"script.cs"} : n->scriptFile;
                m_saveScriptSource = n->scriptEmbeddedSource;
            }
            m_openSaveScriptModal = true;
        }
    }
    if (loadReqNodeId >= 0) {
        const std::string scriptsDir = ofs::util::toUtf8(scriptsDirPath());
        // The callback runs after the dialog resolves (a later frame), so it must not touch the
        // frame-local `region`: re-find it by id in the (stable) project, then apply the script.
        pickFile(eq,
                 {.kind = FileDialogKind::Open,
                  .key = "script_cs",
                  .title = Str::ProcSelectScriptTitle.c_str(),
                  .filterPatterns = {"*.cs"},
                  .filterDesc = Str::ProcCsFilterDesc.c_str(),
                  .fallbackDir = scriptsDir},
                 [&eq, &project, selId, loadReqNodeId](const std::string &picked) {
                     if (picked.empty())
                         return;
                     auto it = std::ranges::find_if(project.regions, [selId](const auto &r) { return r.id == selId; });
                     if (it == project.regions.end())
                         return;
                     const std::string fileName = ofs::util::toUtf8(ofs::util::fromUtf8(picked).filename());
                     const ScriptHeader h = readScriptHeaderFile(fileName);
                     ProcessingRegion updated = *it;
                     for (auto &n : updated.nodeGraph.nodes) {
                         if (n.id == loadReqNodeId) {
                             n.scriptFile = fileName;
                             n.scriptSignal = static_cast<uint8_t>(h.signal);
                             n.scriptInputCount = static_cast<uint8_t>(h.inputCount());
                             n.scriptOutputCount = static_cast<uint8_t>(h.outputCount());
                             n.scriptEmbeddedSource.clear();
                         }
                     }
                     eq.push(ModifyRegionEvent{.regionId = selId, .updatedRegion = std::move(updated)});
                     eq.push(CompileScriptEvent{.fileName = fileName});
                 });
    }

    for (const auto &link : region.nodeGraph.links) {
        const bool functional = isLinkFunctional(link, region.nodeGraph, effectReg);
        const ImU32 lBase = ofs::theme::GetColorU32(functional ? AppCol_LinkFunctional : AppCol_LinkDiscrete);
        ImNodes::PushColorStyle(ImNodesCol_Link, lBase);
        ImNodes::PushColorStyle(ImNodesCol_LinkHovered, stateLayer(lBase, 0.25f));
        ImNodes::PushColorStyle(ImNodesCol_LinkSelected, stateLayer(lBase, 0.40f));
        ImNodes::Link(GraphId::link(link.id), GraphId::outPin(link.fromNode, link.fromPin),
                      GraphId::inPin(link.toNode, link.toPin));
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
    }

    ImNodes::MiniMap(0.15f, ImNodesMiniMapLocation_TopRight);
    ImNodes::PopAttributeFlag();
    ImNodes::EndNodeEditor();

    {
        int droppedPin = -1;
        if (ImNodes::IsLinkDropped(&droppedPin, false)) {
            m_pendingLinkPin = droppedPin;
            m_openAddNodeMenuNextFrame = true;
        }
    }

    // An empty graph has no visible affordance to add a node (right-click canvas / drag a link into
    // space); prompt it centered on the canvas so the feature isn't a guess. Drawn after EndNodeEditor
    // so it sits in the panel's own (un-panned) child rect rather than scrolling with the canvas.
    if (region.nodeGraph.nodes.empty()) {
        ImDrawList *cdl = ImGui::GetWindowDrawList();
        const ImVec2 wPos = ImGui::GetWindowPos();
        const ImVec2 wSize = ImGui::GetWindowSize();
        const char *hint = ofs::ui::elide(Str::ProcAddNodeHint.c_str(), wSize.x - ImGui::GetFontSize() * 2.0f);
        const ImVec2 ts = ImGui::CalcTextSize(hint);
        cdl->AddText({wPos.x + (wSize.x - ts.x) * 0.5f, wPos.y + (wSize.y - ts.y) * 0.5f},
                     ofs::theme::GetColorU32(ImGuiCol_TextDisabled), hint);
    }

    {
        ProcessingRegion updated = region;
        if (syncPositions(updated.nodeGraph))
            eq.push(MoveRegionNodesEvent{.regionId = selId, .updatedRegion = updated});
    }

    // Fit-to-view: pan the whole graph so the chosen bbox — the current node selection, or every node
    // when nothing is selected — is centered in the canvas. imnodes has no zoom, so framing is a pure
    // translation that preserves layout; it rides MoveRegionNodesEvent (no re-eval, no undo step), the
    // same path panning uses. Runs here, after EndNodeEditor, because node dimensions are only measured
    // once the nodes have been laid out this frame. F triggers it while the canvas is hovered.
    const bool fitKey = m_graphFocused && ImGui::IsKeyPressed(ImGuiKey_F) && !itemActiveAtFrameStart &&
                        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if ((m_fitRequested || fitKey) && !region.nodeGraph.nodes.empty()) {
        m_fitRequested = false;
        std::vector<int> sel;
        if (const int nSel = ImNodes::NumSelectedNodes(); nSel > 0) {
            sel.resize(static_cast<size_t>(nSel));
            ImNodes::GetSelectedNodes(sel.data());
        }
        const auto selected = [&](int nodeId) {
            return std::ranges::any_of(sel, [&](int enc) { return GraphId::decode(enc).owner == nodeId; });
        };
        // Frame the selection only if it actually covers a live node; a stale/empty selection falls back
        // to framing the whole graph.
        const bool useSel = std::ranges::any_of(region.nodeGraph.nodes, [&](const auto &n) { return selected(n.id); });
        const auto framed = [&](int nodeId) { return !useSel || selected(nodeId); };
        ImVec2 mn{}, mx{};
        bool any = false;
        for (const auto &n : region.nodeGraph.nodes) {
            if (!framed(n.id))
                continue;
            const ImVec2 dim = ImNodes::GetNodeDimensions(GraphId::nodeBody(n.id));
            const ImVec2 lo{n.posX, n.posY};
            const ImVec2 hi{n.posX + dim.x, n.posY + dim.y};
            if (!any) {
                mn = lo;
                mx = hi;
                any = true;
            } else {
                mn.x = std::min(mn.x, lo.x);
                mn.y = std::min(mn.y, lo.y);
                mx.x = std::max(mx.x, hi.x);
                mx.y = std::max(mx.y, hi.y);
            }
        }
        if (any) {
            const ImVec2 canvas = ImGui::GetWindowSize();
            const ImVec2 off{canvas.x * 0.5f - (mn.x + mx.x) * 0.5f, canvas.y * 0.5f - (mn.y + mx.y) * 0.5f};
            if (std::abs(off.x) > 0.5f || std::abs(off.y) > 0.5f) {
                ProcessingRegion updated = region;
                for (auto &n : updated.nodeGraph.nodes) {
                    n.posX += off.x;
                    n.posY += off.y;
                }
                eq.push(MoveRegionNodesEvent{.regionId = selId, .updatedRegion = std::move(updated)});
            }
        }
    }

    {
        int startAttr = 0, endAttr = 0;
        if (ImNodes::IsLinkCreated(&startAttr, &endAttr)) {
            const auto isOut = [](int a) { return GraphId::decode(a).tag == GraphId::Tag::OutPin; };
            if (isOut(endAttr))
                std::swap(startAttr, endAttr);

            if (isOut(startAttr) && !isOut(endAttr)) {
                int fromNode = GraphId::decode(startAttr).owner;
                int fromPin = GraphId::decode(startAttr).slot;
                int toNode = GraphId::decode(endAttr).owner;
                int toPin = GraphId::decode(endAttr).slot;

                if (fromNode == toNode) {
                    eq.push(NotifyEvent{.level = NotifyLevel::Warning, .message = Str::ProcLinkRejectedSelf.c_str()});
                } else if (wouldCreateCycle(fromNode, toNode, region.nodeGraph)) {
                    eq.push(NotifyEvent{.level = NotifyLevel::Warning, .message = Str::ProcLinkRejectedCycle.c_str()});
                } else {
                    ProcessingRegion updated = region;
                    auto &links = updated.nodeGraph.links;
                    links.erase(
                        std::ranges::remove_if(
                            links, [toNode, toPin](const auto &l) { return l.toNode == toNode && l.toPin == toPin; })
                            .begin(),
                        links.end());
                    int linkId = updated.nodeGraph.allocId();
                    links.push_back(
                        {.id = linkId, .fromNode = fromNode, .fromPin = fromPin, .toNode = toNode, .toPin = toPin});
                    eq.push(ModifyRegionEvent{.regionId = selId, .updatedRegion = updated});
                }
            }
        }
    }

    {
        int linkId = 0;
        if (ImNodes::IsLinkDestroyed(&linkId)) {
            ProcessingRegion updated = region;
            removeLink(updated.nodeGraph, GraphId::decode(linkId).owner);
            eq.push(ModifyRegionEvent{.regionId = selId, .updatedRegion = updated});
        }
    }

    if (isMathNode(addReq.type)) {
        const GraphNodeType type = addReq.type;
        placeNewNode(eq, region, selId, newNodePos, m_pendingLinkPin, /*ignoresInput=*/false,
                     [type](ProcessingGraphNode &n) { n.type = type; });
        m_pendingLinkPin = -1;
    } else if (addReq.type == GraphNodeType::Discretize) {
        placeNewNode(eq, region, selId, newNodePos, m_pendingLinkPin, /*ignoresInput=*/false,
                     [](ProcessingGraphNode &n) {
                         n.type = GraphNodeType::Discretize;
                         // params: [0] keep-actions (default on → loss-free out of the box), [1] sampling Hz.
                         n.effect.params = {1.0f, 30.0f};
                     });
        m_pendingLinkPin = -1;
    } else if (addReq.type == GraphNodeType::Functionalize) {
        placeNewNode(eq, region, selId, newNodePos, m_pendingLinkPin, /*ignoresInput=*/false,
                     [](ProcessingGraphNode &n) { n.type = GraphNodeType::Functionalize; });
        m_pendingLinkPin = -1;
    } else if (addReq.type == GraphNodeType::Constant) {
        // A Constant is a generator (no input), so ignoresInput=true: a drop on an output pin makes no
        // splice, a drop on an input pin just feeds the Constant into it — exactly the old hand-rolled case.
        placeNewNode(eq, region, selId, newNodePos, m_pendingLinkPin, /*ignoresInput=*/true,
                     [](ProcessingGraphNode &n) { n.type = GraphNodeType::Constant; });
        m_pendingLinkPin = -1;
    } else if (addReq.addEffect) {
        const auto &def = effectReg.effects.at(addReq.effectType);
        std::vector<float> defParams;
        defParams.reserve(def.paramDefs.size());
        for (const auto &pd : def.paramDefs)
            defParams.push_back(pd.defaultValue);
        placeNewNode(eq, region, selId, newNodePos, m_pendingLinkPin, def.ignoresInput, [&](ProcessingGraphNode &n) {
            n.type = GraphNodeType::Effect;
            n.effect = ProcessingEffect{.type = def.type, .params = defParams};
        });
        m_pendingLinkPin = -1;
    } else if (addReq.addPlugin) {
        const auto &pn = effectReg.pluginNodes.at(addReq.pluginNodeId);
        placeNewNode(eq, region, selId, newNodePos, m_pendingLinkPin, /*ignoresInput=*/pn.inputCount == 0,
                     [&](ProcessingGraphNode &n) {
                         // Plugin nodes carry no scalar params — their state is a TState persisted in
                         // nodeState, populated lazily by the node's UI; a fresh node starts with empty state.
                         n.type = GraphNodeType::PluginNode;
                         n.pluginInputCount = static_cast<uint8_t>(pn.inputCount);
                         n.pluginOutputCount = static_cast<uint8_t>(pn.outputCount);
                         n.pluginSignal = static_cast<uint8_t>(pn.signal);
                         n.pluginNodeId = pn.id;
                     });
        m_pendingLinkPin = -1;
    } else if (addReq.addScript && addReq.scriptIndex >= 0 &&
               addReq.scriptIndex < static_cast<int>(m_scriptCatalog.size())) {
        // A ready script picked from the catalog (no editor, unlike "New Script…"). A shipped library
        // script is dropped as an EMBEDDED node — its source baked in from data.pak — so it is read-only
        // and forkable like any embedded script. A user-folder script is
        // dropped as a file reference (editable, hot-reloaded), as before.
        const ScriptCatalogEntry &ce = m_scriptCatalog[addReq.scriptIndex];
        std::optional<std::string> libSource =
            ce.library ? ofs::res::readText(std::string(kScriptLibPrefix) + ce.fileName) : std::optional<std::string>{};
        placeNewNode(eq, region, selId, newNodePos, m_pendingLinkPin, /*ignoresInput=*/ce.inputCount == 0,
                     [&](ProcessingGraphNode &n) {
                         n.type = GraphNodeType::Script;
                         n.scriptFile = ce.fileName; // file ref (user) or the suggested fork name (embedded)
                         n.scriptSignal = ce.signal;
                         n.scriptInputCount = static_cast<uint8_t>(ce.inputCount);
                         n.scriptOutputCount = static_cast<uint8_t>(ce.outputCount);
                         if (libSource)
                             n.scriptEmbeddedSource = *libSource;
                     });
        m_pendingLinkPin = -1;
        if (libSource)
            eq.push(CompileEmbeddedScriptEvent{.source = *libSource});
        else
            eq.push(CompileScriptEvent{.fileName = ce.fileName});
    } else if (m_scriptCreateConfirmed) {
        // The "New Script" modal wrote the stub last frame and captured the drop position; wire up
        // the node here (inside the graph child window, where GetWindowPos is the editor canvas).
        m_scriptCreateConfirmed = false;
        const ScriptHeader h = readScriptHeaderFile(m_newScriptFile);
        const ImVec2 screenPos{m_newScriptPosX, m_newScriptPosY};
        placeNewNode(eq, region, selId, screenPos, m_newScriptLinkPin, /*ignoresInput=*/h.inputCount() == 0,
                     [&](ProcessingGraphNode &n) {
                         n.type = GraphNodeType::Script;
                         n.scriptFile = m_newScriptFile;
                         n.scriptSignal = static_cast<uint8_t>(h.signal);
                         n.scriptInputCount = static_cast<uint8_t>(h.inputCount());
                         n.scriptOutputCount = static_cast<uint8_t>(h.outputCount());
                     });
        m_newScriptLinkPin = -1;
        eq.push(CompileScriptEvent{.fileName = m_newScriptFile});
        ofs::util::openInDefaultApp(scriptsDirPath() / ofs::util::fromUtf8(m_newScriptFile));
    }

    ImGui::EndChild();

    renderFooter(project, eq, selId, region, anyPending);

    maybeShowNewScriptModal(eq);
    maybeShowSaveScriptModal(eq);
    maybeShowTrustModal(project, eq, selId);
    maybeShowRemapModal(project, eq, selId, region);

    // Escape: clear a node/link selection first (the node-editor convention), and only close the panel
    // on a second press when nothing is selected. Closing clears the region selection, reverting the
    // shared dock window to the video player. Deliberately NOT gated on panel focus: selecting a region
    // (e.g. clicking its band on the timeline) opens the panel without moving focus to it, and Escape
    // must still close it — and that case has no node selection, so it closes on the first press.
    // Guarded so Escape still does its normal job first: dismiss any open popup/modal (a separate
    // window), or cancel an in-progress text edit anywhere (itemActiveAtFrameStart is global and was
    // sampled before this frame's InputTexts ran — see above).
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !itemActiveAtFrameStart &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        if (ImNodes::NumSelectedNodes() > 0 || ImNodes::NumSelectedLinks() > 0) {
            ImNodes::ClearNodeSelection();
            ImNodes::ClearLinkSelection();
        } else {
            eq.push(ClearRegionSelectionEvent{});
        }
    }

    ImGui::End();
}

bool ProcessingPanel::deleteSelected(const ScriptProject &project, EventQueue &eq) {
    const int numSelNodes = ImNodes::NumSelectedNodes();
    const int numSelLinks = ImNodes::NumSelectedLinks();
    if (numSelNodes == 0 && numSelLinks == 0)
        return false;

    const int selId = project.procSelRegionId;
    if (selId == -1)
        return false;

    auto regionIt = std::ranges::find_if(project.regions, [selId](const auto &r) { return r.id == selId; });
    if (regionIt == project.regions.end())
        return false;

    ImNodes::EditorContextSet(m_editorCtx);
    ProcessingRegion updated = *regionIt;

    if (numSelLinks > 0) {
        std::vector<int> selLinks(static_cast<size_t>(numSelLinks));
        ImNodes::GetSelectedLinks(selLinks.data());
        for (int lid : selLinks)
            removeLink(updated.nodeGraph, GraphId::decode(lid).owner);
    }

    if (numSelNodes > 0) {
        std::vector<int> selNodes(static_cast<size_t>(numSelNodes));
        ImNodes::GetSelectedNodes(selNodes.data());
        for (int encId : selNodes) {
            const int nid = GraphId::decode(encId).owner;
            const auto *n = updated.nodeGraph.findNode(nid);
            if (!n || n->type == GraphNodeType::Input || n->type == GraphNodeType::Output)
                continue;
            removeNodeAndLinks(updated.nodeGraph, nid);
        }
    }

    eq.push(ModifyRegionEvent{.regionId = selId, .updatedRegion = updated});
    return true;
}

void ProcessingPanel::renderGraphContextMenus(const ProcessingRegion &region, EventQueue &eq, int selId,
                                              ImVec2 windowPad) const {
    // Both popups open inside the imnodes canvas, where BeginNodeEditor has zeroed WindowPadding — restore
    // the theme's padding (captured before the editor scope) so the menus aren't cramped.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, windowPad);

    // Node context menu — Duplicate / Disconnect / Delete on the right-clicked node. Input/Output are the
    // region's fixed axis endpoints: they can be disconnected but never duplicated or deleted, so those
    // items are hidden for them. Each action follows the panel's copy-region → push-ModifyRegion recipe,
    // which earns undo for free.
    if (ImGui::BeginPopup("##nodectx")) {
        const ProcessingGraphNode *n = region.nodeGraph.findNode(m_ctxNodeId);
        if (!n) {
            ImGui::CloseCurrentPopup();
        } else {
            const int nodeId = m_ctxNodeId;
            const bool endpoint = n->type == GraphNodeType::Input || n->type == GraphNodeType::Output;
            const bool hasLinks = std::ranges::any_of(
                region.nodeGraph.links, [nodeId](const auto &l) { return l.fromNode == nodeId || l.toNode == nodeId; });
            if (!endpoint && ImGui::MenuItem(Str::ProcCtxDuplicate.iconId(ICON_COPY, "proc_node_dup"))) {
                ProcessingRegion updated = region;
                ProcessingGraphNode dup = *n;
                dup.id = updated.nodeGraph.allocId();
                // Offset the copy so it doesn't land exactly on the original.
                dup.posX += ImGui::GetFontSize() * 1.5f;
                dup.posY += ImGui::GetFontSize() * 1.5f;
                updated.nodeGraph.nodes.push_back(std::move(dup));
                eq.push(ModifyRegionEvent{.regionId = selId, .updatedRegion = std::move(updated)});
            }
            if (ImGui::MenuItem(Str::ProcCtxDisconnect.iconId(ICON_UNLINK, "proc_node_disc"), nullptr, false,
                                hasLinks)) {
                ProcessingRegion updated = region;
                auto &links = updated.nodeGraph.links;
                links.erase(std::ranges::remove_if(
                                links, [nodeId](const auto &l) { return l.fromNode == nodeId || l.toNode == nodeId; })
                                .begin(),
                            links.end());
                eq.push(ModifyRegionEvent{.regionId = selId, .updatedRegion = std::move(updated)});
            }
            if (!endpoint) {
                ImGui::Separator();
                if (ImGui::MenuItem(Str::ProcCtxDelete.iconId(ICON_TRASH, "proc_node_del"))) {
                    ProcessingRegion updated = region;
                    removeNodeAndLinks(updated.nodeGraph, nodeId);
                    eq.push(ModifyRegionEvent{.regionId = selId, .updatedRegion = std::move(updated)});
                }
            }
        }
        ImGui::EndPopup();
    }

    // Link context menu — Delete the right-clicked link.
    if (ImGui::BeginPopup("##linkctx")) {
        if (ImGui::MenuItem(Str::ProcCtxDelete.iconId(ICON_TRASH, "proc_link_del"))) {
            ProcessingRegion updated = region;
            removeLink(updated.nodeGraph, m_ctxLinkId);
            eq.push(ModifyRegionEvent{.regionId = selId, .updatedRegion = std::move(updated)});
        }
        ImGui::EndPopup();
    }

    ImGui::PopStyleVar();
}

ProcessingPanel::AddNodeRequest ProcessingPanel::renderAddNodeMenu(const EffectRegistryState &effectReg,
                                                                   ImVec2 windowPad) {
    AddNodeRequest req;
    ImVec2 newNodePos{};
    // Restore the theme's WindowPadding (captured before BeginNodeEditor zeroed it), matching the node/
    // link context menus instead of a fixed 8 px that wouldn't scale with the font/DPI.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, windowPad);
    if (ImGui::BeginPopup("##addnode")) {
        newNodePos = ImGui::GetMousePosOnOpeningCurrentPopup();

        // Rebuild the script catalog once per open (a file walk + header parse, not per frame).
        if (ImGui::IsWindowAppearing())
            buildScriptCatalog(m_scriptCatalog);

        // Picking Script defers to a modal (name + signal + inputs) instead of dropping a node
        // immediately; capture the drop position and any dropped link pin for the deferred create.
        auto requestNewScript = [&]() {
            m_openNewScriptModal = true;
            m_focusScriptNameNextFrame = true;
            m_newScriptPosX = newNodePos.x;
            m_newScriptPosY = newNodePos.y;
            m_newScriptLinkPin = m_pendingLinkPin;
            m_pendingLinkPin = -1;
            m_newScriptName.clear();
            m_newScriptDisplayName.clear();
            m_newScriptDescription.clear();
            m_newScriptSignal = 0;
            m_newScriptInputs = 1;
            m_newScriptOutputs = 1;
            m_newScriptComments = true;
            // Scan the scripts folder once, here on open, so the modal's "open existing" list needs
            // no per-frame directory walk.
            m_scriptFileList.clear();
            std::error_code ec;
            for (const auto &de : std::filesystem::directory_iterator(scriptsDirPath(), ec))
                if (de.is_regular_file() && de.path().extension() == ".cs")
                    m_scriptFileList.push_back(ofs::util::toUtf8(de.path().filename()));
            std::ranges::sort(m_scriptFileList);
            ImGui::CloseCurrentPopup();
        };

        if (m_focusFilterNextFrame) {
            ImGui::SetKeyboardFocusHere();
            m_focusFilterNextFrame = false;
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        ImGui::InputText("##filter", &m_nodeFilter);
        ImGui::Separator();

        const bool filtering = !m_nodeFilter.empty();
        const ImVec4 &mathCol = ofs::theme::GetStyleColorVec4(AppCol_NodeMath);
        const ImVec4 &funcCol = ofs::theme::GetStyleColorVec4(AppCol_LinkFunctional);
        const ImVec4 &discCol = ofs::theme::GetStyleColorVec4(AppCol_LinkDiscrete);

        auto renderEffectTooltip = [](const EffectDefinition &def) {
            if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                return;
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
            if (def.description.index != 0) // index 0 ⇒ no description key
                ImGui::TextUnformatted(def.description.c_str());
            ImGui::TextDisabled("%s",
                                (def.kind == EffectKind::Functional ? Str::ProcFunctional : Str::ProcDiscrete).c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        };

        auto renderDiscretizeTooltip = [] {
            if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                return;
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
            ImGui::TextUnformatted(Str::ProcDiscretizeDesc.c_str());
            ImGui::TextDisabled("%s", Str::ProcDiscrete.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        };

        auto renderFunctionalizeTooltip = [] {
            if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                return;
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
            ImGui::TextUnformatted(Str::ProcFunctionalizeDesc.c_str());
            ImGui::TextDisabled("%s", Str::ProcFunctional.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        };

        auto renderScriptTooltip = [](const ScriptCatalogEntry &e) {
            if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                return;
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
            if (!e.description.empty())
                ImGui::TextUnformatted(e.description.c_str());
            const char *sig = (e.signal == OfsSignalDiscrete ? Str::ProcDiscrete : Str::ProcFunctional).c_str();
            const char *kind = (e.library ? Str::ProcLibraryScript : Str::ProcScript).c_str();
            ImGui::TextDisabled("%s " GLYPH_MIDDLE_DOT " %s", sig, kind); // a separator glyph, not translatable text
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        };

        auto renderPluginNodeTooltip = [](const PluginNodeEntry &pn) {
            if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                return;
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
            if (!pn.description.empty())
                ImGui::TextUnformatted(pn.description.c_str());
            const char *sig = (pn.signal == OfsSignalDiscrete ? Str::ProcDiscrete : Str::ProcFunctional).c_str();
            ImGui::TextDisabled("%s", sig);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        };

        if (!filtering) {
            if (ImGui::BeginMenu(fmtScratch("{}  {}###procmenu_math", ICON_CALCULATOR, Str::ProcCatMath.sv()))) {
                ImGui::PushStyleColor(ImGuiCol_Text, mathCol);
                for (const auto t : kMathMenuTypes) {
                    if (ImGui::MenuItem(
                            fmtScratch("{}  {}###{}", mathNodeIcon(t), mathNodeLabel(t), mathNodeMenuId(t))))
                        req.type = t;
                }
                ImGui::PopStyleColor();
                ImGui::EndMenu();
            }

            if (ImGui::MenuItem(
                    fmtScratch("{}  {}" GLYPH_ELLIPSIS "###procnew_script", ICON_BRACES, Str::ProcScript.sv())))
                requestNewScript();

            // Generate / Modify / Combine submenus hold native effects and ready-to-add library/user
            // scripts together, in fixed bucket order so the menu is stable even when a category has
            // only scripts (the common case after the native cull). The built-in Discretize node lives
            // under Modify (a 1-in/1-out signal transform), so Modify is always shown.
            for (const NodeCategory category : {NodeCategory::Generate, NodeCategory::Modify, NodeCategory::Combine}) {
                const bool isModify = (category == NodeCategory::Modify);
                bool any = isModify;
                for (const auto &key : effectReg.orderedKeys)
                    if (effectReg.effects.at(key).category == category) {
                        any = true;
                        break;
                    }
                if (!any)
                    for (const auto &e : m_scriptCatalog)
                        if (nodeCategoryForInputs(e.inputCount) == category) {
                            any = true;
                            break;
                        }
                if (!any)
                    continue;
                if (!ImGui::BeginMenu(fmtScratch("{}  {}###procmenu_cat{}", nodeCategoryIcon(category),
                                                 nodeCategoryLabel(category), static_cast<int>(category))))
                    continue;
                if (isModify) {
                    ImGui::PushStyleColor(ImGuiCol_Text, discCol);
                    if (ImGui::MenuItem(
                            fmtScratch("{}  {}###procnode_discretize", ICON_BAR_CHART, Str::ProcNodeDiscretize.sv())))
                        req.type = GraphNodeType::Discretize;
                    ImGui::PopStyleColor();
                    renderDiscretizeTooltip();
                    ImGui::PushStyleColor(ImGuiCol_Text, funcCol);
                    if (ImGui::MenuItem(fmtScratch("{}  {}###procnode_functionalize", ICON_CHART_SPLINE,
                                                   Str::ProcNodeFunctionalize.sv())))
                        req.type = GraphNodeType::Functionalize;
                    ImGui::PopStyleColor();
                    renderFunctionalizeTooltip();
                }
                for (const auto &key : effectReg.orderedKeys) {
                    const auto &def = effectReg.effects.at(key);
                    if (def.category != category)
                        continue;
                    ImGui::PushStyleColor(ImGuiCol_Text, def.kind == EffectKind::Functional ? funcCol : discCol);
                    if (ImGui::MenuItem(fmtScratch("{}  {}", nodeCategoryIcon(category), def.displayName.sv()))) {
                        req.addEffect = true;
                        req.effectType = key;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopStyleColor();
                    renderEffectTooltip(def);
                }
                for (int i = 0; i < static_cast<int>(m_scriptCatalog.size()); ++i) {
                    const auto &e = m_scriptCatalog[i];
                    if (nodeCategoryForInputs(e.inputCount) != category)
                        continue;
                    ImGui::PushStyleColor(ImGuiCol_Text, e.signal == OfsSignalFunctional ? funcCol : discCol);
                    if (ImGui::MenuItem(fmtScratch("{}  {}", scriptMenuIcon(e.library), e.displayName))) {
                        req.addScript = true;
                        req.scriptIndex = i;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopStyleColor();
                    renderScriptTooltip(e);
                }
                ImGui::EndMenu();
            }

            if (!effectReg.pluginNodeKeys.empty()) {
                // Group each plugin's nodes by arity bucket (Generate / Modify / Combine).
                std::string_view openPlugin;
                for (const auto &key : effectReg.pluginNodeKeys) {
                    const auto &pn = effectReg.pluginNodes.at(key);
                    if (pn.category == openPlugin)
                        continue;
                    openPlugin = pn.category;
                    if (!ImGui::BeginMenu(fmtScratch("{}  {}", ICON_PLUGIN, pn.category)))
                        continue;
                    for (const NodeCategory bucket :
                         {NodeCategory::Generate, NodeCategory::Modify, NodeCategory::Combine}) {
                        bool kindOpen = false;
                        for (const auto &k2 : effectReg.pluginNodeKeys) {
                            const auto &pn2 = effectReg.pluginNodes.at(k2);
                            if (std::string_view(pn2.category) != openPlugin ||
                                nodeCategoryForInputs(pn2.inputCount) != bucket)
                                continue;
                            if (!kindOpen) {
                                kindOpen = ImGui::BeginMenu(
                                    fmtScratch("{}  {}", nodeCategoryIcon(bucket), nodeCategoryLabel(bucket)));
                                if (!kindOpen)
                                    break;
                            }
                            ImGui::PushStyleColor(ImGuiCol_Text, pn2.signal == OfsSignalFunctional ? funcCol : discCol);
                            if (ImGui::MenuItem(fmtScratch("{}  {}", pluginNodeIcon(pn2), pn2.displayName))) {
                                req.addPlugin = true;
                                req.pluginNodeId = k2;
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::PopStyleColor();
                            renderPluginNodeTooltip(pn2);
                        }
                        if (kindOpen)
                            ImGui::EndMenu();
                    }
                    ImGui::EndMenu();
                }
            }
        } else {
            bool mathHeaderShown = false;
            for (const auto t : kMathMenuTypes) {
                if (!ofs::util::fuzzyMatch(m_nodeFilter, mathNodeLabel(t)).matched)
                    continue;
                if (!mathHeaderShown) {
                    ImGui::SeparatorText(fmtScratch("{}  {}", ICON_CALCULATOR, Str::ProcCatMath.sv()));
                    mathHeaderShown = true;
                }
                ImGui::PushStyleColor(ImGuiCol_Text, mathCol);
                if (ImGui::MenuItem(fmtScratch("{}  {}###{}", mathNodeIcon(t), mathNodeLabel(t), mathNodeMenuId(t)))) {
                    req.type = t;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopStyleColor();
            }

            if (ofs::util::fuzzyMatch(m_nodeFilter, Str::ProcScript.sv()).matched) {
                ImGui::SeparatorText(fmtScratch("{}  {}", ICON_BRACES, Str::ProcScript.sv()));
                if (ImGui::MenuItem(
                        fmtScratch("{}  {}" GLYPH_ELLIPSIS "###procnew_script", ICON_BRACES, Str::ProcScript.sv())))
                    requestNewScript();
            }

            // Generate / Modify / Combine — effects and catalog scripts under one header per category.
            for (const NodeCategory category : {NodeCategory::Generate, NodeCategory::Modify, NodeCategory::Combine}) {
                const char *catLabel = nodeCategoryLabel(category);
                bool headerShown = false;
                auto header = [&] {
                    if (!headerShown) {
                        ImGui::SeparatorText(fmtScratch("{}  {}", nodeCategoryIcon(category), catLabel));
                        headerShown = true;
                    }
                };
                if (category == NodeCategory::Modify &&
                    ofs::util::fuzzyMatchAny(m_nodeFilter, {Str::ProcNodeDiscretize.sv(), catLabel}).matched) {
                    header();
                    ImGui::PushStyleColor(ImGuiCol_Text, discCol);
                    if (ImGui::MenuItem(
                            fmtScratch("{}  {}###procnode_discretize", ICON_BAR_CHART, Str::ProcNodeDiscretize.sv()))) {
                        req.type = GraphNodeType::Discretize;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopStyleColor();
                    renderDiscretizeTooltip();
                }
                if (category == NodeCategory::Modify &&
                    ofs::util::fuzzyMatchAny(m_nodeFilter, {Str::ProcNodeFunctionalize.sv(), catLabel}).matched) {
                    header();
                    ImGui::PushStyleColor(ImGuiCol_Text, funcCol);
                    if (ImGui::MenuItem(fmtScratch("{}  {}###procnode_functionalize", ICON_CHART_SPLINE,
                                                   Str::ProcNodeFunctionalize.sv()))) {
                        req.type = GraphNodeType::Functionalize;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopStyleColor();
                    renderFunctionalizeTooltip();
                }
                for (const auto &key : effectReg.orderedKeys) {
                    const auto &def = effectReg.effects.at(key);
                    if (def.category != category)
                        continue;
                    if (!ofs::util::fuzzyMatchAny(m_nodeFilter, {def.displayName.sv(), catLabel}).matched)
                        continue;
                    header();
                    ImGui::PushStyleColor(ImGuiCol_Text, def.kind == EffectKind::Functional ? funcCol : discCol);
                    if (ImGui::MenuItem(fmtScratch("{}  {}", nodeCategoryIcon(category), def.displayName.sv()))) {
                        req.addEffect = true;
                        req.effectType = key;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopStyleColor();
                    renderEffectTooltip(def);
                }
                for (int i = 0; i < static_cast<int>(m_scriptCatalog.size()); ++i) {
                    const auto &e = m_scriptCatalog[i];
                    if (nodeCategoryForInputs(e.inputCount) != category)
                        continue;
                    if (!ofs::util::fuzzyMatchAny(m_nodeFilter, {e.displayName, catLabel}).matched)
                        continue;
                    header();
                    ImGui::PushStyleColor(ImGuiCol_Text, e.signal == OfsSignalFunctional ? funcCol : discCol);
                    if (ImGui::MenuItem(fmtScratch("{}  {}", scriptMenuIcon(e.library), e.displayName))) {
                        req.addScript = true;
                        req.scriptIndex = i;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopStyleColor();
                    renderScriptTooltip(e);
                }
            }
            std::string_view openCategory;
            for (const auto &key : effectReg.pluginNodeKeys) {
                const auto &pn = effectReg.pluginNodes.at(key);
                if (!ofs::util::fuzzyMatchAny(m_nodeFilter, {pn.displayName, pn.category,
                                                             nodeCategoryLabel(nodeCategoryForInputs(pn.inputCount))})
                         .matched)
                    continue;
                if (pn.category != openCategory) {
                    openCategory = pn.category;
                    ImGui::SeparatorText(fmtScratch("{}  {}", ICON_PLUGIN, pn.category));
                }
                ImGui::PushStyleColor(ImGuiCol_Text, pn.signal == OfsSignalFunctional ? funcCol : discCol);
                if (ImGui::MenuItem(fmtScratch("{}  {}", pluginNodeIcon(pn), pn.displayName))) {
                    req.addPlugin = true;
                    req.pluginNodeId = key;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopStyleColor();
                renderPluginNodeTooltip(pn);
            }
        }

        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    req.posX = newNodePos.x;
    req.posY = newNodePos.y;
    return req;
}

void ProcessingPanel::renderHeader(const ScriptProject &project, EventQueue &eq, int selId,
                                   const ProcessingRegion &region) {
    // ── Header ────────────────────────────────────────────────────────────────
    ImGui::TextDisabled("%s " GLYPH_EN_DASH " %s", TimeUtil::formatTime(region.startTime, true),
                        TimeUtil::formatTime(region.endTime, true));

    // Tight, font-relative chip grid (≈3 px spacing, ≈5×2 px padding at the 18 px default) so the chips
    // and their gaps scale with the font/DPI instead of shrinking to hairlines at high DPI.
    const float em = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(em * 0.18f, em * 0.18f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(em * 0.28f, em * 0.12f));

    auto renderChipRow = [&](size_t first, size_t last, bool sameLineBefore) {
        bool rowStarted = false;
        for (size_t i = first; i <= last; ++i) {
            const auto axis = static_cast<StandardAxis>(i);
            if (isScratchAxis(axis) && !project.axes[i].showInStrip)
                continue;

            const bool assigned = region.axisRoles.test(i);
            // Deactivated chip uses the theme's dimmed-axis token (same as inactive axes elsewhere)
            // rather than scaling toward black, which goes near-invisible on the light theme.
            const ImVec4 btnCol = assigned ? standardAxisColorVec4(axis) : standardAxisColorDimVec4(axis);

            if (rowStarted || sameLineBefore)
                ImGui::SameLine();
            rowStarted = true;

            ImGui::PushID(static_cast<int>(i));
            ImGui::PushStyleColor(ImGuiCol_Button, btnCol);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ofs::ui::brighten(btnCol, 0.2f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ofs::ui::brighten(btnCol, 0.35f));

            if (ImGui::Button(standardAxisTag(axis).data())) {
                eq.push(AssignAxisToRegionEvent{.regionId = selId, .axis = axis, .assign = !assigned});
            }

            ImGui::PopStyleColor(3);
            ImGui::PopID();

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", ofs::loc::localizedAxisName(axis));
        }
    };

    ImGui::SameLine(0.0f, em * 0.85f); // ≈15 px at the 18 px default
    renderChipRow(0, 9, false);
    renderChipRow(10, kStandardAxisCount - 1, true);

    ImGui::PopStyleVar(2);

    bool locked = project.procPanelLocked;
    if (ImGui::Checkbox(locked ? ICON_LOCK "##lock" : ICON_LOCK_OPEN "##lock", &locked))
        eq.push(SetProcPanelLockedEvent{locked});
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", Str::ProcLockTip.c_str());

    // Show-actions toggle, mirroring the lock as a single-icon checkbox (eye / eye-off). ###id keeps
    // the id stable across the icon swap so the click target doesn't move when toggled.
    ImGui::SameLine();
    bool showPts = region.showSourceActions;
    if (ImGui::Checkbox(showPts ? ICON_EYE "###showactions" : ICON_EYE_OFF "###showactions", &showPts)) {
        ProcessingRegion updated = region;
        updated.showSourceActions = showPts;
        eq.push(ModifyRegionEvent{.regionId = selId, .updatedRegion = updated});
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", Str::ProcShowActionsTip.c_str());

    // Reserve the right end of the row for the graph I/O buttons; the name field fills the gap between.
    // Measure the actual (translated) labels so the reserve tracks the rendered text, not an English literal.
    const char *fitGraphLabel = Str::ProcFit.icon(ICON_FOCUS);
    const char *remapGraphLabel = Str::ProcRemap.icon(ICON_ARROW_RIGHT_LEFT);
    const char *saveGraphLabel = Str::ProcSaveGraph.icon(ICON_SAVE);
    const char *loadGraphLabel = Str::ProcLoadGraph.icon(ICON_FOLDER_OPEN);
    const float ioSpacing = ImGui::GetStyle().ItemSpacing.x;
    const float ioPadX = ImGui::GetStyle().FramePadding.x * 2.0f;
    const float fitBtnW = ImGui::CalcTextSize(fitGraphLabel).x + ioPadX;
    const float remapBtnW = ImGui::CalcTextSize(remapGraphLabel).x + ioPadX;
    const float saveBtnW = ImGui::CalcTextSize(saveGraphLabel).x + ioPadX;
    const float loadBtnW = ImGui::CalcTextSize(loadGraphLabel).x + ioPadX;
    const float ioReserved = fitBtnW + remapBtnW + saveBtnW + loadBtnW + ioSpacing * 4.0f + ofs::ui::kRightGap;

    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ioReserved);
    ImGui::InputText("##region_name", &m_nameEdit);
    // IsItemDeactivatedAfterEdit also fires on a type-then-revert (the buffer was touched but ends
    // identical). Pushing then would still take an undo snapshot before onModifyRegion discards the
    // unchanged region, so commit only a real rename.
    if (ImGui::IsItemDeactivatedAfterEdit() && m_nameEdit != region.name) {
        ProcessingRegion updated = region;
        updated.name = m_nameEdit;
        eq.push(ModifyRegionEvent{.regionId = selId, .updatedRegion = std::move(updated)});
    } else if (!ImGui::IsItemActive()) {
        // Single sync point: mirror region.name while not editing, so external
        // renames (region switch, graph load) refresh the field. The else skips the
        // commit frame, before the pushed event drains, so the typed value survives.
        m_nameEdit = region.name;
    }

    ImGui::SameLine();
    if (ImGui::Button(fmtScratch("{}###fitgraph", fitGraphLabel)))
        m_fitRequested = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", Str::ProcFitTip.c_str());
    ImGui::SameLine();
    if (ImGui::Button(fmtScratch("{}###remapgraph", remapGraphLabel)))
        eq.push(RemapCurrentGraphEvent{selId});
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", Str::ProcRemapTip.c_str());
    ImGui::SameLine();
    if (ImGui::Button(fmtScratch("{}###savegraph", saveGraphLabel)))
        eq.push(SaveGraphEvent{selId});
    ImGui::SameLine();
    if (ImGui::Button(fmtScratch("{}###loadgraph", loadGraphLabel)))
        eq.push(LoadGraphEvent{selId});

    ImGui::Separator();
}

void ProcessingPanel::renderFooter(const ScriptProject &project, EventQueue &eq, int selId,
                                   const ProcessingRegion &region, bool anyPending) {
    // ── Footer ────────────────────────────────────────────────────────────────
    // Footer: compute (Bake/Recompute) then region rate (Hz/Auto). Graph I/O and the show-actions
    // toggle live in the header row.
    ImGui::Separator();

    // Group 1 — compute. ###Bake/###Recompute pin the IDs so the icon prefix doesn't break ItemClick in tests.
    if (ImGui::Button(fmtScratch("{} {}###Bake", ICON_BAKE, Str::ProcBake.sv()))) {
        eq.push(BakeRegionEvent{selId});
    }
    ImGui::SetItemTooltip("%s", Str::ProcBakeTip.c_str());
    ImGui::SameLine();
    if (ImGui::Button(fmtScratch("{} {}###Recompute", ICON_REFRESH, Str::ProcRecompute.sv()))) {
        for (size_t i = 0; i < kStandardAxisCount; ++i)
            if (region.axisRoles.test(i))
                eq.push(RequestAxisEvalEvent{static_cast<StandardAxis>(i)});
    }

    // Hz is the discretization rate applied wherever the graph turns a functional signal discrete;
    // the "%d Hz" format renders the unit inside the drag, so no label.
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3.0f);
    int hz = region.hz;
    if (ImGui::DragInt("##regionhz", &hz, 1.0f, 1, 120, "%d Hz", ImGuiSliderFlags_AlwaysClamp)) {
        ProcessingRegion updated = region;
        updated.hz = hz;
        eq.push(ModifyRegionEvent{.regionId = selId, .updatedRegion = updated, .snapshot = false});
    }
    ImGui::SetItemTooltip("%s", Str::ProcHzTip.c_str());
    snapshotOnDragStart(eq, selId, region);

    // Auto-eval is global, transient state. Unchecking halts it: results clear and edits stop
    // recomputing until the next Recompute (or re-check). Checked maps directly to autoEvalEnabled.
    ImGui::SameLine();
    bool autoEval = project.autoEvalEnabled;
    if (ImGui::Checkbox(fmtScratch("{}###autoEval", Str::ProcAuto.sv()), &autoEval))
        eq.push(SetAutoEvalEnabledEvent{autoEval});
    ImGui::SetItemTooltip("%s", Str::ProcAutoTip.c_str());

    // Show eval status: spinner while pending, time when resolved, "not computed" when auto-eval is
    // off and nothing has been computed yet (halt cleared the result; the user hasn't hit Recompute).
    {
        const float spinR = ImGui::GetFontSize() * 0.4f;
        const char *evalPrefix = Str::ProcEvalPrefix.c_str();
        const float prefixW = ImGui::CalcTextSize(evalPrefix).x;

        double evalMs = -1.0;
        if (!anyPending) {
            for (size_t i = 0; i < kStandardAxisCount; ++i) {
                if (!region.axisRoles.test(i))
                    continue;
                const auto &optResolved = project.axes[i].resolved;
                if (!optResolved)
                    continue;
                evalMs = optResolved->evalMs;
                break;
            }
        }

        if (anyPending) {
            const float totalW = prefixW + spinR * 2.f;
            ImGui::SameLine();
            float avail = ImGui::GetContentRegionAvail().x;
            if (avail > totalW)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - totalW);
            ImGui::TextDisabled("%s", evalPrefix);
            ImGui::SameLine(0.f, 0.f);
            ofs::ui::spinner("##eval", spinR, 2.f, ofs::theme::GetColorU32(ImGuiCol_TextDisabled));
        } else if (evalMs >= 0.0) {
            ImGui::SameLine();
            const char *timeLabel = Str::ProcEvalMs.fmt(fmtScratch("{:.2f}", evalMs));
            float avail = ImGui::GetContentRegionAvail().x;
            float labelW = ImGui::CalcTextSize(timeLabel).x;
            if (avail > labelW)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - labelW);
            ImGui::TextDisabled("%s", timeLabel);
        } else if (!project.autoEvalEnabled) {
            ImGui::SameLine();
            const char *label = Str::ProcNotComputed.c_str();
            float avail = ImGui::GetContentRegionAvail().x;
            float labelW = ImGui::CalcTextSize(label).x;
            if (avail > labelW)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - labelW);
            ImGui::TextDisabled("%s", label);
        }
    }
}
void ProcessingPanel::maybeShowNewScriptModal(EventQueue &eq) {
    // ── Add Script dialog (open an existing .cs, or create a new one) ─────────────
    // Raised once when m_openNewScriptModal latches; the body (captures only `this`) reads the
    // panel's script-form members and is consumed by the deferred node-create block next frame.
    if (m_openNewScriptModal) {
        m_openNewScriptModal = false;
        showCustomModal(
            eq,
            {.title = Str::ProcAddScriptTitle.c_str(), .width = ImGui::GetFontSize() * 22.5f, .body = [this]() -> bool {
                 bool closeModal = false;
                 bool entered = false; // set by the name InputText (EnterReturnsTrue) inside the create form

                 // Align label→value rows on a measured label column rather than a fixed-width form
                 // table: the Signal/Inputs radio groups are wider than the modal, and a table cell
                 // would *clip* the last radio. Free-flow rows let the AlwaysAutoResize modal grow to
                 // fit the widest row, and the label offset is measured (not a hardcoded px) so a longer
                 // translation can't overrun it.
                 const float labelColW = std::max({ImGui::CalcTextSize(Str::ProcOpen.c_str()).x,
                                                   ImGui::CalcTextSize(Str::ProcName.c_str()).x,
                                                   ImGui::CalcTextSize(Str::ProcDisplayName.c_str()).x,
                                                   ImGui::CalcTextSize(Str::ProcDescription.c_str()).x,
                                                   ImGui::CalcTextSize(Str::ProcSignal.c_str()).x,
                                                   ImGui::CalcTextSize(Str::ProcInputs.c_str()).x,
                                                   ImGui::CalcTextSize(Str::ProcOutputs.c_str()).x}) +
                                         ImGui::GetStyle().ItemSpacing.x * 2.0f;
                 auto labelCell = [labelColW](const char *label) {
                     ImGui::AlignTextToFramePadding();
                     ImGui::TextUnformatted(label);
                     ImGui::SameLine(labelColW);
                 };

                 // ── Open an existing script from <prefPath>/scripts ──────────────────────
                 if (!m_scriptFileList.empty()) {
                     labelCell(Str::ProcOpen);
                     ImGui::SetNextItemWidth(-FLT_MIN);
                     if (ImGui::BeginCombo("##openexisting", Str::ProcChooseExisting)) {
                         for (const auto &f : m_scriptFileList) {
                             if (ImGui::Selectable(f.c_str())) {
                                 // The deferred node-create block reads the file's header for signal/inputs;
                                 // the file already exists, so no stub is written.
                                 m_newScriptFile = f;
                                 m_scriptCreateConfirmed = true;
                                 closeModal = true;
                             }
                         }
                         ImGui::EndCombo();
                     }
                     ImGui::SeparatorText(Str::ProcOrCreateNew.id("proc_or_create"));
                 }

                 // ── Create a new script ──────────────────────────────────────────────────
                 labelCell(Str::ProcName);
                 if (m_focusScriptNameNextFrame) {
                     ImGui::SetKeyboardFocusHere();
                     m_focusScriptNameNextFrame = false;
                 }
                 ImGui::SetNextItemWidth(-FLT_MIN);
                 entered = ImGui::InputText("##scriptname", &m_newScriptName,
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

                 // Optional add-menu metadata baked into the new .cs header. Hints spell out the
                 // fallback so an empty field reads as a deliberate default, not a forgotten one.
                 labelCell(Str::ProcDisplayName);
                 ImGui::SetNextItemWidth(-FLT_MIN);
                 ImGui::InputTextWithHint("##scriptdisplayname", Str::ProcDisplayNameHint, &m_newScriptDisplayName);

                 labelCell(Str::ProcDescription);
                 ImGui::SetNextItemWidth(-FLT_MIN);
                 ImGui::InputTextWithHint("##scriptdescription", Str::ProcDescriptionHint, &m_newScriptDescription);

                 labelCell(Str::ProcSignal);
                 ImGui::RadioButton(Str::ProcFunctional.id("sig_func"), &m_newScriptSignal, 0);
                 ImGui::SameLine();
                 ImGui::RadioButton(Str::ProcDiscrete.id("sig_disc"), &m_newScriptSignal, 1);

                 // Inputs 0..16, outputs 1..16 (OFS_MAX_NODE_PINS). InputInt has no built-in range, so
                 // clamp after each step/type — the count can't exceed what the pin-id encoding allows.
                 const float countW = ImGui::GetFontSize() * 8.0f;
                 labelCell(Str::ProcInputs);
                 ImGui::SetNextItemWidth(countW);
                 ImGui::InputInt("##scriptinputs", &m_newScriptInputs);
                 m_newScriptInputs = std::clamp(m_newScriptInputs, 0, OFS_MAX_NODE_PINS);

                 labelCell(Str::ProcOutputs);
                 ImGui::SetNextItemWidth(countW);
                 ImGui::InputInt("##scriptoutputs", &m_newScriptOutputs);
                 m_newScriptOutputs = std::clamp(m_newScriptOutputs, 1, OFS_MAX_NODE_PINS);

                 // Seed the .cs with the verbose inline guide, or a bare directives+body stub for users
                 // who already know the API. Transient — resets to on each time the dialog opens.
                 ImGui::Checkbox(Str::ProcStubComments.id("stub_comments"), &m_newScriptComments);

                 // Resolve the final file name: trim, reject path separators, ensure a single .cs extension.
                 const auto [finalName, nameEmpty, badChars] = resolveScriptName(m_newScriptName);
                 std::error_code existsEc;
                 const bool exists =
                     !nameEmpty && std::filesystem::exists(scriptsDirPath() / ofs::util::fromUtf8(finalName), existsEc);

                 ImGui::Separator();
                 if (nameEmpty)
                     ImGui::TextDisabled("%s", Str::ProcEnterFileName.c_str());
                 else if (badChars)
                     ImGui::TextColored(ofs::theme::GetStyleColorVec4(AppCol_Error), "%s",
                                        Str::ProcInvalidChars.c_str());
                 else if (exists)
                     ImGui::TextColored(ofs::theme::GetStyleColorVec4(AppCol_Error), "%s",
                                        Str::ProcAlreadyExists.fmt(finalName));
                 else
                     ImGui::TextDisabled("%s", Str::ProcCreates.fmt(finalName));

                 const bool canCreate = !nameEmpty && !badChars && !exists;

                 ImGui::Separator();
                 if (!canCreate)
                     ImGui::BeginDisabled();
                 const bool create = ImGui::Button(Str::ProcCreate.id("proc_create")) || (entered && canCreate);
                 if (!canCreate)
                     ImGui::EndDisabled();
                 if (create && canCreate) {
                     const auto signal = m_newScriptSignal == 1 ? OfsSignalDiscrete : OfsSignalFunctional;
                     if (createScriptStub(finalName, signal, m_newScriptInputs, m_newScriptOutputs, m_newScriptComments,
                                          m_newScriptDisplayName, m_newScriptDescription)) {
                         m_newScriptFile = finalName;
                         m_scriptCreateConfirmed = true; // the node-create block wires it up next frame
                     }
                     closeModal = true;
                 }
                 ImGui::SameLine();
                 if (ImGui::Button(Str::ProcCancel.id("proc_cancel")))
                     closeModal = true;

                 return closeModal;
             }});
    }
}

void ProcessingPanel::maybeShowSaveScriptModal(EventQueue &eq) {
    // ── Save embedded script to the scripts folder (promote to a file node) ──────
    if (m_openSaveScriptModal) {
        m_openSaveScriptModal = false;
        showCustomModal(
            eq, {.title = Str::ProcSaveEmbeddedTitle.c_str(),
                 .width = ImGui::GetFontSize() * 23.75f,
                 .body = [this, eqp = &eq]() -> bool {
                     ImGui::TextWrapped("%s", Str::ProcSaveEmbeddedBody.c_str());
                     ImGui::Separator();
                     bool entered = false;
                     if (ofs::ui::beginForm("##saveembeddedform")) {
                         ofs::ui::formRow(Str::ProcName);
                         ImGui::SetNextItemWidth(-FLT_MIN);
                         entered = ImGui::InputText("##saveembeddedname", &m_saveScriptName,
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
                         ofs::ui::endForm();
                     }

                     // Resolve the final name: trim, reject path separators, ensure a single .cs extension.
                     const auto [finalName, nameEmpty, badChars] = resolveScriptName(m_saveScriptName);

                     // A name clash is allowed only if the existing file is byte-identical (then we just point at
                     // it); a differing file forces a rename.
                     bool clashDiffers = false;
                     if (!nameEmpty && !badChars) {
                         std::error_code existsEc;
                         const auto path = scriptsDirPath() / ofs::util::fromUtf8(finalName);
                         if (std::filesystem::exists(path, existsEc)) {
                             auto local = ofs::util::readFile(path);
                             clashDiffers = !local || *local != m_saveScriptSource;
                         }
                     }

                     ImGui::Separator();
                     if (nameEmpty)
                         ImGui::TextDisabled("%s", Str::ProcEnterFileName.c_str());
                     else if (badChars)
                         ImGui::TextColored(ofs::theme::GetStyleColorVec4(AppCol_Error), "%s",
                                            Str::ProcInvalidChars.c_str());
                     else if (clashDiffers)
                         ImGui::TextColored(ofs::theme::GetStyleColorVec4(AppCol_Error), "%s",
                                            Str::ProcDifferentExists.fmt(finalName));
                     else
                         ImGui::TextDisabled("%s", Str::ProcSaves.fmt(finalName));

                     const bool canSave = !nameEmpty && !badChars && !clashDiffers;

                     ImGui::Separator();
                     ImGui::BeginDisabled(!canSave);
                     const bool save = ImGui::Button(Str::ProcSave.id("proc_emb_save")) || (entered && canSave);
                     ImGui::EndDisabled();
                     if (save && canSave) {
                         eqp->push(SaveEmbeddedScriptEvent{
                             .regionId = m_saveScriptRegionId, .nodeId = m_saveScriptNodeId, .fileName = finalName});
                         return true;
                     }
                     ImGui::SameLine();
                     if (ImGui::Button(Str::ProcCancel.id("proc_emb_cancel")))
                         return true;
                     return false;
                 }});
    }
}

void ProcessingPanel::maybeShowTrustModal(const ScriptProject &project, EventQueue &eq, int selId) {
    // ── Graph-load trust warning (embedded scripts run code) ─────────────────────
    // Raised once when a pending graph-load needs trust; the body re-checks the pending load each
    // frame (via the captured project pointer) and closes itself when it is gone — resolved or
    // cancelled elsewhere — so it needs no per-frame driving from the panel.
    if (project.pendingGraphLoad && project.pendingGraphLoad->regionId == selId &&
        project.pendingGraphLoad->needsTrust && m_trustForRegion != selId) {
        m_trustForRegion = selId;
        showCustomModal(
            eq, {.title = Str::ProcTrustTitle.c_str(),
                 .width = ImGui::GetFontSize() * 26.25f,
                 .body = [this, proj = &project, eqp = &eq, regionId = selId]() -> bool {
                     const auto &pgl = proj->pendingGraphLoad;
                     if (!pgl || pgl->regionId != regionId || !pgl->needsTrust) {
                         m_trustForRegion = -1;
                         return true;
                     }
                     const auto &pend = *pgl;
                     ImGui::TextWrapped("%s", Str::ProcTrustBody.c_str());
                     ImGui::Separator();
                     for (const auto &n : pend.graph.nodes)
                         if (n.scriptEmbedded())
                             ImGui::BulletText("%s", n.scriptFile.empty() ? Str::ProcEmbeddedScript.c_str()
                                                                          : n.scriptFile.c_str());
                     ImGui::Separator();
                     ImGui::TextDisabled("%s", Str::ProcTrustNote.c_str());
                     ImGui::Separator();
                     // Let the recipient read the code in a real editor before deciding. ProjectManager
                     // writes throwaway copies to a temp dir and opens them; nothing is accepted here.
                     if (ImGui::Button(Str::ProcReviewCode.id("proc_review"))) // intentionally does not close the modal
                         eqp->push(ReviewGraphScriptsEvent{regionId});
                     ImGui::SameLine();
                     if (ImGui::Button(Str::ProcLoadAndRun.id("proc_loadrun"))) {
                         eqp->push(ConfirmGraphTrustEvent{regionId});
                         m_trustForRegion = -1;
                         return true;
                     }
                     ImGui::SameLine();
                     if (ImGui::Button(Str::ProcCancel.id("proc_trust_cancel"))) {
                         eqp->push(CancelGraphLoadEvent{regionId});
                         m_trustForRegion = -1;
                         return true;
                     }
                     return false;
                 }});
    }
}

void ProcessingPanel::maybeShowRemapModal(const ScriptProject &project, EventQueue &eq, int selId,
                                          const ProcessingRegion &region) {
    // ── Graph-load axis remap dialog ────────────────────────────────────────────
    // Raised once per pending load. The positional defaults read the live `region`, so they are
    // computed here at the push site (not in the body, which has no region reference). The body
    // re-checks the pending load each frame; a size mismatch (the pending load was swapped out
    // for one with a different axis count) closes it, and the next frame re-pushes with fresh
    // targets since m_remapForRegion was reset to -1.
    if (project.pendingGraphLoad && project.pendingGraphLoad->regionId == selId &&
        !project.pendingGraphLoad->needsTrust && m_remapForRegion != selId) {
        const auto &pend = *project.pendingGraphLoad;
        m_remapForRegion = selId;
        std::vector<int> regionAxes; // region's assigned axes, for positional defaults
        for (size_t i = 0; i < kStandardAxisCount; ++i)
            if (region.axisRoles.test(i))
                regionAxes.push_back(static_cast<int>(i));
        m_remapTargets.assign(pend.savedAxes.size(), 0);
        for (size_t k = 0; k < pend.savedAxes.size(); ++k)
            m_remapTargets[k] = k < regionAxes.size() ? regionAxes[k] : static_cast<int>(pend.savedAxes[k]);

        showCustomModal(
            eq, {.title = Str::ProcRemapTitle.c_str(),
                 .width = ImGui::GetFontSize() * 22.5f,
                 .body = [this, proj = &project, eqp = &eq, regionId = selId]() -> bool {
                     const auto &pgl = proj->pendingGraphLoad;
                     if (!pgl || pgl->regionId != regionId || pgl->needsTrust) {
                         m_remapForRegion = -1;
                         return true;
                     }
                     const auto &pend = *pgl;
                     if (m_remapTargets.size() != pend.savedAxes.size()) {
                         m_remapForRegion = -1; // pending load swapped; re-push next frame with fresh targets
                         return true;
                     }

                     ImGui::TextWrapped("%s", Str::ProcRemapBody.c_str());
                     ImGui::Separator();

                     // 2-column form: [saved axis name] | → [target picker]. The label column auto-fits the
                     // (variable-length) axis names instead of an absolute SameLine offset.
                     if (ofs::ui::beginForm("##remapform")) {
                         for (size_t k = 0; k < pend.savedAxes.size(); ++k) {
                             ImGui::PushID(static_cast<int>(k));
                             ofs::ui::formRow(ofs::loc::localizedAxisName(pend.savedAxes[k]));
                             ImGui::TextUnformatted(GLYPH_ARROW_RIGHT);
                             ImGui::SameLine();
                             ImGui::SetNextItemWidth(-FLT_MIN);
                             const char *preview =
                                 ofs::loc::localizedAxisName(static_cast<StandardAxis>(m_remapTargets[k]));
                             if (ImGui::BeginCombo("##target", preview)) {
                                 for (size_t i = 0; i < kStandardAxisCount; ++i) {
                                     if (isScratchAxis(static_cast<StandardAxis>(i)) && !proj->axes[i].showInStrip)
                                         continue;
                                     const bool sel = m_remapTargets[k] == static_cast<int>(i);
                                     if (ImGui::Selectable(
                                             fmtScratch("{}###remap_opt_{}",
                                                        ofs::loc::localizedAxisName(static_cast<StandardAxis>(i)), i),
                                             sel))
                                         m_remapTargets[k] = static_cast<int>(i);
                                     if (sel)
                                         ImGui::SetItemDefaultFocus();
                                 }
                                 ImGui::EndCombo();
                             }
                             ImGui::PopID();
                         }
                         ofs::ui::endForm();
                     }

                     bool dup = false;
                     for (size_t a = 0; a < m_remapTargets.size() && !dup; ++a)
                         for (size_t b = a + 1; b < m_remapTargets.size(); ++b)
                             if (m_remapTargets[a] == m_remapTargets[b]) {
                                 dup = true;
                                 break;
                             }
                     if (dup)
                         ImGui::TextColored(ofs::theme::GetStyleColorVec4(AppCol_Error), "%s",
                                            Str::ProcRemapDistinct.c_str());

                     ImGui::Separator();
                     if (dup)
                         ImGui::BeginDisabled();
                     // Load is unclickable while dup, so reaching here implies !dup (BeginDisabled was not
                     // called) — returning early needs no matching EndDisabled.
                     if (ImGui::Button(Str::ProcLoad.id("proc_remap_load"))) {
                         ApplyGraphRemapEvent ev{.regionId = regionId};
                         for (size_t k = 0; k < pend.savedAxes.size(); ++k)
                             ev.mapping.push_back(
                                 {.from = pend.savedAxes[k], .to = static_cast<StandardAxis>(m_remapTargets[k])});
                         eqp->push(std::move(ev));
                         m_remapForRegion = -1;
                         return true;
                     }
                     if (dup)
                         ImGui::EndDisabled();
                     ImGui::SameLine();
                     if (ImGui::Button(Str::ProcCancel.id("proc_remap_cancel"))) {
                         eqp->push(CancelGraphLoadEvent{regionId});
                         m_remapForRegion = -1;
                         return true;
                     }
                     return false;
                 }});
    }
}
} // namespace ofs
