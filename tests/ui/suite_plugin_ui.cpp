// Plugin <-> live-UI integration tests. These drive the real C# Ofs.TestPlugin through the app's single
// PluginManager (the one OfsApp owns — there is no second manager in the process), exercising the actual
// command palette, Shortcut window and Processing panel.
//
// The plugin is loaded on demand (see ensurePluginLoaded), not at startup, so every other suite runs
// plugin-free; this suite is registered LAST so nothing runs after it with the plugin loaded. loadPlugins()
// runs on the main thread (via the GuiFunc gate) because the plugin's OnLoad calls registerCommand, which
// the host guards to the thread that booted the .NET runtime.
//
// .NET and the built test plugin are mandatory: a test that can't load the plugin fails (IM_CHECK),
// it never silently skips.

#include "Core/Events.h"
#include "Core/IntentEvents.h" // EditRequestEvent / SetActiveEditModeEvent ÔÇö drive Ofs.Core's edit mode
#include "Core/ProcessingRegion.h"
#include "Core/ProjectLifecycleEvents.h"
#include "Core/ScriptAxisAction.h"
#include "Core/ScriptProject.h"
#include "Core/StandardAxis.h"
#include "Core/VectorSet.h"
#include "Format/AppSettings.h"
#include "Localization/Translator.h" // active ISO 639 code ├ö├ç├Â the language signal Ofs.Core follows
#include "Services/BindingSystem.h"
#include "Services/CommandRegistry.h"
#include "Services/EditModeRegistry.h" // kNativeEditModeId
#include "Services/EffectRegistry.h"
#include "Services/NavigatorRegistry.h" // kFollowOverlayNavigatorId
#include "Services/PluginApi.h"
#include "Services/PluginEvents.h"
#include "Services/PluginManager.h"
#include "Services/ScriptNodeEvents.h"
#include "Services/ScriptRegistry.h"
#include "Services/SelectionModeRegistry.h" // kNativeSelectionModeId
#include "UI/Icons.h"
#include "UI/ModalManager.h" // setNativeDialogOverrideForTesting ├ö├ç├Â drive the plugin's non-blocking file dialog
#include "Util/PathUtil.h"
#include "helpers/TestState.h"
#include <SDL3/SDL_keycode.h>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <imgui_te_context.h>
#include <imgui_te_engine.h>
#include <initializer_list>
#include <nlohmann/json.hpp> // seed Ofs.Core's project-scoped panel settings for the edit-mode test
#include <string>

using namespace ofs;

namespace {

namespace fs = std::filesystem;

constexpr const char *kPluginName = "Ofs.TestPlugin";
constexpr const char *kCmdSeek = "Ofs.TestPlugin.seekmark";     // bindable, seeks to second 7 when invoked
constexpr int kSeekSecond = 7;                                  // matches Host.Player.Seek(7.0) in TestPlugin.cs
constexpr const char *kCmdNudge = "Ofs.TestPlugin.nudge";       // bindable, no side-effect
constexpr const char *kCmdHidden = "Ofs.TestPlugin.hiddenseek"; // bindable but inPalette:false, seeks to 6
constexpr int kHiddenSecond = 6;                                // matches Host.Player.Seek(6.0) in TestPlugin.cs
// Ui-guard probe commands (Ui.cs thread/freshness guard + Dialogs.cs cancellation). Each seeks a distinct
// second iff the guard rejected the deliberate misuse — see the seek constants in TestPlugin.cs.
constexpr const char *kCmdUiOffPass = "Ofs.TestPlugin.uioffpass";           // off-pass Ui call rejected → seeks 94
constexpr const char *kCmdUiOffThread = "Ofs.TestPlugin.uioffthread";       // off-thread Ui call rejected → seeks 95
constexpr const char *kCmdUiDialogCancel = "Ofs.TestPlugin.uidialogcancel"; // cancelled dialog task → seeks 98
constexpr const char *kNodeFgen = "Ofs.TestPlugin.fgen";                    // functional generator node
constexpr const char *kNodeStateOffset = "Ofs.TestPlugin.stateoffset"; // TState modifier: input + Offset (ui slider)
constexpr const char *kNodeCustomBump =
    "Ofs.TestPlugin.custombump"; // TState modifier: custom `ui` callback (buttons + deferred Update)
// The plugin's own UI window: ImGui::Begin("<displayName>###<name>") in PluginManager::renderUI — the
// "###" pins the window id to the DLL stem, so the ref must use it too (the visible label is ignored).
constexpr const char *kPluginWindow = "Test Plugin###Ofs.TestPlugin";

// Handshake between a test (producer, runs on the imgui_test_engine coroutine thread) and the GuiFunc
// (consumer, runs on the main thread). loadPlugins() must run on the main thread: the plugin's OnLoad
// calls registerCommand, which the host guards to the thread that booted the .NET runtime.
bool g_loadWanted = false;
bool g_loadDone = false;

// Copy the built test plugin into <pref>/plugins (the only root loadPlugins() scans). Empty when the
// plugin wasn't staged next to the binary → the caller fails (the plugin is required).
fs::path stagePlugin() {
    std::error_code ec;
    const fs::path src = ofs::util::getBasePath() / "plugins" / "Ofs.TestPlugin";
    const fs::path dst = ofs::util::getPrefPath() / "plugins" / "Ofs.TestPlugin";
    if (!fs::exists(src))
        return {};
    fs::create_directories(dst.parent_path(), ec);
    fs::copy(src, dst, fs::copy_options::recursive, ec);
    return dst / "Ofs.TestPlugin.dll";
}

// The real shipped first-party plugin whose editing/selection commands the core_plugin tests drive.
constexpr const char *kCorePluginName = "Ofs.Core";
constexpr const char *kCmdEqualize = "Ofs.Core.equalize";
constexpr const char *kCmdInvert = "Ofs.Core.invert";
constexpr const char *kCmdIsolate = "Ofs.Core.isolate";
constexpr const char *kCmdRepeat = "Ofs.Core.repeat-stroke";

// Native host-owned selection commands, routed through SelectIntentRouter.
constexpr const char *kCmdSelectAllLeft = "edit.select-all-left";
constexpr const char *kCmdSelectAllRight = "edit.select-all-right";

// Make Ofs.Core loadable in the same loadPlugins() the test plugin triggers. Ofs.Core is first-party,
// so the loader only picks it up from the BASE plugins root (getBasePath()/managed/plugins) by name —
// the user-root path the test plugin uses ignores first-party names. It is staged at build time into
// core-plugin/ (never scanned), so copying it into the base root here is what makes it visible; doing
// it just before the one loadPlugins() call keeps it out of the startup scan. A no-op when the source
// isn't present; the core_plugin tests then fail at ensureCoreLoaded (Ofs.Core is required).
void stageCorePlugin() {
    std::error_code ec;
    const fs::path src = ofs::util::getBasePath() / "core-plugin" / "Ofs.Core";
    const fs::path dst = ofs::util::getBasePath() / "managed" / "plugins" / "Ofs.Core";
    if (!fs::exists(src))
        return;
    fs::create_directories(dst.parent_path(), ec);
    fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
}

void pluginGateGuiFunc(ImGuiTestContext *) {
    if (g_loadWanted && !g_loadDone) {
        if (getTestState().pluginManager != nullptr)
            getTestState().pluginManager->loadPlugins(); // main thread
        g_loadDone = true;
    }
}

// Lazily load the real plugin into the live app the first time a plugin_ui test needs it. .NET and the
// built test plugin are mandatory, so a caller IM_CHECKs the result — false (failed to load) is a test
// failure, never a skip.
bool ensurePluginLoaded(ImGuiTestContext *ctx) {
    auto &cmd = *getTestState().commandRegistry;
    if (cmd.find(kCmdSeek) != nullptr)
        return true; // already loaded by an earlier test in this suite
    if (stagePlugin().empty())
        return false;
    stageCorePlugin(); // also make Ofs.Core loadable in this one load (core tests require it)
    g_loadWanted = true;
    g_loadDone = false;
    for (int i = 0; i < 120 && !g_loadDone; ++i)
        ctx->Yield();
    ctx->Yield(3); // let the RegisterPluginNodeEvent(s) the plugin pushed during OnLoad drain
    return cmd.find(kCmdSeek) != nullptr;
}

// True once Ofs.Core's commands are live (it rides the same loadPlugins() as the test plugin). Ofs.Core
// is a first-party plugin always built alongside the suite, so a caller IM_CHECKs this — a false result
// is a hard failure.
bool ensureCoreLoaded(ImGuiTestContext *ctx) {
    if (!ensurePluginLoaded(ctx))
        return false;
    return getTestState().commandRegistry->find(kCmdEqualize) != nullptr;
}

// L0 is the fixture's active axis. Replace its actions with a known set.
void seedL0(ImGuiTestContext *ctx, std::initializer_list<ofs::ScriptAxisAction> pts) {
    ofs::VectorSet<ofs::ScriptAxisAction> actions;
    for (const auto &p : pts)
        actions.insert(p);
    getTestState().eventQueue->push(ofs::CommitAxisActionsEvent{.axis = ofs::StandardAxis::L0, .actions = actions});
    ctx->Yield(2);
}

// Set L0's selection to every action in [start, end] (the same path the box-select UI drives).
void selectL0Range(ImGuiTestContext *ctx, double start, double end) {
    getTestState().eventQueue->push(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::Box,
                                                            .axis = ofs::StandardAxis::L0,
                                                            .startTime = start,
                                                            .endTime = end,
                                                            .additive = false});
    ctx->Yield(2);
}

ofs::AxisState &l0Axis() {
    return getTestState().project->axes[static_cast<size_t>(ofs::StandardAxis::L0)];
}

bool hasAction(const ofs::AxisState &axis, double at) {
    return axis.actions.contains(ofs::ScriptAxisAction{at, 0});
}

