// Real-.NET plugin dispatch + HostApi tests.
//
// These boot the CoreCLR runtime and drive a genuine C# plugin (tests/plugins/Ofs.ProbePlugin)
// through the full native<->managed bridge — no fake PluginApi. The probe plugin makes every
// host->plugin callback observable by translating it into a Host.Player.Seek(value); the seek
// "value" is an encoded channel (thousands = which callback, remainder = its argument), so the
// test captures SeekEvent and decodes what fired and with what data. See ProbePlugin.cs.
//
// Requires the .NET runtime plus the staged managed/ assemblies and the built probe plugin next to
// the binary (see tests/CMakeLists.txt). .NET is mandatory: a test whose fixture can't come up fails
// (REQUIRE_DOTNET), it never skips. The pure-function ABI-version gate runs unconditionally.

#include <doctest/doctest.h>

#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/IntentEvents.h"
#include "Core/ScriptProject.h"
#include "Localization/Translator.h"
#include "Services/BindingSystem.h"
#include "Services/CommandRegistry.h"
#include "Services/EditIntentRouter.h"
#include "Services/EditModeRegistry.h"
#include "Services/EffectRegistry.h"
#include "Services/JobSystem.h"
#include "Services/NavigatorRegistry.h"
#include "Services/NavigatorRouter.h"
#include "Services/PluginApi.h"
#include "Services/PluginManager.h"
#include "Services/ProcessingSystem.h"
#include "Services/SelectIntentRouter.h"
#include "Services/SelectionModeRegistry.h"
#include "Util/PathUtil.h"
#include "helpers/EventCapture.h"
#include "helpers/FakeVideoPlayer.h"
#include "helpers/TestProject.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace ofs;
using ofs::test::EventCapture;
using ofs::test::FakeVideoPlayer;
using ofs::test::TestProject;

namespace {
namespace fs = std::filesystem;

// ── Seek-channel encoding, mirrored from ProbePlugin.cs ───────────────────────
// Each callback seeks (base + arg); channels are 1000 apart and args stay < 1000, so a captured
// seek decodes unambiguously to (channel, arg).
constexpr double kPlayBase = 1000;
constexpr double kSpeedBase = 2000;
constexpr double kMediaBase = 3000;
constexpr double kProjectMark = 4000;
constexpr double kAxisModBase = 5000;
constexpr double kActiveBase = 6000;
constexpr double kTimeBase = 7000;
constexpr double kUpdateMark = 8000;

constexpr double kStTime = 10000;
constexpr double kStPlaying = 11000;
constexpr double kStSpeed = 12000;
constexpr double kStExisting = 13000;
constexpr double kStActionCount = 14000;
constexpr double kStActive = 15000;
constexpr double kStFirstPos = 16000;
constexpr double kStSecondPos = 17000;

constexpr double kPingMark = 22000;
constexpr double kWorkerOk = 23000;
constexpr double kWorkerRejected = 23001;

// state2 channels (axis/project/player additions), mirrored from ProbePlugin.cs.
constexpr double kStVisible = 24000;
constexpr double kStLocked = 25000;
constexpr double kStName = 27000;
constexpr double kStVolume = 28000;
constexpr double kStFps = 29000;
constexpr double kStVideoDims = 30000;
constexpr double kStDirty = 31000;
constexpr double kStChapters = 32000;
constexpr double kStBookmarks = 33000;
constexpr double kStRegions = 34000;
constexpr double kStTitle = 35000;
constexpr double kStTagCount = 36000;
constexpr double kStPerformerCount = 37000;
constexpr double kStCustomCount = 38000;
constexpr double kCancelStartedBase = 39000;  // cancelprobe eval entered its loop (monotonic counter)
constexpr double kCancelObservedBase = 40000; // cancelprobe eval saw ctx.IsCancelled (monotonic counter)
constexpr double kStaleRejected = 41000;      // a cross-frame AxisEdit.Commit was rejected (expected)
constexpr double kStaleCommitted = 41001;     // a cross-frame AxisEdit.Commit unexpectedly succeeded (bug)
constexpr double kStScopedN = 45000;          // ProjectScoped<DataProbe>("scoped").Value.N
constexpr double kStAppScopedN = 68000;       // AppScoped<DataProbe>("appscoped").Value.N
constexpr double kModeEnterBase = 46000;      // probe edit mode onEnter count (intentlifecycle command)
constexpr double kModeExitBase = 47000;       // probe edit mode onExit count  (intentlifecycle command)
constexpr double kEditActiveBase = 48000;     // Editing.IsActive("intentmode")     (intentactive command)
constexpr double kNavActiveBase = 49000;      // Navigation.IsActive("fixedstep")   (intentactive command)
constexpr double kSelActiveBase = 50000;      // Selection.IsActive("probeselect")  (intentactive command)
constexpr double kNavEnterBase = 51000;       // probe navigator onEnter count       (intentlifecycle command)
constexpr double kNavExitBase = 52000;        // probe navigator onExit count        (intentlifecycle command)
constexpr double kSelEnterBase = 53000;       // probe selection mode onEnter count  (intentlifecycle command)
constexpr double kSelExitBase = 54000;        // probe selection mode onExit count   (intentlifecycle command)
constexpr double kNullRole = 99;              // ProbePlugin.NullRole — "no video" / "no axis" sentinel

// Host-surface probe channels (Host.cs), mirrored from ProbePlugin.cs.
constexpr double kIsMainMark = 54000;
constexpr double kUnloadLive = 55000;
constexpr double kRunFire = 56000;
constexpr double kRunAsyncAction = 57000;
constexpr double kRunAsyncFunc = 58000;
constexpr double kCultureInvariant = 59000;
constexpr double kStDuration2 = 60000; // Player additions (Player.cs)
constexpr double kStMediaLen = 61000;
constexpr double kStVideoH = 62000;
constexpr double kDialogs2Ran = 63000;    // Dialogs additions (Dialogs.cs)
constexpr double kAxisApiOk = 64000;      // axisapi passed-check count (Axes.cs AxisMath/AxisEdit)
constexpr int kAxisApiChecks = 11;        // ProbePlugin.AxisApiChecks
constexpr double kFreshRejected = 65000;  // a cross-frame Axis.Actions read was rejected (expected)
constexpr double kFreshLived = 65001;     // a cross-frame Axis read unexpectedly succeeded (bug)
constexpr double kScratchOk = 66000;      // a >4096-action axis read grew the scratch buffer and returned all rows
constexpr double kAbsentRejected = 67000; // + count of absent-axis sub-checks that threw (ProbePlugin.AbsentChecks)
constexpr int kAbsentChecks = 2;          // ProbePlugin.AbsentChecks (indexer + SetActive)
constexpr double kAxisApi2Ok = 69000;     // axisapi2 passed-check count (Axes.cs ScriptAction overloads)
constexpr int kAxisApi2Checks = 7;        // ProbePlugin.AxisApi2Checks
constexpr double kEditIntentsOk = 70000;  // editintents factory passed-check count (Editing.cs)
constexpr int kEditIntentsChecks = 14;    // ProbePlugin.EditIntentsChecks
constexpr double kStepTimeRan = 71000;    // editintents read Host.Editing.StepTime without throwing

// Number of captured seeks whose value lies in [base, base+1000).
int channelCount(const std::vector<SeekEvent> &v, double base) {
    return static_cast<int>(
        std::ranges::count_if(v, [&](const SeekEvent &e) { return e.time >= base && e.time < base + 1000; }));
}

// Argument (value - base) of the LAST seek in the channel, or nullopt if the channel never fired.
std::optional<double> channelArg(const std::vector<SeekEvent> &v, double base) {
    for (auto it = v.rbegin(); it != v.rend(); ++it)
        if (it->time >= base && it->time < base + 1000)
            return it->time - base;
    return std::nullopt;
}

// True if a seek with the exact encoded value (base + arg) was captured.
bool sawSeek(const std::vector<SeekEvent> &v, double base, double arg = 0.0) {
    return std::ranges::any_of(v, [&](const SeekEvent &e) { return std::abs(e.time - (base + arg)) < 1e-6; });
}

// ── Real-CoreCLR fixture: boots the runtime, stages the named plugins into the test pref dir, and
// loads them. `ready` is false when the .NET runtime / built plugins are unavailable — callers skip.
// On teardown it unloads every loaded plugin (releasing the DLL locks) and clears the staged dir so
// the next case — here or in test_plugin_load.cpp — starts from a clean plugins/ root.
struct DotNetFixture {
    TestProject tp;
    std::shared_ptr<FakeVideoPlayer> player = std::make_shared<FakeVideoPlayer>();
    std::shared_ptr<FakeVideoPlayer> dummy = std::make_shared<FakeVideoPlayer>();
    CommandRegistry cmdReg{tp.eq};
    RebindState rebind;
    BindingSystem binding{tp.eq, cmdReg, rebind};
    EffectRegistryState effectReg;
    PluginManager pm{tp.project, tp.eq, player, dummy, cmdReg, binding, effectReg};
    // The interaction-intent routers + their registries: the probe plugin registers an edit mode, a
    // navigator, and a selection mode at OnLoad (RegisterEditModeEvent / RegisterNavigatorEvent /
    // RegisterSelectionModeEvent, drained below); the routers are the sole subscribers, so they own the
    // registries and dispatch EditRequest / StepRequest / SelectRequest to the active plugin selection.
    // Constructed before freeze() so their on<> handlers register.
    EditModeRegistry editModeReg;
    NavigatorRegistry navReg;
    SelectionModeRegistry selReg;
    EditIntentRouter editRouter{tp.project, tp.eq, editModeReg};
    NavigatorRouter navRouter{tp.project, tp.eq, navReg};
    SelectIntentRouter selRouter{tp.project, tp.eq, selReg};
    EventCapture<SeekEvent> seeks;
    EventCapture<CommitAxisActionsEvent> commits;
    EventCapture<SetAxisSelectionEvent> selections;
    EventCapture<AxisSelectedEvent> activeSelections;
    EventCapture<SetPluginProjectDataEvent> pluginData;
    EventCapture<AddActionAtTimeEvent> adds;
    EventCapture<RemoveActionAtTimeEvent> removes;
    EventCapture<NotifyEvent> notifies;
    bool ready = false;

    explicit DotNetFixture(std::initializer_list<const char *> plugins) {
        // L0 present so axis forwarding and commitAxisActions aren't gated away.
        tp.project.axes[static_cast<size_t>(StandardAxis::L0)].showInStrip = true;
        // on<> must register before freeze(); attaching here keeps every case's captures valid.
        seeks.attach(tp.eq);
        commits.attach(tp.eq);
        selections.attach(tp.eq);
        activeSelections.attach(tp.eq);
        pluginData.attach(tp.eq);
        adds.attach(tp.eq);
        removes.attach(tp.eq);
        notifies.attach(tp.eq);
        tp.eq.freeze();

        if (!pm.init())
            return; // no .NET runtime / managed host
        if (!stage(plugins))
            return; // plugins not built next to the binary
        pm.loadPlugins();
        tp.eq.drain(); // flush RegisterPluginNodeEvent(s) pushed during OnLoad
        ready = true;
    }

