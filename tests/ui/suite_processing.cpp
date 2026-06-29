#include "Core/Events.h"
#include "Core/ProcessingRegion.h"
#include "Core/StandardAxis.h"
#include "Services/CommandRegistry.h"
#include "Services/EffectRegistry.h"
#include "UI/Icons.h"
#include "UI/ProcessingPanel.h"
#include "helpers/TestState.h"
#include <algorithm>
#include <imgui.h>
#include <imgui_internal.h> // ImGuiWindow::DockNode, ImGuiDockNode, FindWindowByName
#include <imgui_te_context.h>
#include <imgui_te_engine.h>
#include <imnodes.h> // GetNodeDimensions / GetStyle, to assert node-width stability
#include <string>

// The Processing panel and the Video Player are the same dock window (shared "###video_player" id),
// swapped by selection. So once a region is selected the center window's *name* is this — the engine
// still resolves items under it because the id text after "###" is what gets hashed.
static constexpr const char *kCenterWin = "Processing###video_player";

// The dock node a window lives in, looked up by id straight from ImGui (the engine's locator only
// finds currently-submitted windows; these tests inspect the node regardless of which view is up).
static ImGuiDockNode *nodeOf(const char *window) {
    ImGuiWindow *w = ImGui::FindWindowByName(window);
    return w != nullptr ? w->DockNode : nullptr;
}

// True when the window under the cursor (respecting z-order) is the panel or a child of it — the
// imnodes canvas reports as a child. The guard the click-away tests use to confirm they are actually
// over the panel and not a window floating above it.
static bool panelUnderCursor() {
    ImGuiWindow *panel = ImGui::FindWindowByName(kCenterWin);
    for (ImGuiWindow *w = ImGui::GetCurrentContext()->HoveredWindow; w != nullptr; w = w->ParentWindow)
        if (w == panel)
            return true;
    return false;
}