bool isSelected(const ofs::AxisState &axis, double at) {
    return axis.selection.contains(ofs::ScriptAxisAction{at, 0});
}

// Position of the action at time `at`, or -1 if none.
int posAt(const ofs::AxisState &axis, double at) {
    auto it = axis.actions.find(ofs::ScriptAxisAction{at, 0});
    return it != axis.actions.end() ? it->pos : -1;
}

// Bind a private chord to a plugin command and fire it through the real key-dispatch path (the same
// path a user's keypress takes). Distinctive Ctrl+Alt+Shift chords so they can't alias native or
// earlier-seeded bindings.
void invokeCore(ImGuiTestContext *ctx, const char *commandId, SDL_Keycode key) {
    constexpr SDL_Keymod kMods = SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_SHIFT;
    auto &binding = *getTestState().bindingSystem;
    binding.addBinding({.trigger = ofs::KeyChord{.key = key, .modifiers = kMods}, .commandId = commandId});
    getTestState().eventQueue->push(ofs::KeyDownEvent{.key = key, .modifiers = kMods, .repeat = false});
    ctx->Yield(3); // KeyDown -> run -> managed handler pushes Commit/selection events -> next drain applies them
}

bool anyPopupOpen() {
    return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

// ── Crash-safety helpers ─────────────────────────────────────────────────────────────────────────
// The faulty entry points the plugin_crash tests drive. Each throws on invocation; the host must
// catch at the native↔managed boundary and survive (see PluginGuard / the node trampolines).
constexpr const char *kCmdBoom = "Ofs.TestPlugin.boom";          // command handler throws
constexpr const char *kNodeBoomMod = "Ofs.TestPlugin.boommod";   // functional modifier throws → pass-through
constexpr const char *kNodeBoomDGen = "Ofs.TestPlugin.boomdgen"; // discrete generator throws → emits nothing

// Create a single L0 region over [0,10] and return its id (procSelRegionId, set by onCreateRegion).
int createL0Region(ImGuiTestContext *ctx) {
    getTestState().eventQueue->push(
        ofs::CreateRegionEvent{.axisRole = ofs::StandardAxis::L0, .startTime = 0.0, .endTime = 10.0});
    ctx->Yield(3);
    return getTestState().project->procSelRegionId;
}

// Replace region `id`'s graph (keeping its time span / axes) and let the resulting re-eval run.
void setRegionGraph(ImGuiTestContext *ctx, int id, const ofs::ProcessingNodeGraph &graph) {
    const ofs::ProcessingRegion *cur = getTestState().project->findRegion(id);
    IM_CHECK(cur != nullptr);
    ofs::ProcessingRegion updated = *cur; // preserve start/end/hz/axes; swap only the graph
    updated.nodeGraph = graph;
    getTestState().eventQueue->push(ofs::ModifyRegionEvent{.regionId = id, .updatedRegion = updated, .snapshot = true});
    ctx->Yield(3);
}

// Spin frames until L0's async eval settles (pendingEval cleared). Returns false on timeout.
bool waitForRegionEval(ImGuiTestContext *ctx, int maxFrames = 600) {
    auto &axis = l0Axis();
    for (int i = 0; i < maxFrames && axis.pendingEval != nullptr; ++i)
        ctx->Yield();
    return axis.pendingEval == nullptr;
}

// Input(L0) → PluginNode(modifier) → Output(L0). The node's kind/signal come from the registry at
// eval time; the cached pluginInputCount/pluginSignal here only affect persistence/rendering.
ofs::ProcessingNodeGraph pluginModGraph(const char *nodeId) {
    ofs::ProcessingNodeGraph g;
    const int inId = g.allocId();
    const int nId = g.allocId();
    const int outId = g.allocId();
    g.nodes.push_back({.id = inId, .type = ofs::GraphNodeType::Input, .role = ofs::StandardAxis::L0});
    ofs::ProcessingGraphNode n;
    n.id = nId;
    n.type = ofs::GraphNodeType::PluginNode;
    n.pluginNodeId = nodeId;
    n.pluginInputCount = 1;
    n.pluginSignal = static_cast<uint8_t>(ofs::OfsSignalFunctional);
    n.role = ofs::StandardAxis::L0;
    g.nodes.push_back(n);
    g.nodes.push_back({.id = outId, .type = ofs::GraphNodeType::Output, .role = ofs::StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = inId, .toNode = nId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = nId, .toNode = outId, .toPin = 0});
    return g;
}

// PluginNode(discrete generator) → Output(L0). No input — a pure generator.
ofs::ProcessingNodeGraph pluginGenGraph(const char *nodeId) {
    ofs::ProcessingNodeGraph g;
    const int nId = g.allocId();
    const int outId = g.allocId();
    ofs::ProcessingGraphNode n;
    n.id = nId;
    n.type = ofs::GraphNodeType::PluginNode;
    n.pluginNodeId = nodeId;
    n.pluginInputCount = 0;
    n.pluginSignal = static_cast<uint8_t>(ofs::OfsSignalDiscrete);
    n.role = ofs::StandardAxis::L0;
    g.nodes.push_back(n);
    g.nodes.push_back({.id = outId, .type = ofs::GraphNodeType::Output, .role = ofs::StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = nId, .toNode = outId, .toPin = 0});
    return g;
}

// Input(L0) → Script(file, functional modifier) → Output(L0).
ofs::ProcessingNodeGraph scriptModGraph(const std::string &file) {
    ofs::ProcessingNodeGraph g;
    const int inId = g.allocId();
    const int sId = g.allocId();
    const int outId = g.allocId();
    g.nodes.push_back({.id = inId, .type = ofs::GraphNodeType::Input, .role = ofs::StandardAxis::L0});
    ofs::ProcessingGraphNode s;
    s.id = sId;
    s.type = ofs::GraphNodeType::Script;
    s.scriptFile = file;
    s.scriptInputCount = 1;
    s.role = ofs::StandardAxis::L0;
    g.nodes.push_back(s);
    g.nodes.push_back({.id = outId, .type = ofs::GraphNodeType::Output, .role = ofs::StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = inId, .toNode = sId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = sId, .toNode = outId, .toPin = 0});
    return g;
}

// Seed L0 with a lone {1.0, 25} action — the input every crash graph runs the faulty node against —
// and let the eval it triggers run to completion. Returns false if the eval never settles.
bool seedAndEvalL0(ImGuiTestContext *ctx) {
    seedL0(ctx, {{1.0, 25}});
    return waitForRegionEval(ctx);
}

} // namespace