    ~DotNetFixture() {
        std::vector<std::string> loaded;
        for (const auto &lp : pm.getPlugins())
            if (lp.enabled)
                loaded.push_back(lp.name);
        for (const auto &name : loaded)
            tp.eq.push(SetPluginEnabledEvent{.name = name, .enabled = false});
        tp.eq.drain();

        std::error_code ec;
        for (int i = 0; i < 50 && fs::exists(userPlugins()); ++i) {
            fs::remove_all(userPlugins(), ec);
            if (!fs::exists(userPlugins()))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20)); // OS lag releasing a just-unloaded DLL
        }
    }

    void send(auto e) {
        tp.eq.push(std::move(e));
        tp.eq.drain();
    }

    // Model one frame: run update()'s per-frame callbacks, then drain the events they push (the real
    // loop drains update()'s pushes on the next frame). Use after sending events whose plugin reaction
    // is dispatched from update() — e.g. the coalesced onAxisModified flush.
    void tick(float dt = 0.016f) {
        pm.update(dt);
        tp.eq.drain();
    }

    // Full unload+reload of a plugin so its OnLoad re-runs — the only path that re-constructs a plugin's
    // *Scoped values, and thus the only way to exercise their load-from-store Reload(). Trusted under
    // OFS_PLUGIN_TEST_HOOKS, so the enable path loads synchronously with no consent modal.
    void reload(const char *name) {
        send(SetPluginEnabledEvent{.name = name, .enabled = false});
        send(SetPluginEnabledEvent{.name = name, .enabled = true});
    }

    static fs::path userPlugins() { return ofs::util::getPrefPath() / "plugins"; }

  private:
    // Copy each requested plugin from the binary's plugins/ dir (where CMake staged it) into the test
    // pref dir, the only root loadPlugins() scans. Returns false if none were built (no .NET SDK).
    static bool stage(std::initializer_list<const char *> plugins) {
        std::error_code ec;
        fs::remove(ofs::util::getPrefPath() / "plugin_states.json", ec);
        fs::remove(ofs::util::getPrefPath() / "plugins_pending_uninstall.json", ec);
        fs::remove_all(ofs::util::getPrefPath() / "plugin_settings", ec); // start each test with no app settings
        for (int i = 0; i < 50 && fs::exists(userPlugins()); ++i) {
            fs::remove_all(userPlugins(), ec);
            if (!fs::exists(userPlugins()))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        bool any = false;
        for (const char *name : plugins) {
            const fs::path src = ofs::util::getBasePath() / "plugins" / name;
            const fs::path dst = userPlugins() / name;
            if (!fs::exists(src))
                continue;
            fs::create_directories(dst.parent_path(), ec);
            fs::copy(src, dst, fs::copy_options::recursive, ec);
            any = true;
        }
        return any;
    }
};

// A .NET-driven test case requires its fixture to be ready (host init + plugin staging both
// succeeded). .NET is mandatory: a fixture that failed to come up is a hard failure, never a skip.
#define REQUIRE_DOTNET(fx) REQUIRE((fx).ready)

} // namespace

// ── Load sanity: the real plugin registered its commands and node, and rejected the dead command ──

TEST_CASE("Probe plugin loads and round-trips its registrations through the bridge") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // OnLoad ran in managed code and called back through registerCommand (native -> managed -> native).
    CHECK(fx.cmdReg.find("Ofs.ProbePlugin.state") != nullptr);
    CHECK(fx.cmdReg.find("Ofs.ProbePlugin.commit") != nullptr);
    CHECK(fx.cmdReg.find("Ofs.ProbePlugin.ping") != nullptr);

    // registerNode round-tripped into the native effect registry, carrying the author-declared group +
    // icon + description (group overrides the default plugin-name bucket; icon is the curated NodeIcon enum
    // by value; description is the add-menu hover tooltip).
    REQUIRE(fx.effectReg.pluginNodes.count("Ofs.ProbePlugin.pnode") == 1);
    const auto &pnode = fx.effectReg.pluginNodes.at("Ofs.ProbePlugin.pnode");
    CHECK(pnode.category == "Probes");
    CHECK(pnode.icon == OfsNodeIconWaveform);
    CHECK(pnode.description == "Probe node description");

    // The real loaded plugin reports the host ABI version.
    const LoadedPlugin *lp = nullptr;
    for (const auto &p : fx.pm.getPlugins())
        if (p.name == "Ofs.ProbePlugin")
            lp = &p;
    REQUIRE(lp != nullptr);
    CHECK(lp->api.version == OFS_ABI_VERSION);

    // The "dead" command (listed in neither the rebind list nor the palette) was rejected by the managed
    // Register guard, so it never reached the registry.
    CHECK(fx.cmdReg.find("Ofs.ProbePlugin.dead") == nullptr);

    // A NodeShape with a duplicate input pin name throws in its constructor, so "dupinnode" never reached
    // registerNode and is absent from the effect registry.
    CHECK(fx.effectReg.pluginNodes.count("Ofs.ProbePlugin.dupinnode") == 0);
}

// ── Event -> managed callback dispatch (observed as encoded seeks) ────────────

TEST_CASE("PluginManager forwards player events to managed callbacks") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    fx.send(PlayStateChangedEvent{true});
    CHECK(channelArg(fx.seeks.received, kPlayBase) == doctest::Approx(1.0));
    fx.send(PlayStateChangedEvent{false});
    CHECK(channelArg(fx.seeks.received, kPlayBase) == doctest::Approx(0.0));

    fx.send(SpeedChangedEvent{2.5f});
    CHECK(channelArg(fx.seeks.received, kSpeedBase) == doctest::Approx(2.5));

    // "video.mp4" has length 9; the callback seeks MediaBase + path length.
    fx.send(MediaChangedEvent{"video.mp4"});
    CHECK(channelArg(fx.seeks.received, kMediaBase) == doctest::Approx(9.0));
}

TEST_CASE("PluginManager forwards LoadProjectEvent to onProjectChange") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    fx.send(LoadProjectEvent{});
    CHECK(channelCount(fx.seeks.received, kProjectMark) == 1);
}

TEST_CASE("PluginManager forwards AxisModifiedEvent only for present axes") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // The handler only marks the role dirty; update() flushes one coalesced callback per dirtied axis.
    // L0 is present (fixture) → forwarded; the seek encodes the role (L0 == 0).
    fx.send(AxisModifiedEvent{StandardAxis::L0});
    fx.tick();
    REQUIRE(channelCount(fx.seeks.received, kAxisModBase) == 1);
    CHECK(channelArg(fx.seeks.received, kAxisModBase) == doctest::Approx(static_cast<double>(StandardAxis::L0)));

    // Several events for the same axis in one frame collapse to a single callback at the next flush.
    fx.send(AxisModifiedEvent{StandardAxis::L0});
    fx.send(AxisModifiedEvent{StandardAxis::L0});
    fx.tick();
    CHECK(channelCount(fx.seeks.received, kAxisModBase) == 2);

    // R0 is absent → the present-guard drops it before it reaches the plugin.
    fx.send(AxisModifiedEvent{StandardAxis::R0});
    fx.tick();
    CHECK(channelCount(fx.seeks.received, kAxisModBase) == 2);
}

TEST_CASE("PluginManager forwards AxisSelectedEvent to onActiveAxisChanged") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    fx.send(AxisSelectedEvent{StandardAxis::R0});
    REQUIRE(channelCount(fx.seeks.received, kActiveBase) == 1);
    CHECK(channelArg(fx.seeks.received, kActiveBase) == doctest::Approx(static_cast<double>(StandardAxis::R0)));
}

TEST_CASE("SetLanguageEvent reloads plugins so they re-register strings in the new language") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // Registered at load in English: the plugin builds the title/name from Host.Language ("en") at OnLoad.
    const Command *cmd = fx.cmdReg.find("Ofs.ProbePlugin.langcmd");
    REQUIRE(cmd != nullptr);
    CHECK(cmd->title == "Cmd-en");
    REQUIRE(fx.effectReg.pluginNodes.count("Ofs.ProbePlugin.langnode") == 1);
    CHECK(fx.effectReg.pluginNodes.at("Ofs.ProbePlugin.langnode").displayName == "Node-en");

    // Host.Culture maps the active code to a CultureInfo: English → the invariant culture (the plugin's
    // neutral .resx). CultureFromCode's "en"/empty branch.
    fx.pm.firePluginCommand("Ofs.ProbePlugin", "culture");
    fx.tp.eq.drain();
    CHECK(channelArg(fx.seeks.received, kCultureInvariant) == doctest::Approx(1.0));

    // onSetLanguage reads the host's ACTIVE ISO 639 code from the Translator (never from the event's
    // languageId / filename), so set the Translator first. ja_[AI].toml declares [_meta] iso639 = "ja".
    // The host then unloads and reloads the plugin; OnLoad re-runs with Host.Language == "ja", so it
    // re-registers the command (synchronously) and the node (via RegisterPluginNodeEvent, drained in the
    // same pass) under the same ids with the now-Japanese strings.
    REQUIRE(ofs::loc::Translator::instance().load("ja_[AI]"));
    fx.send(SetLanguageEvent{.languageId = "ja_[AI]"});

    CHECK(fx.cmdReg.find("Ofs.ProbePlugin.langcmd")->title == "Cmd-ja");
    CHECK(fx.effectReg.pluginNodes.at("Ofs.ProbePlugin.langnode").displayName == "Node-ja");

    // Under "ja", CultureFromCode takes the GetCultureInfo branch, so Host.Culture is no longer invariant.
    fx.pm.firePluginCommand("Ofs.ProbePlugin", "culture");
    fx.tp.eq.drain();
    CHECK(channelArg(fx.seeks.received, kCultureInvariant) == doctest::Approx(0.0));

    ofs::loc::Translator::instance().loadDefaults(); // restore English for the rest of the suite
}

// ── update(): onTimeChange dedup threshold + onUpdate cadence ─────────────────

TEST_CASE("PluginManager::update fires onTimeChange only past the time threshold") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // First update from the -1.0 sentinel always reports.
    fx.tp.project.playback.cursorPos = 0.0;
    fx.tick();
    REQUIRE(channelCount(fx.seeks.received, kTimeBase) == 1);
    CHECK(channelArg(fx.seeks.received, kTimeBase) == doctest::Approx(0.0));

    // Unchanged cursor → no new report.
    fx.tick();
    CHECK(channelCount(fx.seeks.received, kTimeBase) == 1);

    // Sub-threshold move (<0.001) → still no report.
    fx.tp.project.playback.cursorPos = 0.0005;
    fx.tick();
    CHECK(channelCount(fx.seeks.received, kTimeBase) == 1);

    // Clear move → reports.
    fx.tp.project.playback.cursorPos = 0.5;
    fx.tick();
    REQUIRE(channelCount(fx.seeks.received, kTimeBase) == 2);
    CHECK(channelArg(fx.seeks.received, kTimeBase) == doctest::Approx(0.5));

    // onUpdate fires every frame regardless of the time threshold.
    CHECK(channelCount(fx.seeks.received, kUpdateMark) == 4);
}

TEST_CASE("PluginManager resets the reported time on project load") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    fx.tp.project.playback.cursorPos = 1.0;
    fx.tick();
    REQUIRE(channelCount(fx.seeks.received, kTimeBase) == 1);

    // Loading a project resets lastReportedTime_, so the same cursor reports again.
    fx.send(LoadProjectEvent{});
    fx.tick();
    REQUIRE(channelCount(fx.seeks.received, kTimeBase) == 2);
    CHECK(channelArg(fx.seeks.received, kTimeBase) == doctest::Approx(1.0));
}

// ── HostApi reads: the managed Player/Axes wrappers read live project state ───

TEST_CASE("Managed Player/Axes wrappers read live project state") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    fx.tp.project.playback.cursorPos = 3.25;
    fx.player->paused = false; // isPlaying == 1
    fx.tp.project.state.activeAxis = StandardAxis::L0;
    fx.tp.project.mutate(
        StandardAxis::L0,
        [](AxisState &a) {
            a.actions.insert({1.0, 40});
            a.actions.insert({2.0, 60});
        },
        fx.tp.eq);
    fx.tp.eq.drain();

    // The "state" command reads each value through the managed wrappers and seeks it back encoded.
    fx.pm.firePluginCommand("Ofs.ProbePlugin", "state");
    fx.tp.eq.drain();

    const auto &s = fx.seeks.received;
    CHECK(channelArg(s, kStTime) == doctest::Approx(3.25));
    CHECK(channelArg(s, kStPlaying) == doctest::Approx(1.0));
    CHECK(channelArg(s, kStSpeed) == doctest::Approx(100.0));  // speed 1.0 * 100
    CHECK(channelArg(s, kStExisting) == doctest::Approx(1.0)); // only L0 exists
    CHECK(channelArg(s, kStActionCount) == doctest::Approx(2.0));
    CHECK(channelArg(s, kStFirstPos) == doctest::Approx(40.0));
    CHECK(channelArg(s, kStSecondPos) == doctest::Approx(60.0));
    CHECK(channelArg(s, kStActive) == doctest::Approx(static_cast<double>(StandardAxis::L0)));
}