static bool anyPopupOpen() {
    return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

// Create a 0–30 s region on L0. CreateRegionEvent auto-selects the new region, so on the next frame
// OfsApp renders the Processing panel into the center window in place of the video.
static void setupRegion(ImGuiTestContext *ctx) {
    loadFixture(ctx);
    getTestState().eventQueue->push(ofs::CreateRegionEvent{
        .axisRole = ofs::StandardAxis::L0,
        .startTime = 0.0,
        .endTime = 30.0,
    });
    ctx->Yield();
}

// Region selected → Layout/Default so the panel sits in a deterministic central node large enough for
// the add-node menu to land a node on empty canvas.
static void setupRegionInDefaultLayout(ImGuiTestContext *ctx) {
    setupRegion(ctx);
    ctx->MenuClick("//##MainMenuBar/###menu_layout/###layout_default");
    ctx->Yield(3);
}

// Right-click the empty graph canvas, filter the add-node menu to `filter`, and click the item whose
// full (icon + label) text is `label`. Leaves the freshly-created node as nodeGraph.nodes.back().
static void addNodeViaMenu(ImGuiTestContext *ctx, const char *filter, const std::string &label) {
    ImGuiWindow *panel = ImGui::FindWindowByName(kCenterWin);
    IM_CHECK(panel != nullptr);
    ctx->MouseMoveToPos(panel->Rect().GetCenter());
    ctx->Yield();
    IM_CHECK(panelUnderCursor());
    ctx->MouseClick(ImGuiMouseButton_Right);
    ctx->Yield(2);
    IM_CHECK(anyPopupOpen());
    ctx->KeyCharsReplace(filter);
    ctx->Yield(2);
    ctx->ItemClick(label.c_str());
    ctx->Yield(2);
}

// Right-click a node's title bar to open its context menu. The node is located by its imnodes screen
// position (the panel's editor context is still current right after a render), nudged inside the title
// so the click lands on the node body — not a param drag — and registers IsNodeHovered first.
static void rightClickNode(ImGuiTestContext *ctx, int nodeId) {
    const ImVec2 p = ImNodes::GetNodeScreenSpacePos(ofs::GraphId::nodeBody(nodeId));
    ctx->MouseMoveToPos(ImVec2(p.x + 12.0f, p.y + 8.0f));
    ctx->Yield(); // let imnodes register the hover so the next frame's IsNodeHovered() is true
    ctx->MouseClick(ImGuiMouseButton_Right);
    ctx->Yield(2);
}

// Right-click the link between two nodes. imnodes exposes no pin/link screen position, so probe the
// bezier: walk down the vertical band at the horizontal midpoint between the source node's right edge
// and the target node's left edge until IsLinkHovered fires, then right-click. Returns false if the link
// was never hit (the caller asserts). Reliable for the default graph, whose endpoints share a row.
static bool rightClickLink(ImGuiTestContext *ctx, int fromNodeId, int toNodeId) {
    const ImVec2 from = ImNodes::GetNodeScreenSpacePos(ofs::GraphId::nodeBody(fromNodeId));
    const ImVec2 fromDim = ImNodes::GetNodeDimensions(ofs::GraphId::nodeBody(fromNodeId));
    const ImVec2 to = ImNodes::GetNodeScreenSpacePos(ofs::GraphId::nodeBody(toNodeId));
    const float midX = ((from.x + fromDim.x) + to.x) * 0.5f;
    int linkId = 0;
    for (float y = from.y; y <= from.y + fromDim.y; y += 2.0f) {
        ctx->MouseMoveToPos(ImVec2(midX, y));
        ctx->Yield();
        if (ImNodes::IsLinkHovered(&linkId)) {
            ctx->MouseClick(ImGuiMouseButton_Right);
            ctx->Yield(2);
            return true;
        }
    }
    return false;
}

void RegisterProcessingTests(ImGuiTestEngine *e) {
    IM_REGISTER_TEST(e, "processing", "bake_button_present_with_selected_region")->TestFunc =
        [](ImGuiTestContext *ctx) {
            setupRegion(ctx);
            IM_CHECK(ctx->ItemExists("Processing###video_player/Bake"));
        };

    IM_REGISTER_TEST(e, "processing", "bake_button_click_does_not_crash")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegion(ctx);
        ctx->ItemClick("Processing###video_player/Bake");
        ctx->Yield();
        IM_CHECK(ImGui::GetCurrentContext() != nullptr);
    };

    IM_REGISTER_TEST(e, "processing", "show_actions_checkbox_toggles_region_state")->TestFunc =
        [](ImGuiTestContext *ctx) {
            setupRegion(ctx);
            auto &proj = *getTestState().project;

            const bool initial = proj.regions[0].showSourceActions;
            ctx->ItemClick("Processing###video_player/showactions");
            ctx->Yield();

            IM_CHECK_EQ(proj.regions[0].showSourceActions, !initial);
        };

    IM_REGISTER_TEST(e, "processing", "assign_axis_chip_toggles_role")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegion(ctx);
        auto &proj = *getTestState().project;

        // R0 starts unassigned.
        IM_CHECK(!proj.regions[0].axisRoles.test(static_cast<size_t>(ofs::StandardAxis::R0)));

        // R0 is inside ImGui::PushID(3) — use $$3 to encode the integer PushID in the path.
        ctx->ItemClick("Processing###video_player/$$3/R0");
        ctx->Yield();

        IM_CHECK(proj.regions[0].axisRoles.test(static_cast<size_t>(ofs::StandardAxis::R0)));

        // Clicking again unassigns (L0 remains so the guard doesn't block).
        ctx->ItemClick("Processing###video_player/$$3/R0");
        ctx->Yield();

        IM_CHECK(!proj.regions[0].axisRoles.test(static_cast<size_t>(ofs::StandardAxis::R0)));
    };

    // Selecting a region swaps the center window's content to the Processing graph without spawning a
    // second dock window: it stays the very same ImGui window (same id and node), only the label flips
    // from "Video Player" to "Processing". This single-window invariant is what the feature rests on.
    IM_REGISTER_TEST(e, "processing", "panel_shares_video_player_dock_node")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        ctx->MenuClick("//##MainMenuBar/###menu_layout/###layout_default"); // deterministic central node; layout
                                                                            // persists across tests
        ctx->Yield(3);

        // No region: the center dock window is the Video Player.
        ImGuiWindow *center = ImGui::FindWindowByName("Video Player###video_player");
        IM_CHECK(center != nullptr);
        IM_CHECK(center->DockNode != nullptr);
        // The visible "Video Player" title is localized; only the id slug after ### is stable, so assert
        // on that rather than the full (translatable) display name.
        IM_CHECK(std::string(center->Name).ends_with("###video_player"));

        getTestState().eventQueue->push(ofs::CreateRegionEvent{
            .axisRole = ofs::StandardAxis::L0,
            .startTime = 0.0,
            .endTime = 30.0,
        });
        ctx->Yield(4);

        // Same window object (id is hash of the shared "###video_player" slug either way), now showing the
        // (translated) Processing label. Assert on the language-independent slug, not the visible title.
        ImGuiWindow *now = ImGui::FindWindowByName(kCenterWin);
        IM_CHECK_EQ(now, center);
        IM_CHECK(std::string(now->Name).ends_with("###video_player"));
        IM_CHECK(now->DockNode != nullptr);
        IM_CHECK_EQ(now->DockNode->Windows.Size, 1); // one window in the node — never tabbed
    };

    // The regression that drove this design: ImGui force-clears a node's HiddenTabBar flag (and
    // persists the loss) the moment the node holds more than one window. With the shared-window
    // approach the node stays single-window, so a user who hid the Video Player's tab bar keeps it
    // hidden after a region is selected.
    IM_REGISTER_TEST(e, "processing", "hidden_tab_bar_survives_processing_view")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        ctx->MenuClick("//##MainMenuBar/###menu_layout/###layout_default");
        ctx->Yield(3);

        // Hide the Video Player node's tab bar, as the right-click "Hide Tab Bar" menu would.
        ImGuiDockNode *node = nodeOf("Video Player###video_player");
        IM_CHECK(node != nullptr);
        node->SetLocalFlags(node->LocalFlags | ImGuiDockNodeFlags_HiddenTabBar);
        ctx->Yield(2);
        IM_CHECK(node->IsHiddenTabBar()); // precondition

        getTestState().eventQueue->push(ofs::CreateRegionEvent{
            .axisRole = ofs::StandardAxis::L0,
            .startTime = 0.0,
            .endTime = 30.0,
        });
        ctx->Yield(4);

        ImGuiDockNode *after = nodeOf("Video Player###video_player");
        IM_CHECK(after != nullptr);
        IM_CHECK_EQ(after->Windows.Size, 1); // swapped in place, no second window added
        IM_CHECK(after->IsHiddenTabBar());   // preference preserved (the whole point)
    };

    // Click-away deselect: with a region selected the Processing panel occupies the center node. A
    // left-click outside it — here the (empty) timeline curve, which is neither the panel nor a region
    // bar — clears the region selection, reverting the center window to the Video Player. This exercises
    // ProcessingPanel::cursorInsideThisFrame() reporting "outside" so OfsApp pushes ClearRegionSelection.
    IM_REGISTER_TEST(e, "processing", "click_outside_panel_deselects_region")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegion(ctx);
        auto &proj = *getTestState().project;
        IM_CHECK(proj.procSelRegionId != -1); // CreateRegionEvent auto-selected it
        ctx->Yield();

        // Curve-area center: empty (loadFixture leaves L0 actionless), in the Timeline window, not
        // the region bar — so wasRegionClickedThisFrame stays false and the click reads as away.
        const ImRect curve = ctx->ItemInfo("Timeline###timeline/##timeline").RectFull;
        ctx->MouseMoveToPos(curve.GetCenter());
        ctx->Yield();
        IM_CHECK(!panelUnderCursor()); // guard: genuinely outside the panel
        ctx->MouseClick(ImGuiMouseButton_Left);
        ctx->Yield(2);

        IM_CHECK_EQ(proj.procSelRegionId, -1);
    };

    // Counterpart: a click *inside* the panel must not deselect. The hit-test reports the cursor inside
    // this frame, so OfsApp's click-away leaves the selection intact.
    IM_REGISTER_TEST(e, "processing", "click_inside_panel_keeps_region")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegion(ctx);
        auto &proj = *getTestState().project;
        const int selected = proj.procSelRegionId;
        IM_CHECK(selected != -1);

        ImGuiWindow *panel = ImGui::FindWindowByName(kCenterWin);
        IM_CHECK(panel != nullptr);
        ctx->MouseMoveToPos(panel->Rect().GetCenter()); // empty graph canvas, no widget
        ctx->Yield();
        IM_CHECK(panelUnderCursor()); // guard: actually over the panel, not an overlay
        ctx->MouseClick(ImGuiMouseButton_Left);
        ctx->Yield(2);

        IM_CHECK_EQ(proj.procSelRegionId, selected); // still selected
    };

    // Right-clicking the graph canvas must open the add-node menu even when the panel is *not* the
    // focused window. A user expects one right-click to both focus the panel and bring up the menu;
    // gating the menu on IsWindowFocused() swallowed that first click whenever another window held
    // focus, leaving the panel visible but unable to add nodes.
    IM_REGISTER_TEST(e, "processing", "right_click_adds_node_when_panel_unfocused")->TestFunc =
        [](ImGuiTestContext *ctx) {
            setupRegion(ctx);
            ctx->MenuClick("//##MainMenuBar/###menu_layout/###layout_default"); // deterministic central node placement
            ctx->Yield(3);
            auto &proj = *getTestState().project;
            IM_CHECK(!proj.regions.empty());

            ImGuiWindow *panel = ImGui::FindWindowByName(kCenterWin);
            IM_CHECK(panel != nullptr);

            // Take focus away from the panel: a sibling window becomes the focused one, so the panel
            // renders unfocused — the exact state the bug regressed in.
            ctx->WindowFocus("Timeline###timeline");
            ctx->Yield();
            ImGuiWindow *nav = ImGui::GetCurrentContext()->NavWindow;
            IM_CHECK(nav == nullptr || nav->RootWindow != panel); // precondition: panel isn't focused

            const size_t before = proj.regions[0].nodeGraph.nodes.size();

            // One right-click on the empty canvas: it must open the add-node popup despite the panel
            // being unfocused.
            ctx->MouseMoveToPos(panel->Rect().GetCenter());
            ctx->Yield();
            IM_CHECK(panelUnderCursor()); // guard: cursor genuinely over the panel canvas
            ctx->MouseClick(ImGuiMouseButton_Right);
            ctx->Yield(2);
            IM_CHECK(anyPopupOpen()); // the regression: this stayed closed when unfocused

            // And the menu actually instantiates a node, proving the whole add path works from here.
            // Filter to Constant (icon + label) and click it — a node should land in the graph.
            ctx->KeyCharsReplace("Constant");
            ctx->Yield(2);
            const std::string item = std::string("**/") + ICON_HASH + "  Constant";
            ctx->ItemClick(item.c_str());
            ctx->Yield(2);

            IM_CHECK_EQ(proj.regions[0].nodeGraph.nodes.size(), before + 1);
        };

    // Add a Math node (Add) through the filtered add-node menu and confirm both the count and the
    // created node's type — exercising the isMathNode(pendingAddType) create branch.
    IM_REGISTER_TEST(e, "processing", "add_math_node")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegionInDefaultLayout(ctx);
        auto &proj = *getTestState().project;
        const size_t before = proj.regions[0].nodeGraph.nodes.size();

        addNodeViaMenu(ctx, "Add", std::string("**/") + ICON_PLUS + "  Add  (A + B)");

        IM_CHECK_EQ(proj.regions[0].nodeGraph.nodes.size(), before + 1);
        IM_CHECK(proj.regions[0].nodeGraph.nodes.back().type == ofs::GraphNodeType::Add);
    };

    // Add "Sine Wave" — after the cull this is a shipped library script catalogued in the add menu
    // (ICON_LIBRARY marks library scripts; user scripts use ICON_BRACES), under Generate, so the
    // pendingAddScript branch drops a Script node referencing the staged data/scripts/lib/Sine.cs. Proves the
    // catalog scan + script-from-catalog create path.
    IM_REGISTER_TEST(e, "processing", "add_library_script_node")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegionInDefaultLayout(ctx);
        auto &proj = *getTestState().project;
        const size_t before = proj.regions[0].nodeGraph.nodes.size();

        addNodeViaMenu(ctx, "Sine", std::string("**/") + ICON_LIBRARY + "  Sine Wave");

        IM_CHECK_EQ(proj.regions[0].nodeGraph.nodes.size(), before + 1);
        const auto &n = proj.regions[0].nodeGraph.nodes.back();
        IM_CHECK(n.type == ofs::GraphNodeType::Script);
        // A shipped library script is dropped as an EMBEDDED node: its source is baked in from data.pak
        // (read-only, forkable) and scriptFile holds the suggested fork name.
        IM_CHECK(n.scriptEmbedded());
        IM_CHECK(!n.scriptEmbeddedSource.empty());
        IM_CHECK_STR_EQ(n.scriptFile.c_str(), "Sine.cs");
    };

    // Select a placed node (imnodes click) and delete it via ProcessingPanel::deleteSelected — the
    // selection-driven path the Delete key triggers when the graph is focused. Input/Output nodes are
    // delete-protected, so we add and remove a Constant.
    IM_REGISTER_TEST(e, "processing", "delete_selected_node_removes_it")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegionInDefaultLayout(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        addNodeViaMenu(ctx, "Constant", std::string("**/") + ICON_HASH + "  Constant");
        const size_t afterAdd = proj.regions[0].nodeGraph.nodes.size();
        const int newId = proj.regions[0].nodeGraph.nodes.back().id;

        // The Constant was dropped at the canvas center; click its title bar (just inside the top-left)
        // to make it the imnodes selection without grabbing the Value drag below.
        ImGuiWindow *panel = ImGui::FindWindowByName(kCenterWin);
        IM_CHECK(panel != nullptr);
        const ImVec2 c = panel->Rect().GetCenter();
        ctx->MouseMoveToPos(ImVec2(c.x + 14.0f, c.y + 8.0f));
        ctx->MouseClick(ImGuiMouseButton_Left);
        ctx->Yield();

        getTestState().processingPanel->deleteSelected(proj, eq);
        ctx->Yield(2);

        IM_CHECK_EQ(proj.regions[0].nodeGraph.nodes.size(), afterAdd - 1);
        const auto &nodes = proj.regions[0].nodeGraph.nodes;
        IM_CHECK(std::none_of(nodes.begin(), nodes.end(), [newId](const auto &n) { return n.id == newId; }));
    };

    // Footer region-name input commits the edited name on deactivation.
    IM_REGISTER_TEST(e, "processing", "region_name_edit")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegion(ctx);
        auto &proj = *getTestState().project;

        ctx->ItemInput("Processing###video_player/##region_name");
        ctx->KeyCharsReplace("Intro");
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);
        IM_CHECK_STR_EQ(proj.regions[0].name.c_str(), "Intro");
    };

    // Save/Load Graph footer buttons push their events (the file dialog returns cancel in the test
    // build, so nothing is written) — assert the buttons exist and clicking them doesn't crash.
    IM_REGISTER_TEST(e, "processing", "graph_save_load_buttons")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegion(ctx);
        // The buttons now carry stable ###ids (savegraph/loadgraph) so a translated label can't move
        // them (the FileDialog they raise returns cancel in the test build).
        IM_CHECK(ctx->ItemExists("Processing###video_player/savegraph"));
        IM_CHECK(ctx->ItemExists("Processing###video_player/loadgraph"));
        ctx->ItemClick("Processing###video_player/savegraph");
        ctx->Yield(2);
        ctx->ItemClick("Processing###video_player/loadgraph");
        ctx->Yield(2);
        IM_CHECK(ImGui::GetCurrentContext() != nullptr);
    };

    // The Remap header button raises the axis-remap modal for the region's *current* graph (no file
    // load) and confirming it applies the chosen mapping.
    IM_REGISTER_TEST(e, "processing", "remap_button_opens_modal_and_applies")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegion(ctx);
        auto &proj = *getTestState().project;
        IM_CHECK(ctx->ItemExists("Processing###video_player/remapgraph"));

        ctx->ItemClick("Processing###video_player/remapgraph");
        ctx->Yield(2);
        IM_CHECK(proj.pendingGraphLoad.has_value()); // modal raised from the live graph

        // Default target for L0 is L0 (identity) → no duplicate, Load enabled. Confirming clears the
        // pending load and leaves the region on L0.
        ctx->SetRef("###ofsmodal");
        ctx->ItemClick("proc_remap_load");
        ctx->Yield(2);
        IM_CHECK_SILENT(!proj.pendingGraphLoad.has_value());
        IM_CHECK(proj.regions[0].axisRoles.test(static_cast<size_t>(ofs::StandardAxis::L0)));
    };

    // Yield until the axis has no in-flight eval (async job + one-frame result latency), capped so a
    // stuck job fails the test rather than hanging it.
    static const auto waitEvalIdle = [](ImGuiTestContext *ctx, ofs::StandardAxis role) {
        auto &proj = *getTestState().project;
        for (int i = 0; i < 240 && proj.axes[static_cast<size_t>(role)].pendingEval; ++i)
            ctx->Yield();
    };

    // The Recompute footer button exists and clicking it doesn't crash. It forces evaluation of every
    // axis the region drives via RequestAxisEvalEvent (###Recompute pins the id past the icon prefix).
    IM_REGISTER_TEST(e, "processing", "recompute_button_present_and_clicks")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegion(ctx);
        IM_CHECK(ctx->ItemExists("Processing###video_player/Recompute"));
        ctx->ItemClick("Processing###video_player/Recompute");
        waitEvalIdle(ctx, ofs::StandardAxis::L0);
        IM_CHECK(ImGui::GetCurrentContext() != nullptr);
    };

    // The Halt Auto-Eval checkbox flips the transient ScriptProject::autoEvalEnabled flag both ways.
    IM_REGISTER_TEST(e, "processing", "halt_checkbox_toggles_auto_eval")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegion(ctx);
        auto &proj = *getTestState().project;

        const bool initial = proj.autoEvalEnabled;
        ctx->ItemClick("Processing###video_player/autoEval");
        ctx->Yield();
        IM_CHECK_EQ(proj.autoEvalEnabled, !initial);

        ctx->ItemClick("Processing###video_player/autoEval");
        ctx->Yield();
        IM_CHECK_EQ(proj.autoEvalEnabled, initial);
    };

    // Engaging the halt clears the processed result so the timeline falls back to raw actions, and an
    // axis edit while halted does not recompute. The Recompute button still forces a fresh result.
    IM_REGISTER_TEST(e, "processing", "halt_clears_resolved_and_suppresses_auto_eval")->TestFunc =
        [](ImGuiTestContext *ctx) {
            setupRegion(ctx);
            auto &proj = *getTestState().project;
            auto &eq = *getTestState().eventQueue;
            const size_t l0 = static_cast<size_t>(ofs::StandardAxis::L0);
            IM_CHECK(proj.axes[l0].showInStrip); // fixture has L0; evaluateAxis needs it present

            // Establish a known processed result (auto-eval is on by default).
            eq.push(ofs::RequestAxisEvalEvent{ofs::StandardAxis::L0});
            waitEvalIdle(ctx, ofs::StandardAxis::L0);
            IM_CHECK(proj.axes[l0].resolved.has_value());

            // Halt: resolved is dropped immediately.
            ctx->ItemClick("Processing###video_player/autoEval");
            ctx->Yield();
            IM_CHECK(!proj.autoEvalEnabled);
            IM_CHECK(!proj.axes[l0].resolved.has_value());

            // An edit while halted must not recompute.
            eq.push(ofs::AxisModifiedEvent{ofs::StandardAxis::L0});
            ctx->Yield(4);
            IM_CHECK(proj.axes[l0].pendingEval == nullptr);
            IM_CHECK(!proj.axes[l0].resolved.has_value());

            // Manual Recompute forces a result despite the halt.
            ctx->ItemClick("Processing###video_player/Recompute");
            ctx->Yield(2);
            waitEvalIdle(ctx, ofs::StandardAxis::L0);
            IM_CHECK(proj.axes[l0].resolved.has_value());

            proj.autoEvalEnabled = true; // restore the global transient for later tests
        };

    // Releasing the halt re-runs evaluation so the processed result reflects edits made while halted.
    IM_REGISTER_TEST(e, "processing", "unhalt_recomputes")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegion(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        const size_t l0 = static_cast<size_t>(ofs::StandardAxis::L0);
        IM_CHECK(proj.axes[l0].showInStrip);

        // Halt clears resolved.
        eq.push(ofs::SetAutoEvalEnabledEvent{false});
        ctx->Yield(2);
        IM_CHECK(!proj.autoEvalEnabled);
        IM_CHECK(!proj.axes[l0].resolved.has_value());

        // Un-halt via the checkbox recomputes.
        ctx->ItemClick("Processing###video_player/autoEval");
        ctx->Yield(2);
        IM_CHECK(proj.autoEvalEnabled);
        waitEvalIdle(ctx, ofs::StandardAxis::L0);
        IM_CHECK(proj.axes[l0].resolved.has_value());
    };

    // Render a graph holding one of every node type in a single pass. The existing tests only ever
    // place a node or two, so most of renderNode's per-type bodies (Effect with/without a registered
    // def, math, Constant, a disabled PluginNode, embedded and file-backed Script nodes) never draw.
    // Installing the graph directly and yielding a few frames exercises all those branches at once;
    // nothing pushes an event, so the graph we install is exactly what renders.
    IM_REGISTER_TEST(e, "processing", "render_all_node_types")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegionInDefaultLayout(ctx);
        auto &proj = *getTestState().project;
        const auto &reg = *getTestState().effectRegistry;
        IM_CHECK(!proj.regions.empty());

        ofs::ProcessingNodeGraph g;
        int id = 0;
        auto place = [&](ofs::ProcessingGraphNode n) {
            n.id = ++id;
            n.posX = static_cast<float>((id % 4) * 200);
            n.posY = static_cast<float>((id / 4) * 170);
            g.nodes.push_back(std::move(n));
        };
        using T = ofs::GraphNodeType;
        place({.type = T::Input, .role = ofs::StandardAxis::L0});
        place({.type = T::Output, .role = ofs::StandardAxis::L0});
        place({.type = T::Constant});
        place({.type = T::Add});
        place({.type = T::Subtract});
        place({.type = T::Multiply});
        place({.type = T::Divide});
        place({.type = T::Discretize, .effect = {.params = {1.0f, 30.0f}}}); // keep-actions + Hz drag

        // Effect referencing a real registered effect, params sized to its defs so the param widgets draw.
        if (!reg.orderedKeys.empty()) {
            const std::string &key = reg.orderedKeys.front();
            ofs::ProcessingGraphNode eff;
            eff.type = T::Effect;
            eff.effect.type = key;
            eff.effect.params.assign(reg.effects.at(key).paramDefs.size(), 0.0f);
            place(std::move(eff));
        }
        // Effect with an unknown type → the def == nullptr rendering branch.
        {
            ofs::ProcessingGraphNode eff;
            eff.type = T::Effect;
            eff.effect.type = "__no_such_effect__";
            place(std::move(eff));
        }
        // Disabled plugin node (none registered) → the grayed disabled branch.
        {
            ofs::ProcessingGraphNode pn;
            pn.type = T::PluginNode;
            pn.pluginNodeId = "ghost.node";
            pn.pluginInputCount = 1; // one input
            place(std::move(pn));
        }
        // Embedded script node (carries its own source) → embedded branch + save/load buttons.
        {
            ofs::ProcessingGraphNode sn;
            sn.type = T::Script;
            sn.scriptEmbeddedSource = "// !ofs:signal functional\nreturn a;\n";
            sn.scriptFile = "embed.cs";
            sn.scriptInputCount = 1;
            place(std::move(sn));
        }
        // File-backed script node, uncompiled → the file-combo UI + "compiling" status, two input pins.
        {
            ofs::ProcessingGraphNode sn;
            sn.type = T::Script;
            sn.scriptFile = "missing.cs";
            sn.scriptInputCount = 2;
            place(std::move(sn));
        }

        g.nextId = id + 1;
        const size_t count = g.nodes.size();
        proj.regions[0].nodeGraph = std::move(g);
        ctx->Yield(3); // render the populated graph

        IM_CHECK(ImGui::GetCurrentContext() != nullptr);
        IM_CHECK_EQ(proj.regions[0].nodeGraph.nodes.size(), count); // nothing mutated it
    };

    // Add a node through the add-menu WITHOUT typing a filter. Every existing add test types a filter
    // immediately, so the unfiltered submenu branch (Math / Generate / Modify / Combine / plugin menus)
    // is never entered. Opening the popup unfiltered builds those menus; navigating Math → Constant then
    // exercises the unfiltered create path.
    IM_REGISTER_TEST(e, "processing", "add_node_via_unfiltered_menu")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegionInDefaultLayout(ctx);
        auto &proj = *getTestState().project;
        IM_CHECK(!proj.regions.empty());
        const size_t before = proj.regions[0].nodeGraph.nodes.size();

        ImGuiWindow *panel = ImGui::FindWindowByName(kCenterWin);
        IM_CHECK(panel != nullptr);
        ctx->MouseMoveToPos(panel->Rect().GetCenter());
        ctx->Yield();
        IM_CHECK(panelUnderCursor());
        ctx->MouseClick(ImGuiMouseButton_Right);
        ctx->Yield(2);
        IM_CHECK(anyPopupOpen());

        // No KeyCharsReplace: stay in the unfiltered menu tree. Open Math, then click Constant.
        ctx->ItemClick((std::string("**/") + ICON_CALCULATOR + "  Math").c_str());
        ctx->Yield(2);
        ctx->ItemClick((std::string("**/") + ICON_HASH + "  Constant").c_str());
        ctx->Yield(2);

        IM_CHECK_EQ(proj.regions[0].nodeGraph.nodes.size(), before + 1);
        IM_CHECK(proj.regions[0].nodeGraph.nodes.back().type == ofs::GraphNodeType::Constant);
    };

    // The "New Script…" add-menu entry raises a modal (name + signal + inputs). Open it and Cancel:
    // this renders the whole modal form (the big uncovered block) without writing a stub or launching
    // an external editor (the Create path does both, so it's deliberately not exercised here).
    IM_REGISTER_TEST(e, "processing", "new_script_modal_opens_and_cancels")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegionInDefaultLayout(ctx);
        auto &proj = *getTestState().project;
        IM_CHECK(!proj.regions.empty());
        const size_t before = proj.regions[0].nodeGraph.nodes.size();

        ImGuiWindow *panel = ImGui::FindWindowByName(kCenterWin);
        IM_CHECK(panel != nullptr);
        ctx->MouseMoveToPos(panel->Rect().GetCenter());
        ctx->Yield();
        IM_CHECK(panelUnderCursor());
        ctx->MouseClick(ImGuiMouseButton_Right);
        ctx->Yield(2);
        IM_CHECK(anyPopupOpen());

        // Filter to Script so the "Script…" item is shown, then click it to raise the modal.
        ctx->KeyCharsReplace("Script");
        ctx->Yield(2);
        ctx->ItemClick((std::string("**/") + ICON_BRACES + "  Script\xe2\x80\xa6").c_str());
        ctx->Yield(3); // modal opens next frame

        // Type a name (renders the "creates <file>" status row), then Cancel — no stub is written.
        ctx->ItemInput("**/##scriptname");
        ctx->KeyCharsReplace("UiTestScript");
        ctx->Yield();
        ctx->ItemClick("**/###proc_cancel");
        ctx->Yield(2);

        IM_CHECK_EQ(proj.regions[0].nodeGraph.nodes.size(), before); // cancelled → no node added
    };

    // The Delete command (edit.remove-action) deletes the selected region when nothing in the graph is
    // selected. Drives the real command lambda via CommandRegistry::run — the exact path the Delete key
    // fires — so the OfsApp routing (region selected → DeleteRegionEvent) is what's under test. No graph
    // node is selected, so deleteSelected() reports "nothing consumed" and the command falls through to
    // dropping the region.
    IM_REGISTER_TEST(e, "processing", "delete_command_removes_selected_region")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegion(ctx);
        auto &proj = *getTestState().project;
        IM_CHECK_EQ(proj.regions.size(), static_cast<size_t>(1));
        const int selId = proj.procSelRegionId;
        IM_CHECK(selId != -1); // CreateRegionEvent auto-selected it

        getTestState().commandRegistry->run("edit.remove-action");
        ctx->Yield(2);

        IM_CHECK(proj.regions.empty());        // the selected region is gone
        IM_CHECK_EQ(proj.procSelRegionId, -1); // selection cleared with it
    };

    // Priority: when the graph is focused with a node selected, the Delete command removes that node and
    // leaves the region intact — node deletion is more specific than dropping the whole region. Clicking
    // a placed node focuses the graph and makes it the imnodes selection, so deleteSelected() consumes
    // the delete and the command returns before reaching the region-delete branch.
    IM_REGISTER_TEST(e, "processing", "delete_command_with_node_selected_keeps_region")->TestFunc =
        [](ImGuiTestContext *ctx) {
            setupRegionInDefaultLayout(ctx);
            auto &proj = *getTestState().project;

            addNodeViaMenu(ctx, "Constant", std::string("**/") + ICON_HASH + "  Constant");
            const size_t afterAdd = proj.regions[0].nodeGraph.nodes.size();
            const int newId = proj.regions[0].nodeGraph.nodes.back().id;

            // Click the node's title bar to select it (and focus the graph) without grabbing the Value drag.
            ImGuiWindow *panel = ImGui::FindWindowByName(kCenterWin);
            IM_CHECK(panel != nullptr);
            const ImVec2 c = panel->Rect().GetCenter();
            ctx->MouseMoveToPos(ImVec2(c.x + 14.0f, c.y + 8.0f));
            ctx->MouseClick(ImGuiMouseButton_Left);
            ctx->Yield();
            IM_CHECK(getTestState().processingPanel->isGraphFocused()); // precondition for the node branch

            const int selId = proj.procSelRegionId;
            getTestState().commandRegistry->run("edit.remove-action");
            ctx->Yield(2);

            IM_CHECK_EQ(proj.regions.size(), static_cast<size_t>(1)); // region survives
            IM_CHECK_EQ(proj.procSelRegionId, selId);                 // still selected
            const auto &nodes = proj.regions[0].nodeGraph.nodes;
            IM_CHECK_EQ(nodes.size(), afterAdd - 1); // only the node was removed
            IM_CHECK(std::none_of(nodes.begin(), nodes.end(), [newId](const auto &n) { return n.id == newId; }));
        };

    // Escape closes the panel even when the panel is NOT the focused window. Selecting a region (here
    // via the event a region-bar click pushes) opens the panel but leaves focus on the timeline, so the
    // close must not depend on panel focus. Same "panel closed" signal the click-away test asserts.
    IM_REGISTER_TEST(e, "processing", "escape_closes_panel")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegion(ctx);
        auto &proj = *getTestState().project;
        IM_CHECK(proj.procSelRegionId != -1); // CreateRegionEvent auto-selected it

        // Focus a sibling window so the panel renders unfocused — the reported scenario.
        ctx->WindowFocus("Timeline###timeline");
        ctx->Yield();
        ImGuiWindow *nav = ImGui::GetCurrentContext()->NavWindow;
        ImGuiWindow *panel = ImGui::FindWindowByName(kCenterWin);
        IM_CHECK(nav == nullptr || nav->RootWindow != panel); // precondition: panel isn't focused

        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);

        IM_CHECK_EQ(proj.procSelRegionId, -1);
    };

    // A node's measured width must not creep upward when it scrolls off the LEFT edge of the canvas.
    // Output labels used to right-align to the node's own previous-frame ImNodes::GetNodeDimensions,
    // making the rendered width monotonic (it can only hold or grow). On-screen that is a stable fixed
    // point, but once the node's content crosses into negative window-relative coordinates ImGui's
    // IM_TRUNC line-start rounding flips from floor to truncate-toward-zero, so each frame measures the
    // right-aligned label a little wider, the feedback latches it in, and the node grows without bound.
    // Width now anchors to an intrinsic constant, independent of position.
    //
    // In the app this is what dragging any node does to the *others*: dragging off the canvas edge makes
    // imnodes auto-pan the camera, the panel feeds the pan back into every untouched node's stored
    // position (syncPositions), and nodes pushed off the left edge grow — Math nodes especially, since
    // (unlike the title-pinned Input/Output nodes) their narrow title lets the output mirror drive width.
    // The test-engine mouse can't drive auto-pan, so we reproduce it directly by walking a Math node off
    // the left edge a few pixels per frame.
    IM_REGISTER_TEST(e, "processing", "node_width_stable_offscreen_left")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegionInDefaultLayout(ctx);
        auto &proj = *getTestState().project;

        // A Math node has a narrow title and a right-aligned output with no wide, stable body anchor, so
        // the output mirror dominates its width (Input/Output nodes are title-pinned and never drift).
        addNodeViaMenu(ctx, "Add", std::string("**/") + ICON_PLUS + "  Add  (A + B)");
        const int nodeId = proj.regions[0].nodeGraph.nodes.back().id;
        const float baseX = proj.regions[0].nodeGraph.nodes.back().posX;

        // Fractional node padding mimics a non-100% DPI scale (applyDpiToImNodes multiplies the themed
        // base by the display scale); at integer padding the rounding had nothing to round away and the
        // bug stayed latent. Process-global imnodes state — restore it so later tests are unaffected.
        const ImVec2 savedPadding = ImNodes::GetStyle().NodePadding;
        ImNodes::GetStyle().NodePadding = ImVec2(10.5f, 8.0f);

        ctx->Yield(4); // let the node settle to its steady-state width
        const float w0 = ImNodes::GetNodeDimensions(ofs::GraphId::nodeBody(nodeId)).x;

        // Walk the node leftward past the canvas origin so its content crosses x = 0 into negative
        // window-relative coordinates — the regime where the truncate-toward-zero rounding bites. The
        // fractional step keeps it sweeping rounding boundaries the whole way.
        for (int i = 1; i <= 200; ++i) {
            for (auto &n : proj.regions[0].nodeGraph.nodes)
                if (n.id == nodeId)
                    n.posX = baseX - static_cast<float>(i) * 7.137f;
            ctx->Yield();
        }

        const float w1 = ImNodes::GetNodeDimensions(ofs::GraphId::nodeBody(nodeId)).x;
        ImNodes::GetStyle().NodePadding = savedPadding;

        // Rendering in negative coordinates jitters the width by ~1px regardless (inherent ImGui rounding,
        // bounded). The regression instead grew with distance off-screen — tens of px here and climbing —
        // so a few px of slack cleanly separates the fixed, bounded behaviour from the unbounded feedback.
        IM_CHECK_LE(w1, w0 + 4.0f);
    };

    // Escape while a footer InputText is being edited must cancel the edit (ImGui's own behavior), NOT
    // close the panel — the close guard samples the active-item state before the InputText consumes and
    // deactivates on Escape, so the same keypress can't also fall through to closing.
    IM_REGISTER_TEST(e, "processing", "escape_during_name_edit_keeps_panel")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegion(ctx);
        auto &proj = *getTestState().project;
        const int selId = proj.procSelRegionId;
        IM_CHECK(selId != -1);

        ctx->ItemInput("Processing###video_player/##region_name");
        ctx->KeyCharsReplace("Temp");
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_Escape); // cancels the edit; panel stays open
        ctx->Yield(2);

        IM_CHECK_EQ(proj.procSelRegionId, selId);
    };

    // Fit button: shove the whole graph far off-canvas (preserving its internal layout), click Fit, and
    // confirm the nodes are pulled back near the visible canvas with their relative spacing intact.
    // imnodes has no zoom, so framing is a pure translation via MoveRegionNodesEvent.
    IM_REGISTER_TEST(e, "processing", "fit_button_recenters_graph")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegionInDefaultLayout(ctx);
        auto &proj = *getTestState().project;
        auto &g = proj.regions[0].nodeGraph;
        IM_CHECK(g.nodes.size() >= 2);

        const float spacing = g.nodes[1].posX - g.nodes[0].posX;
        for (auto &n : g.nodes) {
            n.posX += 4000.0f;
            n.posY += 4000.0f;
        }
        ctx->Yield(2);

        ctx->ItemClick("Processing###video_player/fitgraph");
        ctx->Yield(3);

        float maxX = 0.0f;
        for (const auto &n : g.nodes)
            maxX = std::max(maxX, n.posX);
        IM_CHECK_LT(maxX, 2000.0f);                                                 // pulled back from +4000
        IM_CHECK_LT(std::abs((g.nodes[1].posX - g.nodes[0].posX) - spacing), 1.0f); // layout preserved
    };

    // Node context menu → Delete removes the right-clicked node (and its links). Uses a Constant since
    // Input/Output are delete-protected.
    IM_REGISTER_TEST(e, "processing", "node_context_menu_deletes_node")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegionInDefaultLayout(ctx);
        auto &proj = *getTestState().project;
        addNodeViaMenu(ctx, "Constant", std::string("**/") + ICON_HASH + "  Constant");
        const size_t afterAdd = proj.regions[0].nodeGraph.nodes.size();
        const int newId = proj.regions[0].nodeGraph.nodes.back().id;

        rightClickNode(ctx, newId);
        IM_CHECK(anyPopupOpen());
        ctx->ItemClick("**/proc_node_del");
        ctx->Yield(2);

        const auto &nodes = proj.regions[0].nodeGraph.nodes;
        IM_CHECK_EQ(nodes.size(), afterAdd - 1);
        IM_CHECK(std::none_of(nodes.begin(), nodes.end(), [newId](const auto &n) { return n.id == newId; }));
    };

    // Node context menu → Duplicate adds an unlinked copy of the same type.
    IM_REGISTER_TEST(e, "processing", "node_context_menu_duplicates_node")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegionInDefaultLayout(ctx);
        auto &proj = *getTestState().project;
        addNodeViaMenu(ctx, "Constant", std::string("**/") + ICON_HASH + "  Constant");
        const size_t afterAdd = proj.regions[0].nodeGraph.nodes.size();
        const int origId = proj.regions[0].nodeGraph.nodes.back().id;

        rightClickNode(ctx, origId);
        IM_CHECK(anyPopupOpen());
        ctx->ItemClick("**/proc_node_dup");
        ctx->Yield(2);

        const auto &nodes = proj.regions[0].nodeGraph.nodes;
        IM_CHECK_EQ(nodes.size(), afterAdd + 1);
        IM_CHECK(nodes.back().type == ofs::GraphNodeType::Constant);
        IM_CHECK(nodes.back().id != origId);
    };

    // Node context menu → Disconnect drops every link touching the node but keeps the node. The default
    // graph wires Input→Output, so disconnecting the Input clears the region's only link.
    IM_REGISTER_TEST(e, "processing", "node_context_menu_disconnects_node")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegionInDefaultLayout(ctx);
        auto &proj = *getTestState().project;
        auto &g = proj.regions[0].nodeGraph;
        IM_CHECK(!g.links.empty());        // default Input→Output link
        const int inputId = g.nodes[0].id; // buildDefaultGraph pushes Input first
        const size_t nodeCount = g.nodes.size();

        rightClickNode(ctx, inputId);
        IM_CHECK(anyPopupOpen());
        ctx->ItemClick("**/proc_node_disc");
        ctx->Yield(2);

        IM_CHECK(proj.regions[0].nodeGraph.links.empty());
        IM_CHECK_EQ(proj.regions[0].nodeGraph.nodes.size(), nodeCount); // node kept
    };

    // Link context menu → Delete removes the right-clicked link but keeps both endpoint nodes. The
    // default graph wires Input→Output, giving exactly one link to target.
    IM_REGISTER_TEST(e, "processing", "link_context_menu_deletes_link")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegionInDefaultLayout(ctx);
        auto &proj = *getTestState().project;
        auto &g = proj.regions[0].nodeGraph;
        IM_CHECK_EQ(g.links.size(), size_t(1));
        const int fromId = g.nodes[0].id; // Input
        const int toId = g.nodes[1].id;   // Output
        const size_t nodeCount = g.nodes.size();

        IM_CHECK(rightClickLink(ctx, fromId, toId));
        IM_CHECK(anyPopupOpen());
        ctx->ItemClick("**/proc_link_del");
        ctx->Yield(2);

        IM_CHECK(proj.regions[0].nodeGraph.links.empty());
        IM_CHECK_EQ(proj.regions[0].nodeGraph.nodes.size(), nodeCount); // endpoints kept
    };

    // The F shortcut fits the graph, and a non-empty selection frames only the selected nodes. Selecting
    // just the Output node and fitting must center *it*, not the Input→Output midpoint — so the Output
    // node lands further left than a whole-graph fit, which leaves it in the right half of the view.
    IM_REGISTER_TEST(e, "processing", "fit_key_frames_selection")->TestFunc = [](ImGuiTestContext *ctx) {
        setupRegionInDefaultLayout(ctx);
        auto &proj = *getTestState().project;
        auto &g = proj.regions[0].nodeGraph;
        IM_CHECK(g.nodes.size() >= 2);
        const int outId = g.nodes[1].id; // Output (buildDefaultGraph pushes Input then Output)

        // Hover the canvas so the graph is focused (F is focus-gated), nudge the nodes off-center so the
        // fit definitely moves them, then fit the whole graph as the baseline.
        ImGuiWindow *panel = ImGui::FindWindowByName(kCenterWin);
        ctx->MouseMoveToPos(panel->Rect().GetCenter());
        for (auto &n : g.nodes)
            n.posX += 1500.0f;
        ImNodes::ClearNodeSelection();
        ctx->Yield(2);
        ctx->KeyPress(ImGuiKey_F);
        ctx->Yield(3);
        const float outAllFit = g.nodes[1].posX;

        // Now frame only the Output node and fit again. Its center moves onto the canvas center, left of
        // where the whole-graph fit parked it.
        ImNodes::SelectNode(ofs::GraphId::nodeBody(outId));
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_F);
        ctx->Yield(3);
        const float outSelFit = g.nodes[1].posX;

        IM_CHECK_LT(outSelFit, outAllFit);
    };
}