void RegisterPluginUiTests(ImGuiTestEngine *e) {
    // 1) A plugin-registered command appears in the live command palette and invokes end-to-end through
    //    the managed bridge: "Seek Mark" runs Host.Player.Seek(7), observable as cursorPos -> 7s.
    ImGuiTest *t1 = IM_REGISTER_TEST(e, "plugin_ui", "command_palette_invokes_plugin_command");
    t1->GuiFunc = pluginGateGuiFunc;
    t1->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        eq.push(ofs::SeekEvent{2.0});
        ctx->Yield(2);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 2);

        eq.push(ofs::OpenCommandPaletteEvent{});
        ctx->Yield(3);
        IM_CHECK(anyPopupOpen());

        ctx->KeyCharsReplace("seek mark");
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(3);

        // Only the whole-second portion matters; the managed handler ran iff the cursor jumped to 7.
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), kSeekSecond);
    };

    // 1b) Plugin availability follows the project lifecycle: with a project the plugin's UI window is up;
    //     closing the project (→ welcome screen) makes it unavailable; reopening restores it. The plugin
    //     DLL stays loaded throughout — what tracks the project is the UI/command availability, since
    //     pluginManager->renderUI() only runs inside the editor body.
    ImGuiTest *t1b = IM_REGISTER_TEST(e, "plugin_ui", "plugin_unavailable_without_project");
    t1b->GuiFunc = pluginGateGuiFunc;
    t1b->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        auto pluginVisible = [&] { return ctx->WindowInfo(kPluginWindow, ImGuiTestOpFlags_NoError).Window != nullptr; };

        ctx->Yield(3);
        IM_CHECK(pluginVisible()); // plugin UI window up with a project

        // Close to the welcome screen (clearDirtyFlags so the close runs without the save prompt).
        proj.clearDirtyFlags();
        eq.push(ofs::CloseProjectRequestEvent{});
        ctx->Yield(3);
        IM_CHECK(!pluginVisible()); // welcome screen: plugin UI unavailable

        loadFixture(ctx); // restore the project (and the plugin window) for later tests
        ctx->Yield(3);
        IM_CHECK(pluginVisible());
    };

    // 2) A plugin's bindable command surfaces in the live Shortcut window with its binding row and the
    //    Press/Hold mode selector rendered. (Plugin commands supply no while-held tick, so the selector
    //    is present but disabled — mode is keyboard/native-hold only this phase.)
    ImGuiTest *t2 = IM_REGISTER_TEST(e, "plugin_ui", "shortcut_window_lists_plugin_command_with_mode");
    t2->GuiFunc = pluginGateGuiFunc;
    t2->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto &binding = *getTestState().bindingSystem;

        // Reset to the default layout so prior suites' docking doesn't leave the window hidden/tabbed.
        ctx->MenuClick("//##MainMenuBar/###menu_layout/###layout_default");
        ctx->Yield(3);

        // Seed a binding for the plugin command so its row (with a Mode selector) renders.
        binding.addBinding({.trigger = KeyChord{.key = SDLK_J, .modifiers = SDL_KMOD_CTRL}, .commandId = kCmdNudge});
        binding.saveBindings();

        // Open the Shortcut window and filter to just the plugin command so its row is the only one.
        ctx->MenuClick("//##MainMenuBar/###menu_view/###menu_shortcuts");
        ctx->Yield(2);
        ctx->ItemClick("Shortcut Bindings###shortcut_bindings/###scfilter");
        ctx->KeyCharsReplace("Nudge");
        ctx->Yield(2);

        // The command surfaced in the window: its row rendered (the per-command add-binding "+" button, with
        // the stable "###bindadd" id, is present on every row). Probe by id, not by the glyph label. The Mode
        // selector is rendered too but disabled for this non-holdable plugin command, so it isn't the probe
        // target (the wildcard search needs an enabled item).
        IM_CHECK(ctx->ItemExists("Shortcut Bindings###shortcut_bindings/**/###bindadd"));
        IM_CHECK(binding.findHint(kCmdNudge) != nullptr);
    };

    // 3) Plugin-registered nodes reach the live Processing panel's add-node menu and instantiate.
    ImGuiTest *t3 = IM_REGISTER_TEST(e, "plugin_ui", "processing_panel_lists_plugin_nodes");
    t3->GuiFunc = pluginGateGuiFunc;
    t3->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto &eq = *getTestState().eventQueue;
        auto &effectReg = *getTestState().effectRegistry;

        // The plugin's nodes registered into the effect registry the panel's menu is built from.
        IM_CHECK(effectReg.pluginNodes.count(kNodeFgen) == 1);

        // Reset to the default layout so the Processing panel takes a known central dock node (prior
        // suites — e.g. layouts — can otherwise leave the center window relocated).
        ctx->MenuClick("//##MainMenuBar/###menu_layout/###layout_default");
        ctx->Yield(3);

        // Close any settings window a prior suite left open (e.g. shortcut_window_lists... opens the
        // Shortcut Bindings window). These windows center themselves, so an open one sits over the
        // Processing panel's center and would swallow the right-click meant for the imnodes canvas.
        // Must happen BEFORE the region is created: WindowClose left-clicks the title-bar close button,
        // and a left-click outside the panel pushes ClearRegionSelectionEvent (OfsApp::onImGuiRender),
        // which would reset procSelRegionId and leave the Video Player — not the panel — on the node.
        for (const char *w :
             {"//Shortcut Bindings###shortcut_bindings", "//Preferences###preferences", "//Project###project_config"}) {
            if (ctx->WindowInfo(w, ImGuiTestOpFlags_NoError).Window != nullptr)
                ctx->WindowClose(w);
        }
        ctx->Yield();

        // Select a region so the Processing panel takes the center dock window.
        eq.push(ofs::CreateRegionEvent{.axisRole = ofs::StandardAxis::L0, .startTime = 0.0, .endTime = 30.0});
        ctx->Yield(3);
        auto &proj = *getTestState().project;
        IM_CHECK(!proj.regions.empty());

        // Bring the panel to the front and focus it (prior suites may leave another window as the
        // active tab of the shared center dock node), then right-click the empty graph canvas — the
        // add-node popup only opens when the imnodes editor is the hovered, focused window.
        ImGuiWindow *panel = ImGui::FindWindowByName("Processing###video_player");
        IM_CHECK(panel != nullptr);
        ctx->WindowFocus("Processing###video_player");
        ctx->Yield();
        ctx->MouseMoveToPos(panel->Rect().GetCenter());
        ctx->Yield();
        ctx->MouseClick(ImGuiMouseButton_Right);
        ctx->Yield(2);
        IM_CHECK(anyPopupOpen());

        // Filter to the plugin node and click its menu item (label is icon + display name) — this
        // instantiates a PluginNode into the region graph, proving the menu lists and creates it.
        ctx->KeyCharsReplace("Test Functional");
        ctx->Yield(2);
        const std::string item = std::string("**/") + ICON_AUDIO_WAVEFORM + "  Test Functional Gen";
        ctx->ItemClick(item.c_str());
        ctx->Yield(2);

        bool added = false;
        for (const auto &n : proj.regions[0].nodeGraph.nodes)
            if (n.type == GraphNodeType::PluginNode && n.pluginNodeId == kNodeFgen)
                added = true;
        IM_CHECK(added);
    };

    // 4) Disabling the plugin at runtime removes its command and nodes from the live registries;
    //    re-enabling restores them.
    ImGuiTest *t4 = IM_REGISTER_TEST(e, "plugin_ui", "disable_plugin_removes_command_and_nodes");
    t4->GuiFunc = pluginGateGuiFunc;
    t4->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto &eq = *getTestState().eventQueue;
        auto &cmdReg = *getTestState().commandRegistry;
        auto &effectReg = *getTestState().effectRegistry;

        IM_CHECK(cmdReg.find(kCmdSeek) != nullptr);
        IM_CHECK(effectReg.pluginNodes.count(kNodeFgen) == 1);

        eq.push(ofs::SetPluginEnabledEvent{.name = kPluginName, .enabled = false});
        ctx->Yield(3);
        IM_CHECK(cmdReg.find(kCmdSeek) == nullptr);
        IM_CHECK(effectReg.pluginNodes.count(kNodeFgen) == 0);

        // Re-enable restores the command and nodes.
        eq.push(ofs::SetPluginEnabledEvent{.name = kPluginName, .enabled = true});
        ctx->Yield(3);
        IM_CHECK(cmdReg.find(kCmdSeek) != nullptr);
        IM_CHECK(effectReg.pluginNodes.count(kNodeFgen) == 1);
    };

    // 5) A key binding assigned to a plugin command survives a disable -> re-enable cycle: the host
    //    re-applies saved bindings (loadedOverrides_ shim) when the plugin re-registers its command.
    //    Uses kCmdSeek so it never aliases the nudge binding seeded by test 2.
    ImGuiTest *t5 = IM_REGISTER_TEST(e, "plugin_ui", "binding_survives_plugin_disable_enable");
    t5->GuiFunc = pluginGateGuiFunc;
    t5->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto &binding = *getTestState().bindingSystem;
        auto &cmdReg = *getTestState().commandRegistry;
        auto &eq = *getTestState().eventQueue;

        // Assign a Hold-mode binding to the plugin command and persist it to bindings.json.
        binding.addBinding({.trigger = KeyChord{.key = SDLK_K, .modifiers = SDL_KMOD_CTRL},
                            .commandId = kCmdSeek,
                            .mode = ofs::ActivationMode::Hold});
        binding.saveBindings();
        IM_CHECK(binding.findHint(kCmdSeek) != nullptr);

        // Disable → command and its binding are torn down.
        eq.push(ofs::SetPluginEnabledEvent{.name = kPluginName, .enabled = false});
        ctx->Yield(3);
        IM_CHECK(cmdReg.find(kCmdSeek) == nullptr);
        IM_CHECK(binding.findHint(kCmdSeek) == nullptr);

        // Re-enable → command re-registers and the saved binding is restored from disk, mode intact.
        eq.push(ofs::SetPluginEnabledEvent{.name = kPluginName, .enabled = true});
        ctx->Yield(3);
        IM_CHECK(cmdReg.find(kCmdSeek) != nullptr);
        const Trigger *hint = binding.findHint(kCmdSeek);
        IM_CHECK(hint != nullptr);
        const auto *kc = std::get_if<KeyChord>(hint);
        IM_CHECK(kc != nullptr);
        IM_CHECK(kc->key == SDLK_K);
        // The restored binding kept its Hold activation mode (carried through the loadedOverrides_ shim).
        ofs::ActivationMode restoredMode = ofs::ActivationMode::Press;
        for (const auto &b : binding.bindings())
            if (b.commandId == kCmdSeek)
                restoredMode = b.mode;
        IM_CHECK(restoredMode == ofs::ActivationMode::Hold);
    };

    // 6) The real C# plugin's OnRenderUi draws host widgets through the managed Ui.* bridge. Driving them
    //    in the live plugin window round-trips each interaction back through the host, observable as a seek
    //    to a distinct second. Covers two things the fake-native widget test in suite_plugins cannot: the
    //    managed Ui marshaling path end-to-end, and the interactive widgets beyond a button (checkbox +
    //    slider, including their ref-value write-back).
    ImGuiTest *t6 = IM_REGISTER_TEST(e, "plugin_ui", "plugin_widgets_roundtrip_through_managed_ui");
    t6->GuiFunc = pluginGateGuiFunc;
    t6->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        // Bring the plugin's own window to the front (prior suites may leave it tabbed or behind) and scope
        // item lookups to it.
        ctx->WindowFocus(kPluginWindow);
        ctx->SetRef(kPluginWindow);
        ctx->Yield(2);

        // Baseline cursor, distinct from every widget's seek target.
        eq.push(ofs::SeekEvent{1.0});
        ctx->Yield(2);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 1);

        // Ui.Button → host uiButton → plugin seeks to 3.
        ctx->ItemClick("**/Test UI Action");
        ctx->Yield(3);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 3);

        // Ui.Checkbox: a real toggle reports "changed" and writes the ref back → plugin seeks to 5.
        ctx->ItemClick("**/Test Flag");
        ctx->Yield(3);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 5);

        // Ui.Slider: setting the value reports "changed" and writes the ref back → plugin seeks to the
        // written-back value (8).
        ctx->ItemInputValue("**/Test Level", 8);
        ctx->Yield(3);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 8);
    };

    // 7) A key bound to a plugin command fires it through the real KeyDownEvent dispatch path
    //    (BindingSystem::onKeyDown → CommandRegistry::run → managed onCommand), not just via the palette
    //    as test 1 does. Observable through seekmark's side-effect (seek to second 7).
    ImGuiTest *t7 = IM_REGISTER_TEST(e, "plugin_ui", "bound_key_invokes_plugin_command");
    t7->GuiFunc = pluginGateGuiFunc;
    t7->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto &binding = *getTestState().bindingSystem;
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        // Bind Ctrl+L to the plugin's bindable seekmark command (distinct from the chords seeded by tests
        // 2 and 5 so it can't alias them).
        binding.addBinding({.trigger = KeyChord{.key = SDLK_L, .modifiers = SDL_KMOD_CTRL}, .commandId = kCmdSeek});

        // Seek away from the target so the command's effect is unambiguous.
        eq.push(ofs::SeekEvent{2.0});
        ctx->Yield(2);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 2);

        // Deliver the chord as a real KeyDownEvent — the boundary the SDL input layer pushes — so the whole
        // binding-dispatch path runs end to end and resolves to the plugin's managed onCommand.
        eq.push(ofs::KeyDownEvent{.key = SDLK_L, .modifiers = SDL_KMOD_CTRL, .repeat = false});
        ctx->Yield(3);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), kSeekSecond);
    };

    // 8) Two host-widget paths the earlier widget test doesn't reach: a combo (selection write-back) and a
    //    button nested two section levels deep. Both round-trip through the managed Ui.* bridge, observable
    //    as a seek; reaching the nested button proves uiPushSection/uiPopSection nest and unwind safely.
    ImGuiTest *t8 = IM_REGISTER_TEST(e, "plugin_ui", "plugin_combo_and_nested_sections");
    t8->GuiFunc = pluginGateGuiFunc;
    t8->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        ctx->WindowFocus(kPluginWindow);

        // The plugin window streams in over a few frames after focus, and its content lives in a child
        // window whose ref path is internally mangled — resolve it via WindowInfo (polling until it exists)
        // and set the ref to the child, per the imgui_test_engine guidance for child windows.
        const std::string childRef = std::string("//") + kPluginWindow + "/##plugin_content";
        ImGuiTestItemInfo content{};
        for (int i = 0; i < 120; ++i) {
            content = ctx->WindowInfo(childRef.c_str(), ImGuiTestOpFlags_NoError);
            if (content.Window != nullptr)
                break;
            ctx->Yield();
        }
        IM_CHECK(content.Window != nullptr);
        ctx->SetRef(content.Window);

        eq.push(ofs::SeekEvent{1.0});
        ctx->Yield(2);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 1);

        // Ui.Combo: ImGui's BeginCombo is one of the few widgets that never registers its label with the test
        // engine, so the "**/" label scan can't find it (unlike the button/checkbox/slider, which do). Address
        // it by its id path instead: the host wraps the 4th widget in PushID(3), and "$$3" is the engine's
        // encoding for PushID((int)3). Selecting the third item writes index 2 back through the managed bridge,
        // observable as a seek to 4 + 2.
        ctx->ItemClick("$$3/Test Mode");
        ctx->Yield();
        ImGuiWindow *popup = ctx->GetWindowByRef("//$FOCUSED");
        IM_CHECK(popup != nullptr);
        ctx->ItemClick((std::string("//") + popup->Name + "/**/Item C").c_str());
        ctx->Yield(3);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 6);

        // Ui.Button two sections deep → plugin seeks to 2. Buttons keep their visible label inside a form
        // section, so "**/" finds it; reaching it confirms the nested sections rendered and balanced.
        eq.push(ofs::SeekEvent{1.0});
        ctx->Yield(2);
        ctx->ItemClick("**/Nested Action");
        ctx->Yield(3);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 2);
    };

    // 8b) The input widgets the earlier tests don't reach — RadioButton, InputInt, InputFloat, DragInt,
    //     DragFloat, InputText — plus Ui.Row. Each round-trips through the managed Ui.* bridge (and its ref
    //     write-back), observable as a seek to a distinct second; reaching the button inside Test Row proves
    //     uiPushRow/uiPopRow balance. All are drawn top-level in the plugin, so "**/" finds them by label.
    ImGuiTest *t8b = IM_REGISTER_TEST(e, "plugin_ui", "plugin_input_widgets_and_row");
    t8b->GuiFunc = pluginGateGuiFunc;
    t8b->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        // The widgets seek as high as 80; give the dummy media a duration that won't clamp those seeks.
        eq.push(ofs::ChangeDummyDurationEvent{.durationSeconds = 100.0});
        ctx->WindowFocus(kPluginWindow);
        // Grow the window so the whole widget list is unclipped — "**/" only matches visible items.
        ctx->WindowResize((std::string("//") + kPluginWindow).c_str(), ImVec2(560.f, 1000.f));
        ctx->SetRef(kPluginWindow);
        ctx->Yield(2);

        // Ui.RadioButton: clicking the option writes its value back through the bridge → seek to 22.
        eq.push(ofs::SeekEvent{1.0});
        ctx->Yield(2);
        ctx->ItemClick("**/Radio To 22");
        ctx->Yield(3);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 22);

        // Ui.InputInt: typing 5 reports "changed" and writes the ref back → plugin seeks to 30 + 5.
        ctx->ItemInputValue("**/Test Int", 5);
        ctx->Yield(3);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 35);

        // Ui.InputFloat → seek to 40.
        eq.push(ofs::SeekEvent{1.0});
        ctx->Yield(2);
        ctx->ItemInputValue("**/Test Float", 2.5f);
        ctx->Yield(3);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 40);

        // Ui.DragInt: type a value (double-click to edit) → seek to 50.
        eq.push(ofs::SeekEvent{1.0});
        ctx->Yield(2);
        ctx->ItemInputValue("**/Test Drag Int", 7);
        ctx->Yield(3);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 50);

        // Ui.DragFloat → seek to 60.
        eq.push(ofs::SeekEvent{1.0});
        ctx->Yield(2);
        ctx->ItemInputValue("**/Test Drag Float", 3.5f);
        ctx->Yield(3);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 60);

        // Ui.InputText: editing the text reports "changed" → seek to 70.
        eq.push(ofs::SeekEvent{1.0});
        ctx->Yield(2);
        ctx->ItemInputValue("**/Test Text", "hello");
        ctx->Yield(3);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 70);

        // Ui.Row: the button inside the row routes its click through the host → seek to 80.
        eq.push(ofs::SeekEvent{1.0});
        ctx->Yield(2);
        ctx->ItemClick("**/Row Action");
        ctx->Yield(3);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 80);
    };

    // 10) inPalette:false hides a plugin command from the command palette, yet a bound key still invokes
    //     it. The palette-hidden "Hidden Xyzzy" (seek to 6) must NOT fire when its unique token is searched
    //     in the palette, but its bound chord must. Guards the OfsApp palette inPalette filter end-to-end.
    ImGuiTest *t10 = IM_REGISTER_TEST(e, "plugin_ui", "inpalette_false_hides_command_from_palette");
    t10->GuiFunc = pluginGateGuiFunc;
    t10->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto &binding = *getTestState().bindingSystem;
        auto &cmdReg = *getTestState().commandRegistry;
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        // The command registered (it is bindable) even though it is hidden from the palette.
        IM_CHECK(cmdReg.find(kCmdHidden) != nullptr);

        // Baseline cursor, distinct from the command's seek target (6).
        eq.push(ofs::SeekEvent{2.0});
        ctx->Yield(2);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 2);

        // Search the palette for the command's unique token. Because it is inPalette:false nothing matches,
        // so Enter invokes nothing and the cursor stays at 2 — it must never jump to the command's 6.
        eq.push(ofs::OpenCommandPaletteEvent{});
        ctx->Yield(3);
        IM_CHECK(anyPopupOpen());
        ctx->KeyCharsReplace("xyzzy");
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(3);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 2); // not invoked via the palette
        ctx->KeyPress(ImGuiKey_Escape);                            // close the palette if Enter left it open
        ctx->Yield(2);

        // The same command IS invocable by a bound key — proving it is registered and functional, just
        // absent from the palette. (The pushed KeyDownEvent isn't keyboard-captured, so it dispatches.)
        binding.addBinding({.trigger = KeyChord{.key = SDLK_M, .modifiers = SDL_KMOD_CTRL}, .commandId = kCmdHidden});
        eq.push(ofs::KeyDownEvent{.key = SDLK_M, .modifiers = SDL_KMOD_CTRL, .repeat = false});
        ctx->Yield(3);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), kHiddenSecond);
    };

    // 11) The widget ADDITIONS round-trip through the managed Ui.* bridge. InputTextMultiline writes its
    //     ref back (→ seek 91); ColorEdit/ProgressBar are exercised every frame just by the window
    //     rendering (their host calls run) — reaching the buttons drawn AFTER them proves they neither threw
    //     nor unbalanced the form. All are drawn top-level, so "**/" finds them by label.
    ImGuiTest *t11 = IM_REGISTER_TEST(e, "plugin_ui", "plugin_widget_additions_roundtrip");
    t11->GuiFunc = pluginGateGuiFunc;
    t11->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        eq.push(ofs::ChangeDummyDurationEvent{.durationSeconds = 100.0});
        ctx->WindowFocus(kPluginWindow);
        ctx->WindowResize((std::string("//") + kPluginWindow).c_str(), ImVec2(560.f, 1000.f));
        ctx->SetRef(kPluginWindow);
        ctx->Yield(2);

        // Ui.InputTextMultiline: editing it reports "changed" and writes the ref back → seek to 91.
        eq.push(ofs::SeekEvent{1.0});
        ctx->Yield(2);
        ctx->ItemInputValue("**/Test Multiline", "edited");
        ctx->Yield(3);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 91);

        // ColorEdit (before the multiline edit) and ProgressBar both ran this frame; the buttons
        // drawn AFTER them are still reachable, proving none threw or left the form table unbalanced.
        IM_CHECK(ctx->ItemExists("**/Test Open Dialog"));
        IM_CHECK(ctx->ItemExists("**/Test Confirm Dialog"));
    };

    // 12) The non-blocking host dialogs resolve back through the managed Task bridge. OpenFile is driven via
    //     the native-dialog test seam (a canned path), so the awaited Task completes a few frames later and the
    //     plugin seeks 92. Confirm raises the host modal; clicking "Yes" resolves the Task true → seek 93.
    ImGuiTest *t12 = IM_REGISTER_TEST(e, "plugin_ui", "plugin_dialogs_resolve_through_managed_tasks");
    t12->GuiFunc = pluginGateGuiFunc;
    t12->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        eq.push(ofs::ChangeDummyDurationEvent{.durationSeconds = 100.0});
        ctx->WindowFocus(kPluginWindow);
        ctx->WindowResize((std::string("//") + kPluginWindow).c_str(), ImVec2(560.f, 1000.f));
        ctx->SetRef(kPluginWindow);
        ctx->Yield(2);

        // OpenFile → the seam returns a canned path → the Task resolves on a later frame → plugin seeks 92.
        const auto picked = std::filesystem::temp_directory_path() / "ofs_plugin_dlg" / "x.funscript";
        std::filesystem::create_directories(picked.parent_path());
        ofs::setNativeDialogOverrideForTesting(
            [picked](const ofs::FileDialogSpec &, const std::string &) { return ofs::util::toUtf8(picked); });

        eq.push(ofs::SeekEvent{1.0});
        ctx->Yield(2);
        ctx->ItemClick("**/Test Open Dialog");
        ctx->Yield(15); // queue dialog → worker returns canned path → pump resumes → Task continuation seeks 92
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 92);
        ofs::setNativeDialogOverrideForTesting(nullptr);

        // OpenFiles → the multi seam returns two canned paths → the Task<string[]> resolves → plugin seeks
        // 80 + count (82), proving the whole multi-select path (host openFilesDialog → FilesResultTrampoline).
        const auto p1 = std::filesystem::temp_directory_path() / "ofs_plugin_dlg" / "a.funscript";
        const auto p2 = std::filesystem::temp_directory_path() / "ofs_plugin_dlg" / "b.funscript";
        ofs::setNativeMultiDialogOverrideForTesting([p1, p2](const ofs::FileDialogSpec &, const std::string &) {
            return std::vector<std::string>{ofs::util::toUtf8(p1), ofs::util::toUtf8(p2)};
        });
        eq.push(ofs::SeekEvent{1.0});
        ctx->Yield(2);
        ctx->ItemClick("**/Test Open Files Dialog");
        ctx->Yield(15);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 82);
        ofs::setNativeMultiDialogOverrideForTesting(nullptr);

        // Confirm → the host modal opens; "Yes" is the affirmative button (###modalbtn0) → Task true → seek 93.
        eq.push(ofs::SeekEvent{1.0});
        ctx->Yield(2);
        ctx->ItemClick("**/Test Confirm Dialog");
        ctx->Yield(3);
        IM_CHECK(anyPopupOpen());
        ctx->SetRef("###ofsmodal");
        ctx->ItemClick("**/###modalbtn0");
        ctx->Yield(3);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 93);
    };

    // 13) Ui freshness guard: a Ui builder stashed during OnRenderUi and called AFTER its pass throws
    //     (instead of pushing/popping into whatever ImGui is drawing then and corrupting the host's own UI).
    //     The plugin captures the builder each render; the bound "uioffpass" command fires on the main thread
    //     during event drain (outside any render pass) and calls it — the guard's freshness branch must throw
    //     with a "render pass" message, so the plugin seeks 94. A wrongly-allowed call would seek 96 instead.
    ImGuiTest *t13 = IM_REGISTER_TEST(e, "plugin_ui", "ui_builder_rejected_outside_render_pass");
    t13->GuiFunc = pluginGateGuiFunc;
    t13->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        eq.push(ofs::ChangeDummyDurationEvent{.durationSeconds = 100.0});
        // Render the plugin window so OnRenderUi runs and stashes the live Ui builder.
        ctx->WindowFocus(kPluginWindow);
        ctx->SetRef(kPluginWindow);
        ctx->Yield(3);

        // Baseline cursor, distinct from the probe's reject/allow markers (94/96).
        eq.push(ofs::SeekEvent{1.0});
        ctx->Yield(2);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 1);

        invokeCore(ctx, kCmdUiOffPass, SDLK_U);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 94); // rejected with the "render pass" message
    };

    // 14) Ui thread guard: the same stashed builder called from a worker thread throws BEFORE touching native
    //     ImGui (single-threaded), turning a would-be data race into a clean managed exception. The
    //     "uioffthread" command calls it inside a Task.Run; the guard's thread branch must throw with a "main
    //     thread" message, so the plugin seeks 95 (97 would mean the call wrongly reached native off-thread).
    ImGuiTest *t14 = IM_REGISTER_TEST(e, "plugin_ui", "ui_builder_rejected_off_main_thread");
    t14->GuiFunc = pluginGateGuiFunc;
    t14->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        eq.push(ofs::ChangeDummyDurationEvent{.durationSeconds = 100.0});
        ctx->WindowFocus(kPluginWindow);
        ctx->SetRef(kPluginWindow);
        ctx->Yield(3);

        eq.push(ofs::SeekEvent{1.0});
        ctx->Yield(2);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 1);

        invokeCore(ctx, kCmdUiOffThread, SDLK_Y);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 95); // rejected with the "main thread" message
    };

    // 15) Dialog cancellation: an awaited dialog whose token is cancelled completes as cancelled rather than
    //     hanging forever (the footgun where an open dialog's strong GCHandle pins the plugin's ALC). The
    //     plugin awaits OpenFile with a pre-cancelled token and seeks 98 from its OperationCanceledException
    //     handler — the same DialogBridge path UnloadToken drives on unload. The native-dialog seam is set so
    //     no real OS picker opens for the queued (and immediately-cancelled) request.
    ImGuiTest *t15 = IM_REGISTER_TEST(e, "plugin_ui", "dialog_task_cancels_on_token");
    t15->GuiFunc = pluginGateGuiFunc;
    t15->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        eq.push(ofs::ChangeDummyDurationEvent{.durationSeconds = 100.0});
        ctx->WindowFocus(kPluginWindow);
        ctx->SetRef(kPluginWindow);
        ctx->Yield(2);

        // Intercept the native dialog so the queued (already-cancelled) request never surfaces a real picker.
        const auto picked = std::filesystem::temp_directory_path() / "ofs_plugin_dlg" / "cancel.funscript";
        std::filesystem::create_directories(picked.parent_path());
        ofs::setNativeDialogOverrideForTesting(
            [picked](const ofs::FileDialogSpec &, const std::string &) { return ofs::util::toUtf8(picked); });

        eq.push(ofs::SeekEvent{1.0});
        ctx->Yield(2);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 1);

        invokeCore(ctx, kCmdUiDialogCancel, SDLK_N);
        ctx->Yield(5); // let the cancelled continuation's Seek(98) push and drain
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 98);

        ctx->Yield(15); // drain the queued dialog (the trampoline no-ops on the already-cancelled task)
        ofs::setNativeDialogOverrideForTesting(nullptr);
    };

    // ── Ofs.Core editing/selection commands ─────────────────────────────────────────────────────────
    // These drive the REAL shipped Ofs.Core plugin (loaded alongside the test plugin by the shared
    // loadPlugins() above) through the live key-dispatch path, then assert the resulting axis state.
    // Each seeds L0 with a known action set and fires the command via a private chord.

    // Equalize: the interior selected point is respaced to the midpoint in time; positions are kept.
    ImGuiTest *c1 = IM_REGISTER_TEST(e, "core_plugin", "equalize_respaces_selection");
    c1->GuiFunc = pluginGateGuiFunc;
    c1->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensureCoreLoaded(ctx));
        auto &eq = *getTestState().eventQueue;
        eq.push(ofs::AxisSelectedEvent{ofs::StandardAxis::L0});
        ctx->Yield();
        seedL0(ctx, {{0.0, 0}, {1.0, 100}, {5.0, 0}});
        selectL0Range(ctx, 0.0, 5.0);

        invokeCore(ctx, kCmdEqualize, SDLK_Q);

        auto &l0 = l0Axis();
        IM_CHECK_EQ(l0.actions.size(), static_cast<size_t>(3));
        IM_CHECK(hasAction(l0, 0.0)); // endpoints unmoved
        IM_CHECK(hasAction(l0, 5.0));
        IM_CHECK(!hasAction(l0, 1.0));    // interior point moved...
        IM_CHECK(hasAction(l0, 2.5));     // ...to the midpoint between the endpoints
        IM_CHECK_EQ(posAt(l0, 2.5), 100); // and kept its position
    };

    // Invert: every selected point's position flips to 100 - pos; the selection is preserved by time.
    ImGuiTest *c2 = IM_REGISTER_TEST(e, "core_plugin", "invert_flips_selected_positions");
    c2->GuiFunc = pluginGateGuiFunc;
    c2->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensureCoreLoaded(ctx));
        auto &eq = *getTestState().eventQueue;
        eq.push(ofs::AxisSelectedEvent{ofs::StandardAxis::L0});
        ctx->Yield();
        seedL0(ctx, {{0.0, 0}, {1.0, 30}, {2.0, 100}});
        selectL0Range(ctx, 0.0, 2.0);

        invokeCore(ctx, kCmdInvert, SDLK_W);

        auto &l0 = l0Axis();
        IM_CHECK_EQ(l0.actions.size(), static_cast<size_t>(3));
        IM_CHECK_EQ(posAt(l0, 0.0), 100);
        IM_CHECK_EQ(posAt(l0, 1.0), 70);
        IM_CHECK_EQ(posAt(l0, 2.0), 0);
        IM_CHECK_EQ(l0.selection.size(), static_cast<size_t>(3)); // selection survives the flip
        IM_CHECK(isSelected(l0, 1.0));
    };

    // Isolate: the points immediately before and after the one nearest the playhead are removed.
    ImGuiTest *c3 = IM_REGISTER_TEST(e, "core_plugin", "isolate_removes_neighbours_of_playhead");
    c3->GuiFunc = pluginGateGuiFunc;
    c3->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensureCoreLoaded(ctx));
        auto &eq = *getTestState().eventQueue;
        eq.push(ofs::AxisSelectedEvent{ofs::StandardAxis::L0});
        ctx->Yield();
        seedL0(ctx, {{0.0, 0}, {1.0, 100}, {2.0, 0}, {3.0, 100}, {4.0, 0}});
        eq.push(ofs::SeekEvent{2.0}); // nearest action is the one at 2.0
        ctx->Yield(2);

        invokeCore(ctx, kCmdIsolate, SDLK_E);

        auto &l0 = l0Axis();
        IM_CHECK_EQ(l0.actions.size(), static_cast<size_t>(3));
        IM_CHECK(hasAction(l0, 0.0));
        IM_CHECK(hasAction(l0, 2.0)); // the isolated point stays
        IM_CHECK(hasAction(l0, 4.0));
        IM_CHECK(!hasAction(l0, 1.0)); // immediate neighbours gone
        IM_CHECK(!hasAction(l0, 3.0));
    };

    // Select All Left: selects every action at or before the playhead.
    ImGuiTest *c4 = IM_REGISTER_TEST(e, "core_plugin", "select_all_left");
    c4->GuiFunc = pluginGateGuiFunc;
    c4->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensureCoreLoaded(ctx));
        auto &eq = *getTestState().eventQueue;
        eq.push(ofs::AxisSelectedEvent{ofs::StandardAxis::L0});
        ctx->Yield();
        seedL0(ctx, {{1.0, 0}, {2.0, 0}, {3.0, 0}, {4.0, 0}, {5.0, 0}});
        eq.push(ofs::SeekEvent{3.0});
        ctx->Yield(2);

        invokeCore(ctx, kCmdSelectAllLeft, SDLK_R);

        auto &l0 = l0Axis();
        IM_CHECK_EQ(l0.selection.size(), static_cast<size_t>(3));
        IM_CHECK(isSelected(l0, 1.0));
        IM_CHECK(isSelected(l0, 3.0)); // inclusive of the playhead
        IM_CHECK(!isSelected(l0, 4.0));
        IM_CHECK(!isSelected(l0, 5.0));
    };

    // Select All Right: selects every action at or after the playhead (up to the media duration).
    ImGuiTest *c5 = IM_REGISTER_TEST(e, "core_plugin", "select_all_right");
    c5->GuiFunc = pluginGateGuiFunc;
    c5->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensureCoreLoaded(ctx));
        auto &eq = *getTestState().eventQueue;
        eq.push(ofs::AxisSelectedEvent{ofs::StandardAxis::L0});
        eq.push(ofs::ChangeDummyDurationEvent{.durationSeconds = 10.0}); // a known upper bound for the command
        ctx->Yield(2);
        seedL0(ctx, {{1.0, 0}, {2.0, 0}, {3.0, 0}, {4.0, 0}, {5.0, 0}});
        eq.push(ofs::SeekEvent{3.0});
        ctx->Yield(2);

        invokeCore(ctx, kCmdSelectAllRight, SDLK_T);

        auto &l0 = l0Axis();
        IM_CHECK_EQ(l0.selection.size(), static_cast<size_t>(3));
        IM_CHECK(isSelected(l0, 3.0)); // inclusive of the playhead
        IM_CHECK(isSelected(l0, 5.0));
        IM_CHECK(!isSelected(l0, 1.0));
        IM_CHECK(!isSelected(l0, 2.0));
    };

    // Repeat Stroke command: continues the motion past the playhead by applying the replayed stroke
    // directly (one undo step) and selecting the inserted points — classic OFS behaviour, now a plain
    // bindable Ofs.Core command (no suggestion/ghost layer).
    ImGuiTest *c8 = IM_REGISTER_TEST(e, "core_plugin", "repeat_stroke_applies_and_selects");
    c8->GuiFunc = pluginGateGuiFunc;
    c8->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensureCoreLoaded(ctx));
        auto &eq = *getTestState().eventQueue;
        eq.push(ofs::AxisSelectedEvent{ofs::StandardAxis::L0});
        ctx->Yield();
        // A down/up/down pattern ending at the top {3.0,100}. Repeat continues the motion, replaying the
        // down stroke {1.0,100}→{2.0,0} shifted to connect to the anchor → {3.0,100},{4.0,0}.
        seedL0(ctx, {{0.0, 0}, {1.0, 100}, {2.0, 0}, {3.0, 100}});
        eq.push(ofs::SeekEvent{3.5});
        ctx->Yield(2);

        invokeCore(ctx, kCmdRepeat, SDLK_I);

        auto &l0 = l0Axis();
        IM_CHECK(hasAction(l0, 3.0));
        IM_CHECK(hasAction(l0, 4.0));
        IM_CHECK_EQ(posAt(l0, 3.0), 100);
        IM_CHECK_EQ(posAt(l0, 4.0), 0);
        IM_CHECK_EQ(l0.selection.size(), static_cast<size_t>(2)); // inserted points selected
    };

    // Motif path: a repeating multi-point rhythm is replayed WHOLE, not collapsed to a single
    // half-stroke. Seed a "double-tap" (0,100,60,100) repeated twice; the smart path detects period 4
    // and appends one more period after the anchor.
    ImGuiTest *c9 = IM_REGISTER_TEST(e, "core_plugin", "repeat_stroke_repeats_a_motif");
    c9->GuiFunc = pluginGateGuiFunc;
    c9->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensureCoreLoaded(ctx));
        auto &eq = *getTestState().eventQueue;
        eq.push(ofs::AxisSelectedEvent{ofs::StandardAxis::L0});
        ctx->Yield();
        // Two repetitions of the 4-point motif 0→100→60→100, anchored at the trailing {8.0,0}.
        seedL0(ctx,
               {{0.0, 0}, {1.0, 100}, {2.0, 60}, {3.0, 100}, {4.0, 0}, {5.0, 100}, {6.0, 60}, {7.0, 100}, {8.0, 0}});
        eq.push(ofs::SeekEvent{8.5});
        ctx->Yield(2);

        invokeCore(ctx, kCmdRepeat, SDLK_O);

        auto &l0 = l0Axis();
        // The motif (5..8) shifted forward by its 4.0s span continues the double-tap: 9,10,11,12.
        IM_CHECK_EQ(posAt(l0, 9.0), 100);
        IM_CHECK_EQ(posAt(l0, 10.0), 60);
        IM_CHECK_EQ(posAt(l0, 11.0), 100);
        IM_CHECK_EQ(posAt(l0, 12.0), 0);
        IM_CHECK(hasAction(l0, 8.0)); // the anchor is untouched (motif lands strictly after it)
        IM_CHECK_EQ(l0.selection.size(), static_cast<size_t>(4));
    };

    // Plateau path: a hold inside the previous stroke is not mistaken for a direction change, so the
    // replayed stroke includes the hold instead of starting mid-plateau.
    ImGuiTest *c10 = IM_REGISTER_TEST(e, "core_plugin", "repeat_stroke_sees_through_a_hold");
    c10->GuiFunc = pluginGateGuiFunc;
    c10->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensureCoreLoaded(ctx));
        auto &eq = *getTestState().eventQueue;
        eq.push(ofs::AxisSelectedEvent{ofs::StandardAxis::L0});
        ctx->Yield();
        // Up to 100, HOLD at 100, down to 0, up to 100. Anchor {4.0,100}; the previous (down) stroke
        // {1,100}→{2,100}→{3,0} — its leading hold must survive into the replay.
        seedL0(ctx, {{0.0, 0}, {1.0, 100}, {2.0, 100}, {3.0, 0}, {4.0, 100}});
        eq.push(ofs::SeekEvent{4.5});
        ctx->Yield(2);

        invokeCore(ctx, kCmdRepeat, SDLK_P);

        auto &l0 = l0Axis();
        // The full down stroke replays shifted by 3.0: {4,100},{5,100},{6,0}. The held {5.0,100} is the
        // discriminator — plateau-as-reversal would instead give {4,100},{5,0}.
        IM_CHECK_EQ(posAt(l0, 4.0), 100);
        IM_CHECK_EQ(posAt(l0, 5.0), 100);
        IM_CHECK_EQ(posAt(l0, 6.0), 0);
    };

    // Unload safety: the repeat-stroke command (like every plugin command) must disappear when Ofs.Core is
    // disabled and return on reload. Exercises the real CoreCLR adapter, not a native fake.
    ImGuiTest *c11 = IM_REGISTER_TEST(e, "core_plugin", "repeat_stroke_command_dropped_on_plugin_unload");
    c11->GuiFunc = pluginGateGuiFunc;
    c11->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensureCoreLoaded(ctx));
        auto &eq = *getTestState().eventQueue;
        auto &cmdReg = *getTestState().commandRegistry;
        IM_CHECK(cmdReg.find(kCmdRepeat) != nullptr);

        eq.push(ofs::SetPluginEnabledEvent{.name = kCorePluginName, .enabled = false});
        ctx->Yield(3);
        IM_CHECK(cmdReg.find(kCmdRepeat) == nullptr); // plugin really unloaded

        eq.push(ofs::SetPluginEnabledEvent{.name = kCorePluginName, .enabled = true});
        ctx->Yield(3);
        IM_CHECK(cmdReg.find(kCmdRepeat) != nullptr); // command re-registered on reload
    };

    // Localization: Ofs.Core follows ofs-ng's active UI language through the host language signal. Its
    // command titles are registered in OnLoad via Loc.Tr(...) — Loc.Culture is seeded from Host.Culture,
    // which the host derives from the active translation file's [_meta] iso639 code. So the live title in
    // the registry is the TRANSLATED string for whatever language the run uses: English under ui-smoke,
    // Japanese under ui-smoke-loc (--language=ja_[AI], iso639 "ja"). This is the end-to-end proof that the
    // host's ISO 639 code reaches a real plugin's .NET ResourceManager AND that its ja satellite assembly
    // (ja/Ofs.Core.resources.dll) resolves under the plugin's collectible, load-from-bytes ALC.
    ImGuiTest *c12 = IM_REGISTER_TEST(e, "core_plugin", "command_titles_follow_host_language");
    c12->GuiFunc = pluginGateGuiFunc;
    c12->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensureCoreLoaded(ctx));
        auto &cmdReg = *getTestState().commandRegistry;
        const ofs::Command *equalize = cmdReg.find(kCmdEqualize);
        const ofs::Command *invert = cmdReg.find(kCmdInvert);
        IM_CHECK(equalize != nullptr);
        IM_CHECK(invert != nullptr);

        // The exact ISO 639 code the host hands the plugin (getActiveLanguage → Host.Culture).
        const bool ja = ofs::loc::Translator::instance().activeLanguageCode() == "ja";
        IM_CHECK_STR_EQ(equalize->title.c_str(), ja ? "均等化" : "Equalize");
        IM_CHECK_STR_EQ(invert->title.c_str(), ja ? "反転" : "Invert");
    };

    // Interaction extension points (Phase 6): the real Ofs.Core plugin registers an edit mode and a
    // navigator through the new surface. These activate each via the footer-selector event path (the only
    // writer of the project fields) and drive the request events a real gesture/key would, proving the
    // shipped plugin's overrides reach the routers end-to-end.

    // c13) Activating Ofs.Core's edit mode reshapes an AddPoint per its Alternating behavior: with the
    //      panel Mode set to Alternating and the previous point in the top half, an add resolves to the
    //      opposite extreme instead of the requested position.
    ImGuiTest *c13 = IM_REGISTER_TEST(e, "core_plugin", "edit_mode_alternating_reshapes_add");
    c13->GuiFunc = pluginGateGuiFunc;
    c13->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensureCoreLoaded(ctx));
        auto &eq = *getTestState().eventQueue;
        auto &proj = *getTestState().project;

        // Seed the panel Mode = Alternating (enum 1) into the plugin's project-scoped store, then a project
        // load makes the plugin reload its scoped settings. Other Settings fields keep their defaults.
        proj.pluginData["Ofs.Core"]["settings"] = nlohmann::json{{"Mode", 1}};
        eq.push(ofs::LoadProjectEvent{});
        ctx->Yield(2);

        eq.push(ofs::AxisSelectedEvent{ofs::StandardAxis::L0});
        ctx->Yield();
        seedL0(ctx, {{1.0, 80}}); // a prior point in the top half → the next alternation goes to the bottom

        // Activate the plugin edit mode (footer-selector path; only it writes activeEditMode).
        eq.push(ofs::SetActiveEditModeEvent{.id = "Ofs.Core.shape"});
        ctx->Yield(2);
        IM_CHECK_STR_EQ(proj.activeEditMode.c_str(), "Ofs.Core.shape"); // accepted ⇒ the mode is registered

        // An add requesting pos 80: native would place 80, but Alternating mirrors around the previous
        // top-half point to the bottom extreme (100 - 80 = 20).
        eq.push(ofs::EditRequestEvent{
            .intent = {.kind = ofs::EditIntentKind::AddPoint, .axis = ofs::StandardAxis::L0, .time = 2.0, .pos = 80}});
        ctx->Yield(2);
        IM_CHECK_EQ(posAt(l0Axis(), 2.0), 20);

        eq.push(ofs::SetActiveEditModeEvent{.id = kNativeEditModeId}); // restore for later tests
        ctx->Yield();
    };

    // ── Crash-safety ────────────────────────────────────────────────────────────────────────────────
    // A plugin (or script) exception must never reach native — that is an instant process crash. These
    // tests fire the deliberately-faulty entry points added to Ofs.TestPlugin (and a throwing Roslyn
    // script) through the live app and assert the host catches the fault and keeps running. Chords use
    // Ctrl+Alt so they can't alias the bindings the tests above seed.
    constexpr SDL_Keymod kCrashMods = SDL_KMOD_CTRL | SDL_KMOD_ALT;

    // x1) A command whose managed handler throws is swallowed by PluginGuard("OnCommand"): the host stays
    //     responsive (a good plugin command still round-trips) and the fault surfaces as an error toast.
    ImGuiTest *x1 = IM_REGISTER_TEST(e, "plugin_crash", "throwing_command_does_not_crash_host");
    x1->GuiFunc = pluginGateGuiFunc;
    x1->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto &binding = *getTestState().bindingSystem;
        auto &cmdReg = *getTestState().commandRegistry;
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        IM_CHECK(cmdReg.find(kCmdBoom) != nullptr);

        // Fire the faulty command through the real key-dispatch path; its handler throws mid-dispatch.
        binding.addBinding({.trigger = KeyChord{.key = SDLK_B, .modifiers = kCrashMods}, .commandId = kCmdBoom});
        eq.push(ofs::SeekEvent{1.0});
        ctx->Yield(2);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 1);
        eq.push(ofs::KeyDownEvent{.key = SDLK_B, .modifiers = kCrashMods, .repeat = false});
        ctx->Yield(3);

        // Host survived and is still responsive: a known-good plugin command still runs end-to-end (→ 7).
        binding.addBinding({.trigger = KeyChord{.key = SDLK_N, .modifiers = kCrashMods}, .commandId = kCmdSeek});
        eq.push(ofs::KeyDownEvent{.key = SDLK_N, .modifiers = kCrashMods, .repeat = false});
        ctx->Yield(3);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), kSeekSecond);
        IM_CHECK(ImGui::GetCurrentContext() != nullptr);
    };

    // x2) A functional node whose eval throws: the ModTrampoline catch passes the input through unchanged,
    //     so the region resolves to its input and the host (and the worker pool) keep running.
    ImGuiTest *x2 = IM_REGISTER_TEST(e, "plugin_crash", "throwing_functional_node_does_not_crash_host");
    x2->GuiFunc = pluginGateGuiFunc;
    x2->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto &effectReg = *getTestState().effectRegistry;
        IM_CHECK(effectReg.pluginNodes.count(kNodeBoomMod) == 1);

        const int id = createL0Region(ctx);
        IM_CHECK(getTestState().project->findRegion(id) != nullptr);
        setRegionGraph(ctx, id, pluginModGraph(kNodeBoomMod));
        IM_CHECK(seedAndEvalL0(ctx));

        auto &l0 = l0Axis();
        IM_CHECK(l0.resolved.has_value());
        const auto &acts = l0.resolved->actions;
        IM_CHECK_EQ(acts.size(), static_cast<size_t>(1));
        if (acts.size() == 1) {
            // Throwing modifier → trampoline returned the input → action passed through unchanged.
            IM_CHECK_LT(std::abs(acts[0].at - 1.0), 1e-6);
            IM_CHECK_EQ(acts[0].pos, 25);
        }
        IM_CHECK(ImGui::GetCurrentContext() != nullptr);
    };

    // x3) A discrete node whose eval throws: the DiscreteGenTrampoline guard emits nothing rather than flush
    //     a half-filled writer, so the region resolves empty — covers the PluginGuard.Run (discrete) path.
    ImGuiTest *x3 = IM_REGISTER_TEST(e, "plugin_crash", "throwing_discrete_node_does_not_crash_host");
    x3->GuiFunc = pluginGateGuiFunc;
    x3->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto &effectReg = *getTestState().effectRegistry;
        IM_CHECK(effectReg.pluginNodes.count(kNodeBoomDGen) == 1);

        const int id = createL0Region(ctx);
        setRegionGraph(ctx, id, pluginGenGraph(kNodeBoomDGen));
        IM_CHECK(seedAndEvalL0(ctx));

        auto &l0 = l0Axis();
        IM_CHECK(l0.resolved.has_value());
        IM_CHECK(l0.resolved->actions.empty()); // throwing generator emitted nothing
        IM_CHECK(ImGui::GetCurrentContext() != nullptr);
    };

    // x5) A TState node with a `ui` callback renders its widget in the node body. Adding it through the
    //     Add-Node menu both proves the menu lists a TState node AND lands it in a focused, rendering
    //     panel — so its body (which calls entry.onNodeUi → NodeUiTrampoline → the `ui`-callback Offset
    //     slider) draws every frame. Yielding many frames exercises that path repeatedly; under
    //     IMGUI_DEBUG_PARANOID a mismatched id/table stack in the render path would assert, so surviving N
    //     frames with the context intact is the real coverage. Separately, the eval-side capture of the
    //     node's JSON state is covered by the .NET dispatch suite (test_plugin_dispatch.cpp).
    ImGuiTest *np = IM_REGISTER_TEST(e, "plugin_ui", "plugin_node_param_ui_renders_in_panel");
    np->GuiFunc = pluginGateGuiFunc;
    np->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto &eq = *getTestState().eventQueue;
        auto &effectReg = *getTestState().effectRegistry;
        IM_CHECK(effectReg.pluginNodes.count(kNodeStateOffset) == 1);
        IM_CHECK(effectReg.pluginNodes.at(kNodeStateOffset).onNodeUi != nullptr); // ui callback → render hook set
        IM_CHECK(static_cast<bool>(effectReg.renderNodeUi));                      // invoker wired

        // Default layout + close any centered settings window a prior suite left open (it would swallow the
        // right-click meant for the imnodes canvas), then select a region so the panel takes the center node.
        ctx->MenuClick("//##MainMenuBar/###menu_layout/###layout_default");
        ctx->Yield(3);
        for (const char *w :
             {"//Shortcut Bindings###shortcut_bindings", "//Preferences###preferences", "//Project###project_config"}) {
            if (ctx->WindowInfo(w, ImGuiTestOpFlags_NoError).Window != nullptr)
                ctx->WindowClose(w);
        }
        ctx->Yield();
        eq.push(ofs::CreateRegionEvent{.axisRole = ofs::StandardAxis::L0, .startTime = 0.0, .endTime = 30.0});
        ctx->Yield(3);
        auto &proj = *getTestState().project;
        IM_CHECK(!proj.regions.empty());

        // Focus the panel and right-click the empty canvas to open the Add-Node popup.
        ImGuiWindow *panel = ImGui::FindWindowByName("Processing###video_player");
        IM_CHECK(panel != nullptr);
        ctx->WindowFocus("Processing###video_player");
        ctx->Yield();
        ctx->MouseMoveToPos(panel->Rect().GetCenter());
        ctx->Yield();
        ctx->MouseClick(ImGuiMouseButton_Right);
        ctx->Yield(2);
        IM_CHECK(anyPopupOpen());

        // Filter to the TState node and click it — proves the menu lists it and instantiates a PluginNode.
        ctx->KeyCharsReplace("State Offset");
        ctx->Yield(2);
        const std::string item = std::string("**/") + ICON_SLIDERS_HORIZONTAL + "  State Offset";
        ctx->ItemClick(item.c_str());
        ctx->Yield(2);

        bool added = false;
        for (const auto &n : proj.regions[0].nodeGraph.nodes)
            if (n.type == GraphNodeType::PluginNode && n.pluginNodeId == kNodeStateOffset)
                added = true;
        IM_CHECK(added);

        // The node now sits on the focused canvas; render many frames so its body (→ onNodeUi → the
        // `ui`-callback Offset slider) draws repeatedly. Surviving with the context intact (and the
        // paranoid id/table-stack checks not asserting) is the proof the render path is sound.
        ctx->Yield(20);
        IM_CHECK(ImGui::GetCurrentContext() != nullptr);
    };

    // A node whose onNodeUi hook exists because of its custom `ui` callback, so adding it and rendering
    // its body exercises the UiCallback path of NodeUiTrampoline (label + buttons + the
    // Node<TState>.Update deferred-write closure). As with the slider test above, the node body's inner
    // widgets aren't ref-targetable through
    // imnodes, so surviving many render frames with the context intact (and the paranoid id/table-stack
    // checks not firing) is the coverage that the custom-ui render path is balanced and sound.
    ImGuiTest *nc = IM_REGISTER_TEST(e, "plugin_ui", "plugin_node_custom_ui_renders_in_panel");
    nc->GuiFunc = pluginGateGuiFunc;
    nc->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto &eq = *getTestState().eventQueue;
        auto &effectReg = *getTestState().effectRegistry;
        IM_CHECK(effectReg.pluginNodes.count(kNodeCustomBump) == 1);
        IM_CHECK(effectReg.pluginNodes.at(kNodeCustomBump).onNodeUi != nullptr); // custom ui → render hook set

        ctx->MenuClick("//##MainMenuBar/###menu_layout/###layout_default");
        ctx->Yield(3);
        for (const char *w :
             {"//Shortcut Bindings###shortcut_bindings", "//Preferences###preferences", "//Project###project_config"}) {
            if (ctx->WindowInfo(w, ImGuiTestOpFlags_NoError).Window != nullptr)
                ctx->WindowClose(w);
        }
        ctx->Yield();
        eq.push(ofs::CreateRegionEvent{.axisRole = ofs::StandardAxis::L0, .startTime = 0.0, .endTime = 30.0});
        ctx->Yield(3);
        auto &proj = *getTestState().project;
        IM_CHECK(!proj.regions.empty());

        ImGuiWindow *panel = ImGui::FindWindowByName("Processing###video_player");
        IM_CHECK(panel != nullptr);
        ctx->WindowFocus("Processing###video_player");
        ctx->Yield();
        ctx->MouseMoveToPos(panel->Rect().GetCenter());
        ctx->Yield();
        ctx->MouseClick(ImGuiMouseButton_Right);
        ctx->Yield(2);
        IM_CHECK(anyPopupOpen());

        ctx->KeyCharsReplace("Custom Bump");
        ctx->Yield(2);
        const std::string item = std::string("**/") + ICON_SLIDERS_HORIZONTAL + "  Custom Bump";
        ctx->ItemClick(item.c_str());
        ctx->Yield(2);

        bool added = false;
        for (const auto &n : proj.regions[0].nodeGraph.nodes)
            if (n.type == GraphNodeType::PluginNode && n.pluginNodeId == kNodeCustomBump)
                added = true;
        IM_CHECK(added);

        ctx->Yield(20);
        IM_CHECK(ImGui::GetCurrentContext() != nullptr);
    };

    // x4) A widget whose handler throws *while inside a section*: Ui.Section pops in a finally so the host's
    //     form table stays balanced as the exception unwinds, and PluginGuard("OnRenderUi") catches it. The
    //     plugin window keeps rendering afterward and a good widget still round-trips.
    ImGuiTest *x4 = IM_REGISTER_TEST(e, "plugin_crash", "throwing_ui_widget_does_not_crash_host");
    x4->GuiFunc = pluginGateGuiFunc;
    x4->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        ctx->WindowFocus(kPluginWindow);
        // Grow the window so both the throwing button and the good widget are unclipped — "**/" only
        // matches visible items, and the plugin window now lists enough widgets to overflow a short window.
        ctx->WindowResize((std::string("//") + kPluginWindow).c_str(), ImVec2(560.f, 1000.f));
        ctx->SetRef(kPluginWindow);
        ctx->Yield(2);

        eq.push(ofs::SeekEvent{1.0});
        ctx->Yield(2);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 1);

        // Click the throwing button — OnRenderUi throws on this frame; the host must survive it.
        ctx->ItemClick("**/Boom UI");
        ctx->Yield(5);
        IM_CHECK(ImGui::GetCurrentContext() != nullptr);

        // The window still renders and a good widget still round-trips through the managed Ui bridge (→ 3).
        ctx->ItemClick("**/Test UI Action");
        ctx->Yield(3);
        IM_CHECK_EQ(static_cast<int>(proj.playback.cursorPos), 3);
    };

    // x5) A Roslyn-compiled script node whose body throws at eval: it runs through the SAME trampolines as a
    //     plugin node, so the ModTrampoline catch passes the input through. Reuses ensurePluginLoaded purely
    //     as a ".NET available" gate (the script host boots independently of any plugin).
    ImGuiTest *x5 = IM_REGISTER_TEST(e, "plugin_crash", "throwing_script_node_does_not_crash_host");
    x5->GuiFunc = pluginGateGuiFunc;
    x5->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensurePluginLoaded(ctx));
        auto *scriptReg = getTestState().scriptRegistry;
        IM_CHECK(scriptReg != nullptr);

        // Write a functional-modifier script whose body throws at runtime (it still compiles cleanly).
        const auto scriptsDir = ofs::util::getPrefPath() / "scripts";
        std::error_code ec;
        std::filesystem::create_directories(scriptsDir, ec);
        const std::string fileName = "crash_throw_mod.cs";
        {
            std::ofstream f(scriptsDir / ofs::util::fromUtf8(fileName), std::ios::binary);
            f << "// !ofs:signal functional\n// !ofs:input a\n"
                 "throw new System.Exception(\"crash_throw_mod: intentional script fault\");\n";
        }

        getTestState().eventQueue->push(ofs::CompileScriptEvent{fileName});
        bool compiled = false;
        for (int i = 0; i < 6000 && !compiled; ++i) {
            const ofs::CompiledScript *cs = scriptReg->find(fileName);
            compiled = cs != nullptr && cs->ref.valid();
            if (!compiled)
                ctx->Yield();
        }
        IM_CHECK(compiled); // .NET/Roslyn is mandatory — a script that never compiles is a failure

        const int id = createL0Region(ctx);
        setRegionGraph(ctx, id, scriptModGraph(fileName));
        IM_CHECK(seedAndEvalL0(ctx));

        auto &l0 = l0Axis();
        IM_CHECK(l0.resolved.has_value());
        const auto &acts = l0.resolved->actions;
        IM_CHECK_EQ(acts.size(), static_cast<size_t>(1));
        if (acts.size() == 1)
            IM_CHECK_EQ(acts[0].pos, 25); // throwing script modifier → input passed through
        IM_CHECK(ImGui::GetCurrentContext() != nullptr);
    };

    // c-onui) The docked Tool Options panel renders a collapsing-header section for an active mode that
    //         supplied an OfsIntentUiFn, and nothing for option-less (native) modes. Ofs.Core's "edit" mode
    //         registers an onUi; the native mode has none. Proves the onUi seam end-to-end: registration →
    //         host panel gating on the active selection → the mode's options drawn (no IsActive hand-gating).
    //         The header's stable "###hdr" id is language-independent, so this holds under ui-smoke-loc.
    ImGuiTest *cu = IM_REGISTER_TEST(e, "core_plugin", "tool_options_panel_shows_active_mode");
    cu->GuiFunc = pluginGateGuiFunc;
    cu->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        IM_CHECK(ensureCoreLoaded(ctx));
        auto &eq = *getTestState().eventQueue;
        auto &proj = *getTestState().project;

        // Reset to the default layout so the Tool Options panel takes its own visible dock slot (prior
        // suites may have relocated or tabbed it).
        ctx->MenuClick("//##MainMenuBar/###menu_layout/###layout_default");
        ctx->Yield(3);

        // All three extension points native → no mode supplies onUi → the panel draws no section header.
        eq.push(ofs::SetActiveEditModeEvent{.id = kNativeEditModeId});
        eq.push(ofs::SetActiveNavigatorEvent{.id = kFollowOverlayNavigatorId});
        eq.push(ofs::SetActiveSelectionModeEvent{.id = kNativeSelectionModeId});
        ctx->Yield(2);
        IM_CHECK(!ctx->ItemExists("//Tool Options###tool_options/**/###hdr"));

        // Activate Ofs.Core's "shape" mode (it supplied an onUi) → its collapsing-header section appears.
        eq.push(ofs::SetActiveEditModeEvent{.id = "Ofs.Core.shape"});
        ctx->Yield(2);
        IM_CHECK_STR_EQ(proj.activeEditMode.c_str(), "Ofs.Core.shape");
        IM_CHECK(ctx->ItemExists("//Tool Options###tool_options/**/###hdr"));

        // Restore native so later tests start from a known selection.
        eq.push(ofs::SetActiveEditModeEvent{.id = kNativeEditModeId});
        ctx->Yield(2);
    };
}