TEST_CASE("Managed AxisEdit.Commit emits a CommitAxisActionsEvent") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // The fixture pre-registered the CommitAxisActionsEvent capture before freeze().
    fx.pm.firePluginCommand("Ofs.ProbePlugin", "commit");
    fx.tp.eq.drain();

    REQUIRE(fx.commits.received.size() == 1);
    CHECK(fx.commits.received[0].axis == StandardAxis::L0);
    REQUIRE(fx.commits.received[0].actions.size() == 2);
    CHECK(fx.commits.received[0].actions[0].pos == 10);
    CHECK(fx.commits.received[0].actions[1].pos == 90);
}

TEST_CASE("Managed AxisEdit rejects a stale cross-frame commit instead of clobbering") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // Begin + stash an edit on the current frame (no commit yet).
    fx.pm.firePluginCommand("Ofs.ProbePlugin", "editstash");
    fx.tp.eq.drain();

    // Advance a frame: OnUpdate bumps the managed FrameGen, so the stashed edit is now from a prior frame.
    fx.tick();

    // Committing the stale edit must throw managed-side (caught → StaleRejected) and push NO commit event,
    // so it can't replay its buffered snapshot over anything that changed since.
    fx.pm.firePluginCommand("Ofs.ProbePlugin", "commitstash");
    fx.tp.eq.drain();

    CHECK(sawSeek(fx.seeks.received, kStaleRejected));
    CHECK_FALSE(sawSeek(fx.seeks.received, kStaleCommitted));
    CHECK(fx.commits.received.empty());
}

TEST_CASE("Managed AxisMath/AxisEdit lookups and buffered mutators are correct") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // Seed L0 with the two actions the "axisapi" command's expected results are computed against.
    fx.tp.project.mutate(
        StandardAxis::L0,
        [](AxisState &a) {
            a.actions.insert({1.0, 40});
            a.actions.insert({2.0, 60});
        },
        fx.tp.eq);
    fx.tp.eq.drain();

    fx.pm.firePluginCommand("Ofs.ProbePlugin", "axisapi");
    fx.tp.eq.drain();

    // Every sub-check (ClosestTo branches, IsSelected miss, buffered Add/Actions/Before/After/Clear, and
    // the spent-edit throw) passed → the encoded count equals the total.
    CHECK(channelArg(fx.seeks.received, kAxisApiOk) == doctest::Approx(kAxisApiChecks));

    // The net-empty selection edit committed through the ClearAxisSelection path: a SetAxisSelectionEvent
    // for L0 carrying an empty selection (the uncommitted action edit pushed no CommitAxisActionsEvent).
    REQUIRE(fx.selections.received.size() == 1);
    CHECK(fx.selections.received[0].axis == StandardAxis::L0);
    CHECK(fx.selections.received[0].selection.empty());
    CHECK(fx.commits.received.empty());
}

TEST_CASE("Managed Axis ScriptAction-overload lookups agree with their double counterparts") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // Same seed as the axisapi test — the overloads must produce the same answers.
    fx.tp.project.mutate(
        StandardAxis::L0,
        [](AxisState &a) {
            a.actions.insert({1.0, 40});
            a.actions.insert({2.0, 60});
        },
        fx.tp.eq);
    fx.tp.eq.drain();

    fx.pm.firePluginCommand("Ofs.ProbePlugin", "axisapi2");
    fx.tp.eq.drain();

    CHECK(channelArg(fx.seeks.received, kAxisApi2Ok) == doctest::Approx(kAxisApi2Checks));
}

TEST_CASE("Managed AxisEdit range/overload selection mutators resolve to the right net selection") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    fx.tp.project.mutate(
        StandardAxis::L0,
        [](AxisState &a) {
            a.actions.insert({1.0, 40});
            a.actions.insert({2.0, 60});
            a.actions.insert({3.0, 55});
        },
        fx.tp.eq);
    fx.tp.eq.drain();

    fx.pm.firePluginCommand("Ofs.ProbePlugin", "axisselrange");
    fx.tp.eq.drain();

    // SelectRange(1..3) then Deselect 3.0 (DeselectRange) and 1.0 (overload) leaves exactly {2.0}.
    REQUIRE(fx.selections.received.size() == 1);
    CHECK(fx.selections.received[0].axis == StandardAxis::L0);
    REQUIRE(fx.selections.received[0].selection.size() == 1);
    CHECK(fx.selections.received[0].selection.begin()->at == doctest::Approx(2.0));
    CHECK(fx.commits.received.empty()); // no action mutation, only a selection commit
}

TEST_CASE("Managed EditIntent factories set the fields named for each gesture kind") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    fx.pm.firePluginCommand("Ofs.ProbePlugin", "editintents");
    fx.tp.eq.drain();

    CHECK(channelArg(fx.seeks.received, kEditIntentsOk) == doctest::Approx(kEditIntentsChecks));
    CHECK(sawSeek(fx.seeks.received, kStepTimeRan)); // Editing.StepTime read without throwing
}

TEST_CASE("Plugin edit mode Replace via the IEnumerable overload applies the listed intents") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    REQUIRE(fx.editModeReg.find("Ofs.ProbePlugin.listmode") != nullptr);
    fx.send(SetActiveEditModeEvent{.id = "Ofs.ProbePlugin.listmode"});

    fx.send(EditRequestEvent{
        .intent = EditIntent{.kind = EditIntentKind::AddPoint, .axis = StandardAxis::L0, .time = 1.0, .pos = 50}});

    REQUIRE(fx.adds.received.size() == 2);
    CHECK(fx.adds.received[0].time == doctest::Approx(1.0));
    CHECK(fx.adds.received[0].pos == 11);
    CHECK(fx.adds.received[1].time == doctest::Approx(2.0));
    CHECK(fx.adds.received[1].pos == 22);
}

TEST_CASE("Plugin edit mode ReplacePerAxis via the IEnumerable overload re-consults per follower axis") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    fx.tp.project.state.activeAxis = StandardAxis::L0;
    fx.tp.project.state.axesGrouping.set(static_cast<size_t>(StandardAxis::L0));
    fx.tp.project.state.axesGrouping.set(static_cast<size_t>(StandardAxis::R0));

    fx.send(SetActiveEditModeEvent{.id = "Ofs.ProbePlugin.peraxislistmode"});
    REQUIRE(fx.tp.project.activeEditMode == "Ofs.ProbePlugin.peraxislistmode");

    fx.send(EditRequestEvent{
        .intent = EditIntent{.kind = EditIntentKind::AddPoint, .axis = StandardAxis::L0, .time = 1.0, .pos = 50}});

    // pos = (int)axis + 1, computed per re-consulted axis: L0 (index 0) → 1, R0 → its index + 1.
    REQUIRE(fx.adds.received.size() == 2);
    const auto byAxis = [&](StandardAxis ax) -> const AddActionAtTimeEvent * {
        for (const auto &e : fx.adds.received)
            if (e.axis == ax)
                return &e;
        return nullptr;
    };
    REQUIRE(byAxis(StandardAxis::L0) != nullptr);
    REQUIRE(byAxis(StandardAxis::R0) != nullptr);
    CHECK(byAxis(StandardAxis::L0)->pos == static_cast<int>(StandardAxis::L0) + 1);
    CHECK(byAxis(StandardAxis::R0)->pos == static_cast<int>(StandardAxis::R0) + 1);
}

TEST_CASE("Managed Axis snapshot rejects a cross-frame read instead of returning stale rows") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // Stash the snapshot this frame, advance a frame, then read it: Axis.CheckFresh must throw.
    fx.pm.firePluginCommand("Ofs.ProbePlugin", "freshstash");
    fx.tp.eq.drain();
    fx.tick();
    fx.pm.firePluginCommand("Ofs.ProbePlugin", "freshcheck");
    fx.tp.eq.drain();

    CHECK(sawSeek(fx.seeks.received, kFreshRejected));
    CHECK_FALSE(sawSeek(fx.seeks.received, kFreshLived));
}

TEST_CASE("Managed Axes rejects an absent axis instead of yielding an empty view / silent no-op") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // S0 doesn't exist in the default project (no data, not shown, not locked): both Axes[S0] and
    // SetActive(S0) must throw managed-side (caught → AbsentRejected + count) — a plugin can only touch
    // existing axes and cannot create one.
    fx.pm.firePluginCommand("Ofs.ProbePlugin", "absentaxis");
    fx.tp.eq.drain();

    CHECK(channelArg(fx.seeks.received, kAbsentRejected) == doctest::Approx(kAbsentChecks));
}

TEST_CASE("Managed Axes indexer grows its scratch buffer for an over-large axis") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // The scratch buffer starts at 4096; seed more than that so reading the axis must grow it.
    constexpr int kCount = 4500;
    fx.tp.project.mutate(
        StandardAxis::L0,
        [](AxisState &a) {
            for (int i = 0; i < kCount; ++i)
                a.actions.insert({static_cast<double>(i), i % 100});
        },
        fx.tp.eq);
    fx.tp.eq.drain();

    fx.pm.firePluginCommand("Ofs.ProbePlugin", "scratchgrow");
    fx.tp.eq.drain();

    CHECK(sawSeek(fx.seeks.received, kScratchOk, 1.0)); // grew and still returned > 4096 rows
}

// ── HostApi additions: axis/project/player state reads via the managed wrappers ──

TEST_CASE("Managed Axes/Project/Player additions read live state (state2)") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    auto &proj = fx.tp.project;
    auto &l0 = proj.axes[static_cast<size_t>(StandardAxis::L0)]; // present (fixture)
    l0.isVisible = false;                                        // non-default so the read is unambiguous
    l0.isLocked = true;
    proj.state.settingsDirty = true;                      // isProjectDirty → true
    proj.metadata.title = "Hi";                           // length 2
    proj.metadata.tags = {"a", "bb"};                     // count 2
    proj.metadata.performers = {"p"};                     // count 1
    proj.metadata.customFields.push_back({"foo", "bar"}); // 1 non-standard key
    proj.bookmarks.bookmarks.push_back({1.0, "b"});
    proj.bookmarks.chapters.push_back({0.0, 1.0, "c1"});
    proj.bookmarks.chapters.push_back({1.0, 2.0, "c2"});
    proj.regions.emplace_back(); // count 1 is all getRegionCount reports

    fx.pm.firePluginCommand("Ofs.ProbePlugin", "state2");
    fx.tp.eq.drain();

    const auto &s = fx.seeks.received;
    CHECK(channelArg(s, kStVisible) == doctest::Approx(0.0));         // isVisible == false
    CHECK(channelArg(s, kStLocked) == doctest::Approx(1.0));          // isLocked  == true
    CHECK(channelArg(s, kStName).value_or(0.0) > 0.0);                // non-empty axis name
    CHECK(channelArg(s, kStVolume) == doctest::Approx(100.0));        // FakeVideoPlayer volume 1.0 → *100
    CHECK(channelArg(s, kStFps) == doctest::Approx(30.0));            // FakeVideoPlayer fps 30
    CHECK(channelArg(s, kStVideoDims) == doctest::Approx(kNullRole)); // width 0 → wrapper null sentinel
    CHECK(channelArg(s, kStDirty) == doctest::Approx(1.0));
    CHECK(channelArg(s, kStChapters) == doctest::Approx(2.0));
    CHECK(channelArg(s, kStBookmarks) == doctest::Approx(1.0));
    CHECK(channelArg(s, kStRegions) == doctest::Approx(1.0));
    CHECK(channelArg(s, kStTitle) == doctest::Approx(2.0));
    CHECK(channelArg(s, kStTagCount) == doctest::Approx(2.0));
    CHECK(channelArg(s, kStPerformerCount) == doctest::Approx(1.0));
    CHECK(channelArg(s, kStCustomCount) == doctest::Approx(1.0));
}

// ── Per-project data store: ProjectScoped through the real bridge ──

TEST_CASE("Managed ProjectScoped reloads per project open and syncs edits back") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // At load the project stored nothing under "scoped", so the value is a fresh default (N == 0).
    fx.pm.firePluginCommand("Ofs.ProbePlugin", "scopedread");
    fx.tp.eq.drain();
    CHECK(channelArg(fx.seeks.received, kStScopedN) == doctest::Approx(0.0));

    // Opening a project with stored data reloads the scoped value (ProjectChanged → ProjectScoped.Reload).
    fx.tp.project.pluginData["Ofs.ProbePlugin"]["scoped"] = nlohmann::json{{"N", 9}, {"Flag", false}};
    fx.send(LoadProjectEvent{});
    fx.pm.firePluginCommand("Ofs.ProbePlugin", "scopedread");
    fx.tp.eq.drain();
    CHECK(channelArg(fx.seeks.received, kStScopedN) == doctest::Approx(9.0));

    // Editing the value persists it back under the plugin's own "scoped" key on the next frame's
    // auto-flush (FlushScopedValues at the top of OnUpdate) — no manual Sync.
    fx.pm.firePluginCommand("Ofs.ProbePlugin", "scopededit");
    fx.tick();
    REQUIRE_FALSE(fx.pluginData.received.empty());
    const auto &last = fx.pluginData.received.back();
    CHECK(last.pluginName == "Ofs.ProbePlugin");
    CHECK(last.key == "scoped");
    CHECK(last.value["N"] == 3);

    // Opening a project with nothing stored resets the scoped value to a fresh default — no bleed.
    fx.tp.project.pluginData = nlohmann::json::object();
    fx.send(LoadProjectEvent{});
    fx.pm.firePluginCommand("Ofs.ProbePlugin", "scopedread");
    fx.tp.eq.drain();
    CHECK(channelArg(fx.seeks.received, kStScopedN) == doctest::Approx(0.0));
}

TEST_CASE("Managed ProjectScoped loads the project's stored value at OnLoad") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // Seed the project's per-plugin store, then reload so the plugin's OnLoad constructs ProjectScoped
    // against a non-empty project — the constructor's Reload (non-Undefined branch), not a later
    // ProjectChanged. The value must be live on the very first read, with no LoadProjectEvent.
    fx.tp.project.pluginData["Ofs.ProbePlugin"]["scoped"] = nlohmann::json{{"N", 7}, {"Flag", true}};
    fx.reload("Ofs.ProbePlugin");

    fx.pm.firePluginCommand("Ofs.ProbePlugin", "scopedread");
    fx.tp.eq.drain();
    CHECK(channelArg(fx.seeks.received, kStScopedN) == doctest::Approx(7.0));
}

// ── Per-plugin app settings: AppScoped persisted to <pref>/plugin_settings/<plugin>.json ──

TEST_CASE("Managed AppScoped persists global settings to a per-plugin file, debounced per frame") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // stage() cleared <pref>/plugin_settings, so the plugin loaded with nothing stored: a fresh default.
    const fs::path settingsFile = ofs::util::getPrefPath() / "plugin_settings" / "Ofs.ProbePlugin.json";
    REQUIRE_FALSE(fs::exists(settingsFile));
    fx.pm.firePluginCommand("Ofs.ProbePlugin", "appscopedread");
    fx.tp.eq.drain();
    CHECK(channelArg(fx.seeks.received, kStAppScopedN) == doctest::Approx(0.0));

    // Editing the value persists it on the next frame's flush (FlushScopedValues → SetAppData → the
    // host's debounced once-per-frame disk write in update()) — no manual Sync, and unlike ProjectScoped
    // it never touches the project's pluginData.
    fx.pm.firePluginCommand("Ofs.ProbePlugin", "appscopededit");
    fx.tick();
    REQUIRE(fx.pluginData.received.empty()); // app settings are NOT per-project data
    REQUIRE(fs::exists(settingsFile));
    nlohmann::json doc;
    std::ifstream(settingsFile) >> doc;
    CHECK(doc["appscoped"]["N"] == 5);
}

TEST_CASE("Managed AppScoped loads its persisted value back at OnLoad") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // Persist a non-default value through the normal edit→flush path so the store holds N == 5.
    fx.pm.firePluginCommand("Ofs.ProbePlugin", "appscopededit");
    fx.tick();
    REQUIRE(fs::exists(ofs::util::getPrefPath() / "plugin_settings" / "Ofs.ProbePlugin.json"));

    // Reloading re-runs OnLoad → AppScoped.Reload reads the stored value (the non-Undefined GetAppData
    // branch) rather than a fresh default. Unlike ProjectScoped this is the *only* trigger that reloads
    // app settings — they never reload mid-session.
    fx.reload("Ofs.ProbePlugin");
    fx.pm.firePluginCommand("Ofs.ProbePlugin", "appscopedread");
    fx.tp.eq.drain();
    CHECK(channelArg(fx.seeks.received, kStAppScopedN) == doctest::Approx(5.0));
}

TEST_CASE("Managed axis writes emit host events: SetActive") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // setActiveAxis no-ops on an absent axis, so make R0 present before asking the plugin to focus it.
    fx.tp.project.axes[static_cast<size_t>(StandardAxis::R0)].showInStrip = true;
    fx.pm.firePluginCommand("Ofs.ProbePlugin", "setactive");
    fx.tp.eq.drain();
    REQUIRE(fx.activeSelections.received.size() == 1);
    CHECK(fx.activeSelections.received[0].role == StandardAxis::R0);
}

TEST_CASE("HostApi rejects a managed read from a non-main thread") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // The "worker" command reads Host.Player.Time on a Task thread; the managed main-thread guard
    // throws, and the command seeks WorkerRejected (never WorkerOk).
    fx.pm.firePluginCommand("Ofs.ProbePlugin", "worker");
    fx.tp.eq.drain();

    CHECK(sawSeek(fx.seeks.received, kWorkerRejected));
    CHECK_FALSE(sawSeek(fx.seeks.received, kWorkerOk));
}

// ── Host surface: toasts, misc services, main-thread marshaling ──────────────

TEST_CASE("Host.Notify* raise NotifyEvents the host prefixes with the plugin name") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    fx.pm.firePluginCommand("Ofs.ProbePlugin", "notify");
    fx.tp.eq.drain();

    // Five toasts: Info/Success/Warning/Error helpers + the level-taking Notify. Each is prefixed "[plugin]".
    REQUIRE(fx.notifies.received.size() == 5);
    for (const auto &n : fx.notifies.received)
        CHECK(n.message.rfind("[Ofs.ProbePlugin] ", 0) == 0);
    CHECK(fx.notifies.received[0].level == NotifyLevel::Info);
    CHECK(fx.notifies.received[1].level == NotifyLevel::Success);
    CHECK(fx.notifies.received[2].level == NotifyLevel::Warning);
    CHECK(fx.notifies.received[3].level == NotifyLevel::Error);
}

TEST_CASE("Host misc services: IsMainThread, UnloadToken, Log") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    fx.pm.firePluginCommand("Ofs.ProbePlugin", "hostmisc");
    fx.tp.eq.drain();

    const auto &s = fx.seeks.received;
    CHECK(channelArg(s, kIsMainMark) == doctest::Approx(1.0)); // the command runs on the main thread
    CHECK(channelArg(s, kUnloadLive) == doctest::Approx(1.0)); // not unloaded mid-command
}

TEST_CASE("Host.RunOnMainThread(Async) work runs when the host pumps the queue next frame") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // The command only enqueues; nothing has run yet.
    fx.pm.firePluginCommand("Ofs.ProbePlugin", "runmain");
    fx.tp.eq.drain();
    CHECK(channelCount(fx.seeks.received, kRunFire) == 0);
    CHECK(channelCount(fx.seeks.received, kRunAsyncAction) == 0);
    CHECK(channelCount(fx.seeks.received, kRunAsyncFunc) == 0);

    // A frame ticks the bridge, which pumps the queue at the top of OnUpdate; all three works now run.
    fx.tick();
    CHECK(channelCount(fx.seeks.received, kRunFire) == 1);
    CHECK(channelCount(fx.seeks.received, kRunAsyncAction) == 1);
    CHECK(channelCount(fx.seeks.received, kRunAsyncFunc) == 1);
}

TEST_CASE("Managed Player additions: Duration / MediaPath / VideoHeight reads and the setters + TogglePlay") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    fx.pm.firePluginCommand("Ofs.ProbePlugin", "player2");
    fx.tp.eq.drain();

    const auto &s = fx.seeks.received;
    CHECK(channelArg(s, kStDuration2) == doctest::Approx(0.0));    // FakeVideoPlayer duration 0
    CHECK(channelArg(s, kStMediaLen) == doctest::Approx(0.0));     // no media → empty path
    CHECK(channelArg(s, kStVideoH) == doctest::Approx(kNullRole)); // height 0 → wrapper null sentinel
}

TEST_CASE("Managed Dialogs additions: SaveFile + PickFolder queue a dialog without throwing") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    fx.pm.firePluginCommand("Ofs.ProbePlugin", "dialogs2");
    fx.tp.eq.drain();
    CHECK(sawSeek(fx.seeks.received, kDialogs2Ran)); // both calls returned a Task; the command completed
}

// ── Command + shortcut interception ──────────────────────────────────────────

TEST_CASE("firePluginCommand dispatches to the named plugin's onCommand") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    fx.pm.firePluginCommand("Ofs.ProbePlugin", "ping");
    fx.tp.eq.drain();
    CHECK(sawSeek(fx.seeks.received, kPingMark));

    // Unknown plugin name → no dispatch.
    const int before = static_cast<int>(fx.seeks.received.size());
    fx.pm.firePluginCommand("nope", "ping");
    fx.tp.eq.drain();
    CHECK(static_cast<int>(fx.seeks.received.size()) == before);
}

// ── Enable/disable lifecycle ─────────────────────────────────────────────────

TEST_CASE("Disabling a plugin tears it down and stops dispatch") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    REQUIRE(fx.effectReg.pluginNodes.count("Ofs.ProbePlugin.pnode") == 1);

    fx.send(SetPluginEnabledEvent{.name = "Ofs.ProbePlugin", .enabled = false});

    // Unload unregistered the plugin's nodes from the effect registry.
    CHECK(fx.effectReg.pluginNodes.count("Ofs.ProbePlugin.pnode") == 0);

    // A disabled plugin receives no further events.
    const int before = static_cast<int>(fx.seeks.received.size());
    fx.send(PlayStateChangedEvent{true});
    fx.send(AxisModifiedEvent{StandardAxis::L0});
    fx.tick();
    CHECK(static_cast<int>(fx.seeks.received.size()) == before);
}

// ── State persistence (two real plugins, different enabled flags) ─────────────

TEST_CASE("savePluginStates writes each loaded plugin's enabled flag to disk") {
    DotNetFixture fx{"Ofs.TestPlugin", "Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // Both load enabled; disable one so the two entries carry different flags.
    fx.send(SetPluginEnabledEvent{.name = "Ofs.ProbePlugin", .enabled = false});

    std::filesystem::create_directories(ofs::util::getPrefPath());
    fx.pm.savePluginStates();

    const auto path = ofs::util::getPrefPath() / "plugin_states.json";
    std::ifstream file(path);
    REQUIRE(file.is_open());
    const auto j = nlohmann::json::parse(file);

    bool sawTest = false, sawProbe = false;
    for (const auto &entry : j) {
        if (entry.at("name") == "Ofs.TestPlugin") {
            sawTest = true;
            CHECK(entry.at("enabled") == true);
        } else if (entry.at("name") == "Ofs.ProbePlugin") {
            sawProbe = true;
            CHECK(entry.at("enabled") == false);
        }
    }
    CHECK(sawTest);
    CHECK(sawProbe);
}

// ── Plugin-node TState JSON codec end-to-end (real CoreCLR, phase 4d) ─────────
// Boots the runtime, loads Ofs.ProbePlugin (which registers a TState "stateoffset" modifier), wires the
// real capture/release codec, and evaluates a region through ProcessingSystem. The node's eval is
// output = input ± (Offset + Scratch), so the resolved action position reveals exactly what the JSON
// decoded to — letting a C++ test assert STJ round-trip, enum-by-name, default-on-missing,
// whole-node-default-on-type-error, and that every field reaches eval, all without
// peeking into managed memory.
namespace {
struct NodeStateFixture {
    TestProject tp;
    std::shared_ptr<FakeVideoPlayer> player = std::make_shared<FakeVideoPlayer>();
    std::shared_ptr<FakeVideoPlayer> dummy = std::make_shared<FakeVideoPlayer>();
    CommandRegistry cmdReg{tp.eq};
    RebindState rebind;
    BindingSystem binding{tp.eq, cmdReg, rebind};
    EffectRegistryState effectReg;
    JobSystem jobSystem;
    ProcessingSystem ps{tp.project, effectReg, tp.scriptReg, tp.eq, jobSystem};
    PluginManager pm{tp.project, tp.eq, player, dummy, cmdReg, binding, effectReg};
    EventCapture<SeekEvent> seeks; // for the cancellation probe, which reports via Host.Player.Seek
    bool ready = false;

    NodeStateFixture() {
        tp.project.axes[static_cast<size_t>(StandardAxis::L0)].showInStrip = true;
        seeks.attach(tp.eq); // before freeze(): on<> registrations must all precede it
        tp.eq.freeze();      // ProcessingSystem + PluginManager registered their handlers in their ctors
        if (!pm.init())
            return;
        if (!stageProbe())
            return;
        pm.loadPlugins();
        tp.eq.drain(); // flush RegisterPluginNodeEvent(s)
        jobSystem.start();
        ready = effectReg.pluginNodes.count("Ofs.ProbePlugin.stateoffset") == 1;
    }

    ~NodeStateFixture() {
        for (const auto &lp : pm.getPlugins())
            if (lp.enabled)
                tp.eq.push(SetPluginEnabledEvent{.name = lp.name, .enabled = false});
        tp.eq.drain();
        std::error_code ec;
        for (int i = 0; i < 50 && fs::exists(DotNetFixture::userPlugins()); ++i) {
            fs::remove_all(DotNetFixture::userPlugins(), ec);
            if (!fs::exists(DotNetFixture::userPlugins()))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // Evaluate L0 over one region whose graph is Input → stateoffset(nodeState) → Output, with a single
    // input action at pos 50. Returns the resolved position (50 ± decoded offset). Re-runnable per case.
    int evalWith(const std::string &nodeStateJson) {
        auto &proj = tp.project;
        proj.regions.clear();

        ProcessingNodeGraph g;
        const int inId = g.allocId();
        const int nodeId = g.allocId();
        const int outId = g.allocId();
        g.nodes.push_back({.id = inId, .type = GraphNodeType::Input, .role = StandardAxis::L0});
        ProcessingGraphNode pn;
        pn.id = nodeId;
        pn.type = GraphNodeType::PluginNode;
        pn.pluginNodeId = "Ofs.ProbePlugin.stateoffset";
        pn.nodeState = nodeStateJson;
        pn.role = StandardAxis::L0;
        g.nodes.push_back(pn);
        g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
        g.links.push_back({.id = g.allocId(), .fromNode = inId, .toNode = nodeId, .toPin = 0});
        g.links.push_back({.id = g.allocId(), .fromNode = nodeId, .toNode = outId, .toPin = 0});

        ProcessingRegion r;
        r.id = 1;
        r.startTime = 0.0;
        r.endTime = 10.0;
        r.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
        r.nodeGraph = g;
        proj.regions = {r};
        proj.sortRegions();

        auto &axis = proj.axes[static_cast<size_t>(StandardAxis::L0)];
        proj.mutate(
            StandardAxis::L0,
            [](AxisState &a) {
                a.actions.clear();
                a.actions.insert({1.0, 50});
            },
            tp.eq);
        tp.eq.drain();

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (axis.pendingEval != nullptr && std::chrono::steady_clock::now() < deadline) {
            tp.eq.drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!axis.resolved || axis.resolved->actions.empty())
            return -1;
        return axis.resolved->actions[0].pos;
    }

    // Evaluate L0 over Input → <pluginNodeId>(nodeState) → Output with the given input actions, and return
    // every resolved output position. Lets a test see all samples of one eval at once (e.g. to prove a
    // factory closure was reused across them). Re-runnable per case.
    std::vector<int> evalAll(const char *pluginNodeId, const std::string &nodeStateJson,
                             std::initializer_list<ofs::ScriptAxisAction> inputs) {
        auto &proj = tp.project;
        proj.regions.clear();

        ProcessingNodeGraph g;
        const int inId = g.allocId();
        const int nodeId = g.allocId();
        const int outId = g.allocId();
        g.nodes.push_back({.id = inId, .type = GraphNodeType::Input, .role = StandardAxis::L0});
        ProcessingGraphNode pn;
        pn.id = nodeId;
        pn.type = GraphNodeType::PluginNode;
        pn.pluginNodeId = pluginNodeId;
        pn.nodeState = nodeStateJson;
        pn.role = StandardAxis::L0;
        g.nodes.push_back(pn);
        g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
        g.links.push_back({.id = g.allocId(), .fromNode = inId, .toNode = nodeId, .toPin = 0});
        g.links.push_back({.id = g.allocId(), .fromNode = nodeId, .toNode = outId, .toPin = 0});

        ProcessingRegion r;
        r.id = 1;
        r.startTime = 0.0;
        r.endTime = 10.0;
        r.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
        r.nodeGraph = g;
        proj.regions = {r};
        proj.sortRegions();

        auto &axis = proj.axes[static_cast<size_t>(StandardAxis::L0)];
        proj.mutate(
            StandardAxis::L0,
            [&](AxisState &a) {
                a.actions.clear();
                for (const auto &act : inputs)
                    a.actions.insert(act);
            },
            tp.eq);
        tp.eq.drain();

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (axis.pendingEval != nullptr && std::chrono::steady_clock::now() < deadline) {
            tp.eq.drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::vector<int> out;
        if (axis.resolved)
            for (const auto &a : axis.resolved->actions)
                out.push_back(a.pos);
        return out;
    }

    // Evaluate a two-input plugin combiner: pin0 = Input(L0), pin1 = Input(R0), node → Output(L0). Both
    // inputs are seeded with the same actions, so a symmetric combiner ((a+b)/2, or a discrete merge)
    // resolves to those positions. Returns every resolved output position. Re-runnable per case.
    std::vector<int> evalComb(const char *pluginNodeId, std::initializer_list<ofs::ScriptAxisAction> inputs) {
        auto &proj = tp.project;
        proj.axes[static_cast<size_t>(StandardAxis::R0)].showInStrip = true; // pin1's input axis must be present
        proj.regions.clear();

        ProcessingNodeGraph g;
        const int inL = g.allocId();
        const int inR = g.allocId();
        const int nodeId = g.allocId();
        const int outId = g.allocId();
        g.nodes.push_back({.id = inL, .type = GraphNodeType::Input, .role = StandardAxis::L0});
        g.nodes.push_back({.id = inR, .type = GraphNodeType::Input, .role = StandardAxis::R0});
        ProcessingGraphNode pn;
        pn.id = nodeId;
        pn.type = GraphNodeType::PluginNode;
        pn.pluginNodeId = pluginNodeId;
        pn.role = StandardAxis::L0;
        g.nodes.push_back(pn);
        g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
        g.links.push_back({.id = g.allocId(), .fromNode = inL, .toNode = nodeId, .toPin = 0});
        g.links.push_back({.id = g.allocId(), .fromNode = inR, .toNode = nodeId, .toPin = 1});
        g.links.push_back({.id = g.allocId(), .fromNode = nodeId, .toNode = outId, .toPin = 0});

        ProcessingRegion r;
        r.id = 1;
        r.startTime = 0.0;
        r.endTime = 10.0;
        r.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
        r.nodeGraph = g;
        proj.regions = {r};
        proj.sortRegions();

        // Seed R0 first; then the L0 mutate submits the region eval, whose snapshot carries both axes.
        proj.mutate(
            StandardAxis::R0,
            [&](AxisState &a) {
                a.actions.clear();
                for (const auto &act : inputs)
                    a.actions.insert(act);
            },
            tp.eq);
        auto &axis = proj.axes[static_cast<size_t>(StandardAxis::L0)];
        proj.mutate(
            StandardAxis::L0,
            [&](AxisState &a) {
                a.actions.clear();
                for (const auto &act : inputs)
                    a.actions.insert(act);
            },
            tp.eq);
        tp.eq.drain();

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (axis.pendingEval != nullptr && std::chrono::steady_clock::now() < deadline) {
            tp.eq.drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::vector<int> out;
        if (axis.resolved)
            for (const auto &a : axis.resolved->actions)
                out.push_back(a.pos);
        return out;
    }

  private:
    static bool stageProbe() {
        std::error_code ec;
        fs::remove(ofs::util::getPrefPath() / "plugin_states.json", ec);
        for (int i = 0; i < 50 && fs::exists(DotNetFixture::userPlugins()); ++i) {
            fs::remove_all(DotNetFixture::userPlugins(), ec);
            if (!fs::exists(DotNetFixture::userPlugins()))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const fs::path src = ofs::util::getBasePath() / "plugins" / "Ofs.ProbePlugin";
        const fs::path dst = DotNetFixture::userPlugins() / "Ofs.ProbePlugin";
        if (!fs::exists(src))
            return false;
        fs::create_directories(dst.parent_path(), ec);
        fs::copy(src, dst, fs::copy_options::recursive, ec);
        return true;
    }
};
} // namespace

TEST_CASE("Plugin TState node: JSON round-trips through the codec to eval") {
    NodeStateFixture fx;
    REQUIRE_DOTNET(fx);

    // A float + enum (by name) decode and reach eval: 50 + 7 = 57.
    CHECK(fx.evalWith(R"({"Offset":7,"Mode":"Add"})") == 57);
    // Enum persisted by member NAME, so "Sub" maps correctly regardless of numeric ordering: 50 - 4 = 46.
    CHECK(fx.evalWith(R"({"Offset":4,"Mode":"Sub"})") == 46);
}

TEST_CASE("Plugin TState node: schema drift degrades per field (best-effort decode)") {
    NodeStateFixture fx;
    REQUIRE_DOTNET(fx);

    // Missing field keeps its default: no Mode → Add; 50 + 5 = 55.
    CHECK(fx.evalWith(R"({"Offset":5})") == 55);
    // Empty object → all defaults (Offset 0, Add): 50 + 0 = 50.
    CHECK(fx.evalWith("{}") == 50);
    // A type change of a kept field name can't be salvaged → whole-node default (Offset 0): 50.
    CHECK(fx.evalWith(R"({"Offset":"not-a-number"})") == 50);
    // An unknown/extra property is ignored, the real field still decodes: 50 + 8 = 58.
    CHECK(fx.evalWith(R"({"Offset":8,"Bogus":123})") == 58);
}

TEST_CASE("Plugin TState node: every field round-trips into eval") {
    NodeStateFixture fx;
    REQUIRE_DOTNET(fx);

    // Scratch is a plain field — there is no per-field persistence opt-out, so it round-trips
    // through nodeState and the worker reads it like any other field: 50 + Offset(2) + Scratch(30) = 82.
    // (Sub-100 sum so the 0..100 output clamp doesn't mask the result.)
    CHECK(fx.evalWith(R"({"Offset":2,"Scratch":30})") == 82);
}

TEST_CASE("Plugin TState factory node: prepare runs once per region eval") {
    NodeStateFixture fx;
    REQUIRE_DOTNET(fx);
    REQUIRE(fx.effectReg.pluginNodes.count("Ofs.ProbePlugin.prepmod") == 1);

    // Three input actions → three output samples in ONE eval. The factory bakes a process-wide build
    // counter into its closure (output = Base + buildCount) and returns it for every sample. All three
    // outputs being EQUAL proves the factory built the closure once and reused it — not once per sample
    // (which would make each output differ by the increment).
    const std::initializer_list<ofs::ScriptAxisAction> input = {{1.0, 0}, {2.0, 0}, {3.0, 0}};
    auto first = fx.evalAll("Ofs.ProbePlugin.prepmod", R"({"Base":10})", input);
    REQUIRE(first.size() == 3);
    CHECK(first[0] == first[1]);
    CHECK(first[1] == first[2]);

    // A second eval re-runs the factory exactly once more, so its (uniform) output is one higher than the
    // first eval's — proving the closure is rebuilt per eval, not leaked across evals nor rebuilt per sample.
    auto second = fx.evalAll("Ofs.ProbePlugin.prepmod", R"({"Base":10})", input);
    REQUIRE(second.size() == 3);
    CHECK(second[0] == second[2]);
    CHECK(second[0] - first[0] == 1);
}

TEST_CASE("Plugin TState node with a custom ui callback still captures and evals its state") {
    NodeStateFixture fx;
    REQUIRE_DOTNET(fx);
    REQUIRE(fx.effectReg.pluginNodes.count("Ofs.ProbePlugin.custombump") == 1);
    // The node's onNodeUi hook is set by its custom `ui` callback.
    CHECK(fx.effectReg.pluginNodes.at("Ofs.ProbePlugin.custombump").onNodeUi != nullptr);

    // The ui callback never runs here (no ImGui), but the registration must still capture and decode the
    // node's JSON state on the worker path: output = input + Bumps. Bumps is a plain field.
    const std::initializer_list<ofs::ScriptAxisAction> input = {{1.0, 50}};
    auto out = fx.evalAll("Ofs.ProbePlugin.custombump", R"({"Bumps":7})", input);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == 57);

    // Empty state → default Bumps 0: passthrough.
    auto def = fx.evalAll("Ofs.ProbePlugin.custombump", "{}", input);
    REQUIRE(def.size() == 1);
    CHECK(def[0] == 50);
}

// ── Node-shape coverage: params read, discrete input read, and the combiner (two-input) shapes ──

TEST_CASE("Plugin functional modifier reads NodeContext.Params") {
    NodeStateFixture fx;
    REQUIRE_DOTNET(fx);
    REQUIRE(fx.effectReg.pluginNodes.count("Ofs.ProbePlugin.paramprobe") == 1);

    // The node returns input + ctx.Params.Length + ctx.Param(0). Plugin nodes carry no scalar params, so
    // both addends are 0 — a passthrough — but the read exercises the Params span + Param accessor.
    auto out = fx.evalAll("Ofs.ProbePlugin.paramprobe", "{}", {{1.0, 50}});
    REQUIRE(out.size() == 1);
    CHECK(out[0] == 50);
}

TEST_CASE("Plugin discrete modifier reads its input via Count + indexer and writes Add(at,pos)") {
    NodeStateFixture fx;
    REQUIRE_DOTNET(fx);
    REQUIRE(fx.effectReg.pluginNodes.count("Ofs.ProbePlugin.discmodcount") == 1);

    // Copies each input action through DiscreteReader.Count / the indexer to DiscreteWriter.Add(at,pos).
    auto out = fx.evalAll("Ofs.ProbePlugin.discmodcount", "{}", {{1.0, 30}, {2.0, 70}});
    REQUIRE(out.size() == 2);
    CHECK(out[0] == 30);
    CHECK(out[1] == 70);
}

TEST_CASE("Plugin functional combiner evaluates through the two-input path") {
    NodeStateFixture fx;
    REQUIRE_DOTNET(fx);
    REQUIRE(fx.effectReg.pluginNodes.count("Ofs.ProbePlugin.combo") == 1);

    // (a + b) / 2: a is L0's 50; b is R0's functional sample, which reads 0 at L0's grid for a lone
    // discrete action — so the combiner resolves to 25, proving StateCombTrampoline ran with both inputs.
    auto out = fx.evalComb("Ofs.ProbePlugin.combo", {{1.0, 50}});
    REQUIRE_FALSE(out.empty());
    CHECK(out[0] == 25);
}

TEST_CASE("Plugin discrete combiner evaluates through the two-input discrete path") {
    NodeStateFixture fx;
    REQUIRE_DOTNET(fx);
    REQUIRE(fx.effectReg.pluginNodes.count("Ofs.ProbePlugin.disccomb") == 1);

    // Merges both inputs' actions; identical {1.0,50} grids dedupe to a single output point at pos 50.
    auto out = fx.evalComb("Ofs.ProbePlugin.disccomb", {{1.0, 50}});
    REQUIRE_FALSE(out.empty());
    CHECK(out[0] == 50);
}

// ── Cooperative cancellation: a discrete eval observes ctx.IsCancelled (real CoreCLR) ──
// Drives Ofs.ProbePlugin's "cancelprobe" discrete modifier, whose eval blocks on the worker and polls
// NodeContext.IsCancelled. The test starts the eval, waits until it is running, cancels the in-flight
// job directly (exactly what a superseding edit does), and confirms the plugin noticed — proving the
// cancel token reaches the managed discrete callback through OfsEvalCtx.

TEST_CASE("Plugin discrete eval observes cooperative cancellation via ctx.IsCancelled") {
    NodeStateFixture fx;
    REQUIRE_DOTNET(fx);
    REQUIRE(fx.effectReg.pluginNodes.count("Ofs.ProbePlugin.cancelprobe") == 1);

    auto &proj = fx.tp.project;

    // Region graph: Input → cancelprobe (discrete modifier) → Output on L0.
    proj.regions.clear();
    ProcessingNodeGraph g;
    const int inId = g.allocId();
    const int nodeId = g.allocId();
    const int outId = g.allocId();
    g.nodes.push_back({.id = inId, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    ProcessingGraphNode pn;
    pn.id = nodeId;
    pn.type = GraphNodeType::PluginNode;
    pn.pluginNodeId = "Ofs.ProbePlugin.cancelprobe";
    pn.nodeState = "{}";
    pn.role = StandardAxis::L0;
    g.nodes.push_back(pn);
    g.nodes.push_back({.id = outId, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = inId, .toNode = nodeId, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = nodeId, .toNode = outId, .toPin = 0});

    ProcessingRegion r;
    r.id = 1;
    r.startTime = 0.0;
    r.endTime = 10.0;
    r.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    r.nodeGraph = g;
    proj.regions = {r};
    proj.sortRegions();

    // Fire the report command and return the latest value on a counter channel (0 if never seen). The
    // counters are monotonic, so the test baselines them and waits for a strict increase — robust to a
    // managed ALC that persists statics across fixtures in the same process.
    auto report = [&](double base) -> double {
        fx.pm.firePluginCommand("Ofs.ProbePlugin", "cancelreport");
        fx.tp.eq.drain();
        return channelArg(fx.seeks.received, base).value_or(0.0);
    };

    const double startedBefore = report(kCancelStartedBase);
    const double observedBefore = report(kCancelObservedBase);

    // mutate → submit a job; the cancelprobe eval blocks on a worker and starts polling IsCancelled.
    auto &axis = proj.axes[static_cast<size_t>(StandardAxis::L0)];
    proj.mutate(
        StandardAxis::L0,
        [](AxisState &a) {
            a.actions.clear();
            a.actions.insert({1.0, 50});
        },
        fx.tp.eq);
    fx.tp.eq.drain();

    // Wait until the eval entered its loop, so cancelling lands while it is genuinely running — past the
    // worker's own between-regions cancel guard, which would otherwise skip the callback entirely.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (report(kCancelStartedBase) <= startedBefore && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(report(kCancelStartedBase) > startedBefore);
    REQUIRE(axis.pendingEval != nullptr);

    // Cancel the in-flight job directly — exactly what a superseding edit does under the hood.
    axis.pendingEval->cancel();

    // The eval should see ctx.IsCancelled and bail within an iteration, bumping the observed counter.
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (report(kCancelObservedBase) <= observedBefore && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(report(kCancelObservedBase) > observedBefore);
}

// ── Interaction extension points: a plugin edit mode + navigator drive editing/stepping ──
// The probe registers an edit mode ("intentmode") and a navigator ("fixedstep") at OnLoad. These tests
// activate each (the footer-selector path), push the request events a real gesture would, and confirm
// the router consulted the plugin: the mode's Replace/Drop reshape the resolved mutation, and the
// navigator's Seek/None decide the resolved seek.

TEST_CASE("Plugin edit mode resolves intents through the router (Replace / Drop / Pass)") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // OnLoad → registerEditMode → RegisterEditModeEvent → the router published it in the registry.
    REQUIRE(fx.editModeReg.find("Ofs.ProbePlugin.intentmode") != nullptr);

    // The current value of an intentlifecycle counter channel (onEnter / onExit runs are monotonic).
    auto lifecycle = [&](double base) -> double {
        fx.pm.firePluginCommand("Ofs.ProbePlugin", "intentlifecycle");
        fx.tp.eq.drain();
        return channelArg(fx.seeks.received, base).value_or(0.0);
    };
    const double entersBefore = lifecycle(kModeEnterBase);

    // Activate the plugin mode (only the footer selector writes activeEditMode); onEnter fires.
    fx.send(SetActiveEditModeEvent{.id = "Ofs.ProbePlugin.intentmode"});
    CHECK(fx.tp.project.activeEditMode == "Ofs.ProbePlugin.intentmode");
    CHECK(lifecycle(kModeEnterBase) > entersBefore);

    // AddPoint → the mode returns Replace with the position shifted +1, so the router emits a single
    // AddActionAtTimeEvent carrying the transformed pos (51), not the original (50).
    fx.send(EditRequestEvent{
        .intent = EditIntent{.kind = EditIntentKind::AddPoint, .axis = StandardAxis::L0, .time = 1.0, .pos = 50}});
    REQUIRE(fx.adds.received.size() == 1);
    CHECK(fx.adds.received[0].axis == StandardAxis::L0);
    CHECK(fx.adds.received[0].time == doctest::Approx(1.0));
    CHECK(fx.adds.received[0].pos == 51);

    // RemovePoint → the mode returns Drop, so nothing is applied (no RemoveActionAtTimeEvent emitted).
    fx.send(EditRequestEvent{
        .intent = EditIntent{.kind = EditIntentKind::RemovePoint, .axis = StandardAxis::L0, .time = 1.0}});
    CHECK(fx.removes.received.empty());

    // Switching back to native runs the mode's onExit.
    const double exitsBefore = lifecycle(kModeExitBase);
    fx.send(SetActiveEditModeEvent{.id = kNativeEditModeId});
    CHECK(lifecycle(kModeExitBase) > exitsBefore);

    // With native active, the same AddPoint resolves unchanged (pos 50, no +1 transform).
    fx.send(EditRequestEvent{
        .intent = EditIntent{.kind = EditIntentKind::AddPoint, .axis = StandardAxis::L0, .time = 2.0, .pos = 50}});
    REQUIRE(fx.adds.received.size() == 2);
    CHECK(fx.adds.received[1].pos == 50);
}

TEST_CASE("A Replace emitting several intents for one axis applies them all, coalesced into one undo step") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    REQUIRE(fx.editModeReg.find("Ofs.ProbePlugin.multimode") != nullptr);
    fx.send(SetActiveEditModeEvent{.id = "Ofs.ProbePlugin.multimode"});

    // One AddPoint gesture. The mode returns Replace with three AddPoints on the same axis; the router
    // resolves each natively, in submission order, into its own AddActionAtTimeEvent — no dedup or merge.
    fx.send(EditRequestEvent{
        .intent = EditIntent{.kind = EditIntentKind::AddPoint, .axis = StandardAxis::L0, .time = 1.0, .pos = 50}});

    // All three emitted intents reach the host, verbatim and in order (none silently dropped or collapsed).
    REQUIRE(fx.adds.received.size() == 3);
    CHECK(fx.adds.received[0].time == doctest::Approx(1.0));
    CHECK(fx.adds.received[0].pos == 10);
    CHECK(fx.adds.received[1].time == doctest::Approx(2.0));
    CHECK(fx.adds.received[1].pos == 20);
    CHECK(fx.adds.received[2].time == doctest::Approx(3.0));
    CHECK(fx.adds.received[2].pos == 30);
    for (const auto &a : fx.adds.received)
        CHECK(a.axis == StandardAxis::L0);

    // The undo-coalescing contract the router host-stamps: the first mutation of the gesture opens the undo
    // step (snapshot=true), every later one folds into it (snapshot=false). This is what makes a single
    // Replace — however many same-axis intents it emits — undo as one step.
    CHECK(fx.adds.received[0].snapshot == true);
    CHECK(fx.adds.received[1].snapshot == false);
    CHECK(fx.adds.received[2].snapshot == false);
}

TEST_CASE("Plugin edit mode ReplacePerAxis re-consults the mode per follower axis through the router") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // Group L0 (active lead) + R0 so the per-axis fan-out has a follower to re-consult.
    fx.tp.project.state.activeAxis = StandardAxis::L0;
    fx.tp.project.state.axesGrouping.set(static_cast<size_t>(StandardAxis::L0));
    fx.tp.project.state.axesGrouping.set(static_cast<size_t>(StandardAxis::R0));

    fx.send(SetActiveEditModeEvent{.id = "Ofs.ProbePlugin.peraxismode"});
    REQUIRE(fx.tp.project.activeEditMode == "Ofs.ProbePlugin.peraxismode");

    // One AddPoint at the lead. ReplacePerAxis emits one AddActionAtTimeEvent per editable axis, each value
    // computed from its own axis: L0→80 (lead), R0→20 (follower re-consulted). A projecting Replace would
    // instead fan the lead's 80 to R0. Both are single-axis (fanToGroup=false) — the router fanned above
    // the seam, so ProjectManager must not re-project.
    fx.send(EditRequestEvent{
        .intent = EditIntent{.kind = EditIntentKind::AddPoint, .axis = StandardAxis::L0, .time = 1.0, .pos = 50}});
    REQUIRE(fx.adds.received.size() == 2);
    const auto byAxis = [&](StandardAxis ax) -> const AddActionAtTimeEvent * {
        for (const auto &e : fx.adds.received)
            if (e.axis == ax)
                return &e;
        return nullptr;
    };
    const AddActionAtTimeEvent *l0 = byAxis(StandardAxis::L0);
    const AddActionAtTimeEvent *r0 = byAxis(StandardAxis::R0);
    REQUIRE(l0);
    REQUIRE(r0);
    CHECK(l0->pos == 80); // lead's own per-axis result
    CHECK(r0->pos == 20); // follower re-consulted → its own result, not the projected 80
    CHECK_FALSE(l0->fanToGroup);
    CHECK_FALSE(r0->fanToGroup);
}

TEST_CASE("Plugin navigator resolves a step through the router (Seek / None / Pass)") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    REQUIRE(fx.navReg.find("Ofs.ProbePlugin.fixedstep") != nullptr);

    // The current value of an intentlifecycle counter channel (onEnter / onExit runs are monotonic).
    auto lifecycle = [&](double base) -> double {
        fx.pm.firePluginCommand("Ofs.ProbePlugin", "intentlifecycle");
        fx.tp.eq.drain();
        return channelArg(fx.seeks.received, base).value_or(0.0);
    };
    const double entersBefore = lifecycle(kNavEnterBase);

    fx.send(SetActiveNavigatorEvent{.id = "Ofs.ProbePlugin.fixedstep"});
    CHECK(fx.tp.project.activeNavigator == "Ofs.ProbePlugin.fixedstep");
    CHECK(lifecycle(kNavEnterBase) > entersBefore); // activation ran the navigator's onEnter

    // Frame granularity: the navigator redefines it — next→42s, prev→7s, regardless of overlay. A plain
    // Nav.Seek (no axis) must not touch the active axis (the result axis defaults to the Count sentinel).
    const int axisSelBefore = static_cast<int>(fx.activeSelections.received.size());
    fx.send(StepRequestEvent{.direction = StepDirection::Forward, .reps = 1});
    CHECK(sawSeek(fx.seeks.received, 42.0));
    fx.send(StepRequestEvent{.direction = StepDirection::Backward, .reps = 1});
    CHECK(sawSeek(fx.seeks.received, 7.0));
    CHECK(static_cast<int>(fx.activeSelections.received.size()) == axisSelBefore);

    // Action granularity: the navigator returns Nav.Pass(), so the router falls back to the native
    // action resolution. Seed an action ahead of the cursor; the step lands on it (12.5s), not 42s.
    fx.tp.project.state.activeAxis = StandardAxis::L0;
    fx.tp.project.axes[static_cast<size_t>(StandardAxis::L0)].actions.insert(ScriptAxisAction{12.5, 40});
    fx.tp.project.playback.cursorPos = 0.0;
    const int seeksBefore = static_cast<int>(fx.seeks.received.size());
    fx.send(StepRequestEvent{.direction = StepDirection::Forward, .granularity = StepGranularity::Action});
    REQUIRE(static_cast<int>(fx.seeks.received.size()) > seeksBefore);
    CHECK(sawSeek(fx.seeks.received, 12.5)); // native action target, not the navigator's fixed 42s

    // Switching back to follow-overlay runs the navigator's onExit.
    const double exitsBefore = lifecycle(kNavExitBase);
    fx.send(SetActiveNavigatorEvent{.id = kFollowOverlayNavigatorId});
    CHECK(lifecycle(kNavExitBase) > exitsBefore);
}

TEST_CASE("Plugin navigator can seek and activate an axis (Nav.Seek with an explicit axis)") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    REQUIRE(fx.navReg.find("Ofs.ProbePlugin.fixedstep") != nullptr);
    fx.send(SetActiveNavigatorEvent{.id = "Ofs.ProbePlugin.fixedstep"});
    fx.tp.project.state.activeAxis = StandardAxis::L0; // so R0 is a genuine switch, not a no-op

    // ActionAllAxes granularity: the navigator returns Nav.Seek(55.0, R0) — the host must both seek and
    // make R0 active, reproducing the native multi-axis-step behavior a time-only Seek can't express.
    const int axisSelBefore = static_cast<int>(fx.activeSelections.received.size());
    fx.send(StepRequestEvent{.direction = StepDirection::Forward, .granularity = StepGranularity::ActionAllAxes});
    CHECK(sawSeek(fx.seeks.received, 55.0));
    REQUIRE(static_cast<int>(fx.activeSelections.received.size()) > axisSelBefore);
    CHECK(fx.activeSelections.received.back().role == StandardAxis::R0);
}

TEST_CASE("Plugin selection mode resolves a gesture per-axis through the router (Replace / Drop / Pass)") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // OnLoad → registerSelectionMode → RegisterSelectionModeEvent → the router published it.
    REQUIRE(fx.selReg.find("Ofs.ProbePlugin.probeselect") != nullptr);

    // The current value of an intentlifecycle counter channel (onEnter / onExit runs are monotonic).
    auto lifecycle = [&](double base) -> double {
        fx.pm.firePluginCommand("Ofs.ProbePlugin", "intentlifecycle");
        fx.tp.eq.drain();
        return channelArg(fx.seeks.received, base).value_or(0.0);
    };
    const double entersBefore = lifecycle(kSelEnterBase);

    // Seed L0 with a low and two high points; activate the plugin mode (only the footer writes the id).
    auto &l0 = fx.tp.project.axes[static_cast<size_t>(StandardAxis::L0)];
    l0.actions.insert({1.0, 40});
    l0.actions.insert({2.0, 60});
    l0.actions.insert({3.0, 80});
    fx.tp.project.state.activeAxis = StandardAxis::L0;
    fx.send(SetActiveSelectionModeEvent{.id = "Ofs.ProbePlugin.probeselect"});
    CHECK(fx.tp.project.activeSelectionMode == "Ofs.ProbePlugin.probeselect");
    CHECK(lifecycle(kSelEnterBase) > entersBefore); // activation ran the mode's onEnter

    // Box over all three → the mode Replaces with only the pos>=50 points (2.0, 3.0), dropping the 40.
    // Native would have selected all three, so this proves the plugin reshaped the selection.
    fx.send(ofs::SelectRequestEvent{
        .gesture = ofs::SelectGesture::Box, .axis = StandardAxis::L0, .startTime = 0.5, .endTime = 3.5});
    REQUIRE(l0.selection.size() == 2);
    CHECK(l0.selection.contains(ScriptAxisAction{2.0, 60}));
    CHECK(l0.selection.contains(ScriptAxisAction{3.0, 80}));

    // All → the mode returns Drop, so nothing is selected (native would select every action).
    fx.send(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::All, .axis = StandardAxis::L0});
    CHECK(l0.selection.empty());

    // Switch back to native: this runs the mode's onExit, and the same Box now selects every in-range
    // action (no pos filter).
    const double exitsBefore = lifecycle(kSelExitBase);
    fx.send(SetActiveSelectionModeEvent{.id = kNativeSelectionModeId});
    CHECK(lifecycle(kSelExitBase) > exitsBefore);
    fx.send(ofs::SelectRequestEvent{
        .gesture = ofs::SelectGesture::Box, .axis = StandardAxis::L0, .startTime = 0.5, .endTime = 3.5});
    CHECK(l0.selection.size() == 3);
}

TEST_CASE("Editing / Navigation / Selection IsActive each track only their own active selection") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // The probe's "intentactive" command seeks back 1/0 on three channels — Editing/Navigation/Selection
    // IsActive for its own mode ids. Fire once, then read each channel's latest value.
    auto fire = [&] {
        fx.pm.firePluginCommand("Ofs.ProbePlugin", "intentactive");
        fx.tp.eq.drain();
    };

    // Nothing activated yet → every query is inactive (defaults are native / follow-overlay).
    fire();
    CHECK(channelArg(fx.seeks.received, kEditActiveBase) == doctest::Approx(0.0));
    CHECK(channelArg(fx.seeks.received, kNavActiveBase) == doctest::Approx(0.0));
    CHECK(channelArg(fx.seeks.received, kSelActiveBase) == doctest::Approx(0.0));

    // Activating the edit mode flips only Editing.IsActive — the three seams are independent.
    fx.send(SetActiveEditModeEvent{.id = "Ofs.ProbePlugin.intentmode"});
    fire();
    CHECK(channelArg(fx.seeks.received, kEditActiveBase) == doctest::Approx(1.0));
    CHECK(channelArg(fx.seeks.received, kNavActiveBase) == doctest::Approx(0.0));
    CHECK(channelArg(fx.seeks.received, kSelActiveBase) == doctest::Approx(0.0));

    // Activating the navigator flips only Navigation.IsActive (edit stays active — independent settings).
    fx.send(SetActiveNavigatorEvent{.id = "Ofs.ProbePlugin.fixedstep"});
    fire();
    CHECK(channelArg(fx.seeks.received, kEditActiveBase) == doctest::Approx(1.0));
    CHECK(channelArg(fx.seeks.received, kNavActiveBase) == doctest::Approx(1.0));
    CHECK(channelArg(fx.seeks.received, kSelActiveBase) == doctest::Approx(0.0));

    // Activating the selection mode flips only Selection.IsActive.
    fx.send(SetActiveSelectionModeEvent{.id = "Ofs.ProbePlugin.probeselect"});
    fire();
    CHECK(channelArg(fx.seeks.received, kEditActiveBase) == doctest::Approx(1.0));
    CHECK(channelArg(fx.seeks.received, kNavActiveBase) == doctest::Approx(1.0));
    CHECK(channelArg(fx.seeks.received, kSelActiveBase) == doctest::Approx(1.0));

    // Switching the edit mode back to native clears only Editing.IsActive; the other two stay active.
    fx.send(SetActiveEditModeEvent{.id = kNativeEditModeId});
    fire();
    CHECK(channelArg(fx.seeks.received, kEditActiveBase) == doctest::Approx(0.0));
    CHECK(channelArg(fx.seeks.received, kNavActiveBase) == doctest::Approx(1.0));
    CHECK(channelArg(fx.seeks.received, kSelActiveBase) == doctest::Approx(1.0));
}

TEST_CASE("Unloading a plugin drops its edit mode / navigator / selection mode and falls back to native dispatch") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    fx.send(SetActiveEditModeEvent{.id = "Ofs.ProbePlugin.intentmode"});
    fx.send(SetActiveNavigatorEvent{.id = "Ofs.ProbePlugin.fixedstep"});
    fx.send(SetActiveSelectionModeEvent{.id = "Ofs.ProbePlugin.probeselect"});

    fx.send(SetPluginEnabledEvent{.name = "Ofs.ProbePlugin", .enabled = false});

    // The unregister events dropped the plugin's entries from every registry.
    CHECK(fx.editModeReg.find("Ofs.ProbePlugin.intentmode") == nullptr);
    CHECK(fx.navReg.find("Ofs.ProbePlugin.fixedstep") == nullptr);
    CHECK(fx.selReg.find("Ofs.ProbePlugin.probeselect") == nullptr);

    // Phase 5: the effective ids were rewritten back to native / follow-overlay (no dangling selection),
    // while the stored (authored) ids are preserved so a re-save keeps the original selection on disk.
    CHECK(fx.tp.project.activeEditMode == kNativeEditModeId);
    CHECK(fx.tp.project.activeNavigator == kFollowOverlayNavigatorId);
    CHECK(fx.tp.project.activeSelectionMode == kNativeSelectionModeId);
    CHECK(fx.tp.project.storedEditMode == "Ofs.ProbePlugin.intentmode");
    CHECK(fx.tp.project.storedNavigator == "Ofs.ProbePlugin.fixedstep");
    CHECK(fx.tp.project.storedSelectionMode == "Ofs.ProbePlugin.probeselect");

    // Dispatch now resolves natively: AddPoint resolves unchanged (pos 50), and a step resolves through
    // the built-in follow-overlay (a SeekEvent, not the plugin's 42s).
    fx.send(EditRequestEvent{
        .intent = EditIntent{.kind = EditIntentKind::AddPoint, .axis = StandardAxis::L0, .time = 1.0, .pos = 50}});
    REQUIRE(fx.adds.received.size() == 1);
    CHECK(fx.adds.received[0].pos == 50);

    const int seeksBefore = static_cast<int>(fx.seeks.received.size());
    fx.send(StepRequestEvent{.direction = StepDirection::Forward, .reps = 1});
    CHECK(static_cast<int>(fx.seeks.received.size()) > seeksBefore); // follow-overlay still produced a seek
    CHECK_FALSE(sawSeek(fx.seeks.received, 42.0));                   // but not the plugin's fixed target

    // Selection likewise resolves natively: a Box now selects every in-range action, proving the active
    // selection mode fell back too — the plugin's pos>=50 filter (probeselect) is gone, so the low point
    // is no longer dropped.
    auto &l0 = fx.tp.project.axes[static_cast<size_t>(StandardAxis::L0)];
    l0.actions.insert({1.0, 10});
    l0.actions.insert({2.0, 80});
    fx.tp.project.state.activeAxis = StandardAxis::L0;
    fx.send(ofs::SelectRequestEvent{
        .gesture = ofs::SelectGesture::Box, .axis = StandardAxis::L0, .startTime = 0.5, .endTime = 2.5});
    CHECK(l0.selection.size() == 2); // native kept both; the plugin mode would have dropped the pos-10 point
}

// ── Phase 5: lifecycle / dangling-selection fallback ─────────────────────────
// The active selection is a weak reference held by id. When the owning plugin departs (disable, unload,
// reload, crash) or a loaded project names an absent plugin, the *effective* id falls back to
// native / follow-overlay while the *stored* (authored) id is preserved for a re-save.

TEST_CASE("Unloading the active-selection plugin runs onExit best-effort and falls back, preserving stored") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    auto lifecycle = [&](double base) -> double {
        fx.pm.firePluginCommand("Ofs.ProbePlugin", "intentlifecycle");
        fx.tp.eq.drain();
        return channelArg(fx.seeks.received, base).value_or(0.0);
    };

    fx.send(SetActiveEditModeEvent{.id = "Ofs.ProbePlugin.intentmode"});
    fx.send(SetActiveNavigatorEvent{.id = "Ofs.ProbePlugin.fixedstep"});
    fx.send(SetActiveSelectionModeEvent{.id = "Ofs.ProbePlugin.probeselect"});
    const double modeExitsBefore = lifecycle(kModeExitBase);
    const double navExitsBefore = lifecycle(kNavExitBase);
    const double selExitsBefore = lifecycle(kSelExitBase);

    // A direct unregister keeps the plugin's managed slots live (unlike the real disable path, which
    // releases them before the event drains), so the router's best-effort onExit actually reaches the
    // handler — exercising the onExit-on-fallback contract for all three seams.
    fx.send(UnregisterEditModesEvent{"Ofs.ProbePlugin"});
    fx.send(UnregisterNavigatorsEvent{"Ofs.ProbePlugin"});
    fx.send(UnregisterSelectionModesEvent{"Ofs.ProbePlugin"});

    CHECK(lifecycle(kModeExitBase) > modeExitsBefore); // each seam's onExit ran best-effort
    CHECK(lifecycle(kNavExitBase) > navExitsBefore);
    CHECK(lifecycle(kSelExitBase) > selExitsBefore);
    CHECK(fx.tp.project.activeEditMode == kNativeEditModeId); // effective fell back
    CHECK(fx.tp.project.activeNavigator == kFollowOverlayNavigatorId);
    CHECK(fx.tp.project.activeSelectionMode == kNativeSelectionModeId);
    CHECK(fx.tp.project.storedEditMode == "Ofs.ProbePlugin.intentmode"); // authored id preserved
    CHECK(fx.tp.project.storedNavigator == "Ofs.ProbePlugin.fixedstep");
    CHECK(fx.tp.project.storedSelectionMode == "Ofs.ProbePlugin.probeselect");
}

TEST_CASE("A loaded project's dangling selection falls back without clobbering the stored id, and restores") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    // Mimic loadFromProject seeding the authored ids from a file whose plugin isn't loaded here: stored
    // and effective start equal, then LoadProjectEvent re-derives the effective id from the stored one.
    fx.tp.project.storedEditMode = "ghost.editmode";
    fx.tp.project.activeEditMode = "ghost.editmode";
    fx.tp.project.storedNavigator = "ghost.navigator";
    fx.tp.project.activeNavigator = "ghost.navigator";
    fx.tp.project.storedSelectionMode = "ghost.selectmode";
    fx.tp.project.activeSelectionMode = "ghost.selectmode";
    fx.send(LoadProjectEvent{});

    // Effective ids fall back so editing/stepping/selection always resolve; stored ids are preserved for
    // a re-save.
    CHECK(fx.tp.project.activeEditMode == kNativeEditModeId);
    CHECK(fx.tp.project.activeNavigator == kFollowOverlayNavigatorId);
    CHECK(fx.tp.project.activeSelectionMode == kNativeSelectionModeId);
    CHECK(fx.tp.project.storedEditMode == "ghost.editmode");
    CHECK(fx.tp.project.storedNavigator == "ghost.navigator");
    CHECK(fx.tp.project.storedSelectionMode == "ghost.selectmode");

    // Loading a project that names a *present* plugin's mode/navigator/selection restores the effective
    // selection from the stored authored id (the normal restore-on-open path).
    fx.tp.project.storedEditMode = "Ofs.ProbePlugin.intentmode";
    fx.tp.project.activeEditMode = kNativeEditModeId; // pretend it was previously fallen back
    fx.tp.project.storedNavigator = "Ofs.ProbePlugin.fixedstep";
    fx.tp.project.activeNavigator = kFollowOverlayNavigatorId;
    fx.tp.project.storedSelectionMode = "Ofs.ProbePlugin.probeselect";
    fx.tp.project.activeSelectionMode = kNativeSelectionModeId;
    fx.send(LoadProjectEvent{});
    CHECK(fx.tp.project.activeEditMode == "Ofs.ProbePlugin.intentmode");
    CHECK(fx.tp.project.activeNavigator == "Ofs.ProbePlugin.fixedstep");
    CHECK(fx.tp.project.activeSelectionMode == "Ofs.ProbePlugin.probeselect");
}

TEST_CASE("Re-registering a fallen-back mode / navigator / selection mode does not silently re-activate it") {
    DotNetFixture fx{"Ofs.ProbePlugin"};
    REQUIRE_DOTNET(fx);

    fx.send(SetActiveEditModeEvent{.id = "Ofs.ProbePlugin.intentmode"});
    fx.send(SetActiveNavigatorEvent{.id = "Ofs.ProbePlugin.fixedstep"});
    fx.send(SetActiveSelectionModeEvent{.id = "Ofs.ProbePlugin.probeselect"});

    fx.send(UnregisterEditModesEvent{"Ofs.ProbePlugin"});
    fx.send(UnregisterNavigatorsEvent{"Ofs.ProbePlugin"});
    fx.send(UnregisterSelectionModesEvent{"Ofs.ProbePlugin"});
    REQUIRE(fx.tp.project.activeEditMode == kNativeEditModeId);
    REQUIRE(fx.tp.project.activeNavigator == kFollowOverlayNavigatorId);
    REQUIRE(fx.tp.project.activeSelectionMode == kNativeSelectionModeId);
    REQUIRE(fx.tp.project.storedEditMode == "Ofs.ProbePlugin.intentmode");
    REQUIRE(fx.tp.project.storedSelectionMode == "Ofs.ProbePlugin.probeselect");

    // A reload re-publishes the registrations, but must NOT re-activate them — the selection stays at the
    // fallback until the user re-picks (a crashing/reloading plugin cannot force itself active).
    EditModeEntry mode;
    mode.id = "Ofs.ProbePlugin.intentmode";
    mode.owningPlugin = "Ofs.ProbePlugin";
    fx.send(RegisterEditModeEvent{mode});
    NavigatorEntry nav;
    nav.id = "Ofs.ProbePlugin.fixedstep";
    nav.owningPlugin = "Ofs.ProbePlugin";
    fx.send(RegisterNavigatorEvent{nav});
    SelectionModeEntry sel;
    sel.id = "Ofs.ProbePlugin.probeselect";
    sel.owningPlugin = "Ofs.ProbePlugin";
    fx.send(RegisterSelectionModeEvent{sel});

    CHECK(fx.editModeReg.find("Ofs.ProbePlugin.intentmode") != nullptr); // re-published
    CHECK(fx.navReg.find("Ofs.ProbePlugin.fixedstep") != nullptr);
    CHECK(fx.selReg.find("Ofs.ProbePlugin.probeselect") != nullptr);
    CHECK(fx.tp.project.activeEditMode == kNativeEditModeId);            // but still on native
    CHECK(fx.tp.project.activeNavigator == kFollowOverlayNavigatorId);   // and follow-overlay
    CHECK(fx.tp.project.activeSelectionMode == kNativeSelectionModeId);  // and native selection
    CHECK(fx.tp.project.storedEditMode == "Ofs.ProbePlugin.intentmode"); // authored id still preserved
    CHECK(fx.tp.project.storedSelectionMode == "Ofs.ProbePlugin.probeselect");
}

// ── ABI version gate (pure function — no plugin involved, runs unconditionally) ──

TEST_CASE("isPluginAbiVersionSupported accepts only 1..OFS_ABI_VERSION") {
    CHECK_FALSE(isPluginAbiVersionSupported(0));  // a failed/zeroed PluginApi fill
    CHECK_FALSE(isPluginAbiVersionSupported(-1)); // garbage
    CHECK(isPluginAbiVersionSupported(1));        // floor
    CHECK(isPluginAbiVersionSupported(OFS_ABI_VERSION));
    CHECK_FALSE(isPluginAbiVersionSupported(OFS_ABI_VERSION + 1)); // newer than this host knows
}
