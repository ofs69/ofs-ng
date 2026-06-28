// HostApi callback tests — no CLR, no plugin.
//
// PluginManager's constructor fully wires the HostApi function-pointer table and the PluginCtx it
// passes as `void* ctx`, independently of init()/CoreCLR. That lets these tests call each host
// callback directly through the exposed hostApi struct and exercise the *guard and edge* branches the
// real-plugin dispatch tests never reach: a call off the main thread, a null/out-of-range role, an
// absent axis, an oversized index, a null buffer. Happy paths are covered too where cheap, so a
// regression in a guard shows up as a behavior change rather than a silent crash.

#include <doctest/doctest.h>

#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/ScriptProject.h"
#include "Localization/Translator.h"
#include "Services/BindingSystem.h"
#include "Services/CommandRegistry.h"
#include "Services/EffectRegistry.h"
#include "Services/PluginApi.h"
#include "Services/PluginManager.h"
#include "Services/PluginNodeIO.h"
#include "helpers/EventCapture.h"
#include "helpers/FakeVideoPlayer.h"
#include "helpers/PluginManagerTestAccess.h"
#include "helpers/TestProject.h"

#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace ofs;
using ofs::test::EventCapture;
using ofs::test::FakeVideoPlayer;
using ofs::test::TestProject;

namespace {

constexpr int kL0 = static_cast<int>(StandardAxis::L0);
constexpr int kR0 = static_cast<int>(StandardAxis::R0);
constexpr int kS0 = static_cast<int>(StandardAxis::S0);

// Builds a PluginManager and exposes its wired hostApi + ctx via the friend test seam. init() is never
// called: the dispatch path needs only what the constructor sets up. Event captures are attached before
// freeze() so a callback that pushes can be observed after a drain().
struct HostApiFixture {
    TestProject tp;
    std::shared_ptr<FakeVideoPlayer> player = std::make_shared<FakeVideoPlayer>();
    std::shared_ptr<FakeVideoPlayer> dummy = std::make_shared<FakeVideoPlayer>();
    CommandRegistry cmdReg{tp.eq};
    RebindState rebind;
    BindingSystem binding{tp.eq, cmdReg, rebind};
    EffectRegistryState effectReg;
    PluginManager pm{tp.project, tp.eq, player, dummy, cmdReg, binding, effectReg};

    EventCapture<SeekEvent> seeks;
    EventCapture<VolumeChangedEvent> volumes;
    EventCapture<PlaybackSpeedEvent> speeds;
    EventCapture<PlayPauseEvent> playPauses;
    EventCapture<CommitAxisActionsEvent> commits;
    EventCapture<SetAxisSelectionEvent> selections;
    EventCapture<AxisSelectedEvent> activeSelections;
    EventCapture<NotifyEvent> notifies;
    EventCapture<SetPluginProjectDataEvent> pluginData;

    HostApiFixture() {
        seeks.attach(tp.eq);
        volumes.attach(tp.eq);
        speeds.attach(tp.eq);
        playPauses.attach(tp.eq);
        commits.attach(tp.eq);
        selections.attach(tp.eq);
        activeSelections.attach(tp.eq);
        notifies.attach(tp.eq);
        pluginData.attach(tp.eq);
        tp.eq.freeze();
        // The dispatch path sets currentPluginName per call; set it once so name-dependent callbacks work.
        PluginManagerTestAccess::pluginCtx(pm).currentPluginName = "TestPlugin";
    }

    const HostApi &h() { return PluginManagerTestAccess::hostApi(pm); }
    void *cv() { return h().ctx; }
    void drain() { tp.eq.drain(); }

    AxisState &axis(int role) { return tp.project.axes[static_cast<size_t>(role)]; }
};

} // namespace

TEST_CASE("HostApi reads reject calls from a non-main thread and return safe fallbacks") {
    HostApiFixture fx;
    fx.axis(kL0).showInStrip = true;
    fx.tp.project.playback.cursorPos = 5.0;
    fx.player->paused = false;

    // Every guarded read, when invoked off the main thread, must hit its checkMainThread fallback
    // rather than touch project state. Capture the fallbacks from a worker and assert them.
    double time = -1, speed = -1, vol = -1, fps = -1;
    int playing = -2, active = -2, vis = -2, locked = -2, roles = -2, dirty = -2;
    std::thread worker([&] {
        time = fx.h().getTime(fx.cv());
        playing = fx.h().isPlaying(fx.cv());
        speed = fx.h().getSpeed(fx.cv());
        vol = fx.h().getVolume(fx.cv());
        fps = fx.h().getFps(fx.cv());
        active = fx.h().getActiveAxisRole(fx.cv());
        vis = fx.h().isAxisVisible(fx.cv(), kL0);
        locked = fx.h().isAxisLocked(fx.cv(), kL0);
        roles = fx.h().getAxisRoles(fx.cv(), nullptr, 0);
        dirty = fx.h().isProjectDirty(fx.cv());
    });
    worker.join();

    CHECK(time == doctest::Approx(0.0));
    CHECK(playing == 0);
    CHECK(speed == doctest::Approx(1.0f));
    CHECK(vol == doctest::Approx(0.0f));
    CHECK(fps == doctest::Approx(0.0));
    CHECK(active == -1);
    CHECK(vis == -1);
    CHECK(locked == -1);
    CHECK(roles == 0);
    CHECK(dirty == 0);
}

TEST_CASE("HostApi player/project reads return live state on the main thread") {
    HostApiFixture fx;
    fx.axis(kL0).showInStrip = true;
    fx.tp.project.playback.cursorPos = 3.25;
    fx.player->paused = false; // playing

    CHECK(fx.h().getTime(fx.cv()) == doctest::Approx(3.25));
    CHECK(fx.h().isPlaying(fx.cv()) == 1);
    CHECK(fx.h().getSpeed(fx.cv()) == doctest::Approx(1.0f));
    CHECK(fx.h().getVolume(fx.cv()) == doctest::Approx(1.0f));
    CHECK(fx.h().getFps(fx.cv()) == doctest::Approx(30.0));

    fx.tp.project.state.activeAxis = StandardAxis::R0;
    CHECK(fx.h().getActiveAxisRole(fx.cv()) == kR0);

    // isProjectDirty mirrors ProjectManager: settings flag or any present+dirty axis.
    CHECK(fx.h().isProjectDirty(fx.cv()) == 0);
    fx.axis(kL0).dirty = true;
    CHECK(fx.h().isProjectDirty(fx.cv()) == 1);
    fx.axis(kL0).dirty = false;
    fx.tp.project.state.settingsDirty = true;
    CHECK(fx.h().isProjectDirty(fx.cv()) == 1);
}

TEST_CASE("getAxisRoles counts present axes and respects the buffer size") {
    HostApiFixture fx;
    fx.axis(kL0).showInStrip = true;
    fx.axis(kR0).showInStrip = true;

    // Count without a buffer.
    CHECK(fx.h().getAxisRoles(fx.cv(), nullptr, 0) == 2);

    // Fill into a buffer smaller than the count: only `bufSize` entries written, full count returned.
    int buf[1] = {-1};
    CHECK(fx.h().getAxisRoles(fx.cv(), buf, 1) == 2);
    CHECK(buf[0] == kL0); // first present role

    int buf2[8];
    CHECK(fx.h().getAxisRoles(fx.cv(), buf2, 8) == 2);
    CHECK(buf2[0] == kL0);
    CHECK(buf2[1] == kR0);
}

TEST_CASE("axis action/selection reads guard on role range and presence") {
    HostApiFixture fx;
    fx.axis(kL0).showInStrip = true;
    fx.tp.project.mutate(
        StandardAxis::L0,
        [](AxisState &a) {
            a.actions.insert({1.0, 40});
            a.actions.insert({2.0, 60});
        },
        fx.tp.eq);
    fx.drain();

    // Out-of-range role → 0.
    CHECK(fx.h().getAxisActionCount(fx.cv(), -1) == 0);
    CHECK(fx.h().getAxisActionCount(fx.cv(), static_cast<int>(kStandardAxisCount)) == 0);
    // Absent axis → 0.
    CHECK(fx.h().getAxisActionCount(fx.cv(), kR0) == 0);
    // Present → real count.
    CHECK(fx.h().getAxisActionCount(fx.cv(), kL0) == 2);

    // Read into a buffer smaller than the count: returns the full count, copies only what fits.
    PluginAction buf[1];
    CHECK(fx.h().getAxisActions(fx.cv(), kL0, buf, 1) == 2);
    CHECK(buf[0].pos == 40);
    // No-buffer call still returns the count.
    CHECK(fx.h().getAxisActions(fx.cv(), kL0, nullptr, 0) == 2);

    // Selection mirrors the action guards; empty by default.
    CHECK(fx.h().getAxisSelectionCount(fx.cv(), kR0) == 0); // absent
    CHECK(fx.h().getAxisSelectionCount(fx.cv(), kL0) == 0); // present, nothing selected
}

TEST_CASE("axis writes push events only for an in-range, present axis") {
    HostApiFixture fx;
    fx.axis(kL0).showInStrip = true;

    const PluginAction acts[2] = {{1.0, 10}, {2.0, 90}};

    // commitAxisActions: out-of-range and absent are dropped; present emits one event.
    fx.h().commitAxisActions(fx.cv(), -1, acts, 2);
    fx.h().commitAxisActions(fx.cv(), kR0, acts, 2); // absent
    fx.drain();
    CHECK(fx.commits.received.empty());

    fx.h().commitAxisActions(fx.cv(), kL0, acts, 2);
    fx.drain();
    REQUIRE(fx.commits.received.size() == 1);
    CHECK(fx.commits.received[0].axis == StandardAxis::L0);
    REQUIRE(fx.commits.received[0].actions.size() == 2);
    CHECK(fx.commits.received[0].actions[0].pos == 10);

    // setAxisSelection + clearAxisSelection both push SetAxisSelectionEvent for a present axis.
    fx.h().setAxisSelection(fx.cv(), kR0, acts, 2); // absent → dropped
    fx.drain();
    CHECK(fx.selections.received.empty());

    fx.h().setAxisSelection(fx.cv(), kL0, acts, 2);
    fx.h().clearAxisSelection(fx.cv(), kL0);
    fx.drain();
    REQUIRE(fx.selections.received.size() == 2);
    CHECK(fx.selections.received[0].selection.size() == 2);
    CHECK(fx.selections.received[1].selection.empty()); // the clear

    // setActiveAxis: absent → dropped; present → AxisSelectedEvent.
    fx.h().setActiveAxis(fx.cv(), kR0); // R0 absent
    fx.drain();
    CHECK(fx.activeSelections.received.empty());
    fx.h().setActiveAxis(fx.cv(), kL0);
    fx.drain();
    REQUIRE(fx.activeSelections.received.size() == 1);
    CHECK(fx.activeSelections.received[0].role == StandardAxis::L0);
}

// Existence is AxisState::exists() (data / locked / shown), NOT showInStrip alone. A standard axis
// hidden from the strip but still holding actions exists, so the host API surfaces it exactly like a
// shown axis — its data is real and reachable. The genuinely-absent axis (no data, no strip) stays
// invisible. This is the divergence case the showInStrip-only gate used to hide; under the old gate
// every check below would have taken the absent-axis fallback.
TEST_CASE("HostApi surfaces a standard axis hidden from the strip but still holding data") {
    HostApiFixture fx;
    const int kR1 = static_cast<int>(StandardAxis::R1);

    // R0: hidden (showInStrip stays false) but populated and locked → exists().
    fx.axis(kR0).isLocked = true;
    fx.axis(kR0).actions.insert({1.0, 30});
    fx.axis(kR0).actions.insert({2.0, 70});
    // R1 left at defaults: no data, not shown → genuinely absent.

    // Enumeration includes the hidden-with-data axis and excludes the empty one.
    CHECK(fx.h().getAxisRoles(fx.cv(), nullptr, 0) == 1);
    int buf[4] = {-1, -1, -1, -1};
    CHECK(fx.h().getAxisRoles(fx.cv(), buf, 4) == 1);
    CHECK(buf[0] == kR0);

    // Reads reach its data/flags instead of the absent-axis fallbacks (0 / -1).
    CHECK(fx.h().getAxisActionCount(fx.cv(), kR0) == 2);
    CHECK(fx.h().isAxisLocked(fx.cv(), kR0) == 1);  // exists → real flag, not -1
    CHECK(fx.h().isAxisVisible(fx.cv(), kR0) == 1); // exists, isVisible defaults true

    // Writes are no longer gated away.
    const PluginAction acts[1] = {{3.0, 55}};
    fx.h().commitAxisActions(fx.cv(), kR0, acts, 1);
    fx.drain();
    REQUIRE(fx.commits.received.size() == 1);
    CHECK(fx.commits.received[0].axis == StandardAxis::R0);

    // The genuinely-absent axis stays absent: reads fall back, writes drop.
    CHECK(fx.h().getAxisActionCount(fx.cv(), kR1) == 0);
    CHECK(fx.h().isAxisLocked(fx.cv(), kR1) == -1);
    fx.h().commitAxisActions(fx.cv(), kR1, acts, 1);
    fx.drain();
    CHECK(fx.commits.received.size() == 1); // unchanged — dropped
}

TEST_CASE("playback writes map to the right events") {
    HostApiFixture fx;

    fx.h().seekTo(fx.cv(), 7.5);
    fx.h().setVolume(fx.cv(), 0.3f);
    fx.h().setSpeed(fx.cv(), 1.75f);
    fx.drain();
    REQUIRE(fx.seeks.received.size() == 1);
    CHECK(fx.seeks.received[0].time == doctest::Approx(7.5));
    REQUIRE(fx.volumes.received.size() == 1);
    CHECK(fx.volumes.received[0].volume == doctest::Approx(0.3f));
    REQUIRE(fx.speeds.received.size() == 1);
    CHECK(fx.speeds.received[0].speed == doctest::Approx(1.75f));

    // setPlaying only toggles when the requested state differs from the player's current state.
    fx.player->paused = true;
    fx.h().setPlaying(fx.cv(), 0); // already paused → no event
    fx.drain();
    CHECK(fx.playPauses.received.empty());
    fx.h().setPlaying(fx.cv(), 1); // paused → want playing → toggle
    fx.drain();
    CHECK(fx.playPauses.received.size() == 1);
}

TEST_CASE("axis metadata reads guard on role range and presence") {
    HostApiFixture fx;
    fx.axis(kL0).showInStrip = true;
    fx.axis(kL0).isVisible = false;
    fx.axis(kL0).isLocked = true;

    CHECK(fx.h().isAxisVisible(fx.cv(), kL0) == 0);  // present, not visible
    CHECK(fx.h().isAxisLocked(fx.cv(), kL0) == 1);   // present, locked
    CHECK(fx.h().isAxisVisible(fx.cv(), kR0) == -1); // absent → -1
    CHECK(fx.h().isAxisLocked(fx.cv(), -1) == -1);   // out of range → -1

    char name[64] = {1};
    fx.h().getAxisName(fx.cv(), -1, name, sizeof(name)); // out of range → empty
    CHECK(name[0] == '\0');
    fx.h().getAxisName(fx.cv(), kL0, name, sizeof(name));
    CHECK(std::string(name).size() > 0);
    // Null buffer / zero size are no-ops (no crash).
    fx.h().getAxisName(fx.cv(), kL0, nullptr, 0);
    fx.h().getAxisName(fx.cv(), kL0, name, 0);
}

TEST_CASE("getActiveLanguage returns the Translator's active ISO 639 code and guards the worker thread") {
    HostApiFixture fx;
    ofs::loc::Translator::instance().loadDefaults(); // built-in English → code "en"

    char buf[16] = {1, 1};
    CHECK(fx.h().getActiveLanguage(fx.cv(), buf, sizeof(buf)) == 2); // "en" is two bytes (excl NUL)
    CHECK(std::string(buf) == "en");

    // Null buffer is a pure length query: no write, required length still reported.
    CHECK(fx.h().getActiveLanguage(fx.cv(), nullptr, 0) == 2);

    // Off the main thread → guarded: buffer cleared, zero length (so managed GrowAndRead reads no stale bytes).
    int len = -1;
    std::thread worker([&] { len = fx.h().getActiveLanguage(fx.cv(), buf, sizeof(buf)); });
    worker.join();
    CHECK(len == 0);
    CHECK(buf[0] == '\0');
}

TEST_CASE("getMediaPath copies the path and guards a null/zero buffer") {
    HostApiFixture fx;
    fx.tp.project.state.mediaPath = "C:/clip.mp4";

    char buf[64] = {1};
    CHECK(fx.h().getMediaPath(fx.cv(), buf, sizeof(buf)) == 11); // returns required length (excl NUL)
    CHECK(std::string(buf) == "C:/clip.mp4");

    // Null buffer / zero size write nothing but still report the required length.
    CHECK(fx.h().getMediaPath(fx.cv(), nullptr, 64) == 11);
    CHECK(fx.h().getMediaPath(fx.cv(), buf, 0) == 11);
}

TEST_CASE("chapter/bookmark/region reads guard on index and report counts") {
    HostApiFixture fx;
    auto &proj = fx.tp.project;
    proj.bookmarks.chapters.push_back({1.0, 5.0, "intro"});
    proj.bookmarks.bookmarks.push_back({2.0, "cue"});
    proj.regions.emplace_back();
    proj.regions[0].startTime = 0.0;
    proj.regions[0].endTime = 9.0;
    proj.regions[0].name = "verse";

    CHECK(fx.h().getChapterCount(fx.cv()) == 1);
    CHECK(fx.h().getBookmarkCount(fx.cv()) == 1);
    CHECK(fx.h().getRegionCount(fx.cv()) == 1);

    double start = 0, end = 0, time = 0;
    unsigned int color = 0;
    char name[64];
    int nameReq = -1;

    // Out-of-range index → 0, no writes; nameReq reset to 0.
    CHECK(fx.h().getChapter(fx.cv(), 5, &start, &end, &color, name, sizeof(name), &nameReq) == 0);
    CHECK(nameReq == 0);
    CHECK(fx.h().getBookmark(fx.cv(), -1, &time, name, sizeof(name), &nameReq) == 0);
    CHECK(fx.h().getRegion(fx.cv(), 9, &start, &end, name, sizeof(name), &nameReq) == 0);

    // Valid index → 1, fields populated, nameReq reports the full name byte length (excl NUL).
    REQUIRE(fx.h().getChapter(fx.cv(), 0, &start, &end, &color, name, sizeof(name), &nameReq) == 1);
    CHECK(start == doctest::Approx(1.0));
    CHECK(end == doctest::Approx(5.0));
    CHECK(std::string(name) == "intro");
    CHECK(nameReq == 5);

    REQUIRE(fx.h().getBookmark(fx.cv(), 0, &time, name, sizeof(name), &nameReq) == 1);
    CHECK(time == doctest::Approx(2.0));
    CHECK(std::string(name) == "cue");

    REQUIRE(fx.h().getRegion(fx.cv(), 0, &start, &end, name, sizeof(name), &nameReq) == 1);
    CHECK(std::string(name) == "verse");

    // Too-small name buffer: still returns 1, truncates within bounds + NUL-terminates, and reports the
    // FULL required length so the caller can grow and re-read (the GrowAndRead contract).
    char tiny[3] = {1, 1, 1};
    REQUIRE(fx.h().getChapter(fx.cv(), 0, &start, &end, &color, tiny, sizeof(tiny), &nameReq) == 1);
    CHECK(std::string(tiny) == "in"); // "intro" clamped to 2 bytes + NUL
    CHECK(nameReq == 5);
    // Null name buffer is a pure length query: no write, required length still reported.
    CHECK(fx.h().getChapter(fx.cv(), 0, &start, &end, &color, nullptr, 0, &nameReq) == 1);
    CHECK(nameReq == 5);

    // Metadata read serializes to JSON; smoke-check it's non-empty and parseable shape.
    proj.metadata.title = "Title";
    char meta[256];
    fx.h().getProjectMetadata(fx.cv(), meta, sizeof(meta));
    CHECK(std::string(meta).find("Title") != std::string::npos);
}

TEST_CASE("getFunscriptJson serializes axes per the requested format version") {
    HostApiFixture fx;
    fx.axis(kL0).showInStrip = true;
    fx.axis(kR0).showInStrip = true;
    fx.tp.project.mutate(
        StandardAxis::L0, [](AxisState &a) { a.actions.insert({1.0, 40}); a.actions.insert({2.0, 60}); }, fx.tp.eq);
    fx.tp.project.mutate(StandardAxis::R0, [](AxisState &a) { a.actions.insert({0.5, 10}); }, fx.tp.eq);
    fx.tp.project.metadata.title = "MyTitle";
    fx.drain();

    const int l0[] = {kL0};
    const int both[] = {kL0, kR0};
    char buf[1024];

    // 1.0 single axis: root actions in ms, project metadata carried, version stamped, no axes/channels.
    REQUIRE(fx.h().getFunscriptJson(fx.cv(), l0, 1, OfsFunscript10, buf, sizeof(buf)) > 0);
    const auto j10 = nlohmann::json::parse(buf);
    CHECK(j10["actions"].size() == 2);
    CHECK(j10["actions"][0]["at"] == 1000); // 1.0 s → 1000 ms
    CHECK(j10["actions"][0]["pos"] == 40);
    CHECK(j10["metadata"]["title"] == "MyTitle");
    CHECK(j10["metadata"]["version"] == "1.0");
    CHECK_FALSE(j10.contains("axes"));
    CHECK_FALSE(j10.contains("channels"));

    // 1.1 multi-axis: L0 stays the root, R0 goes under "axes".
    REQUIRE(fx.h().getFunscriptJson(fx.cv(), both, 2, OfsFunscript11, buf, sizeof(buf)) > 0);
    const auto j11 = nlohmann::json::parse(buf);
    CHECK(j11["actions"].size() == 2);
    REQUIRE(j11.contains("axes"));
    CHECK(j11["axes"][0]["id"] == "R0");
    CHECK(j11["axes"][0]["actions"].size() == 1);
    CHECK(j11["metadata"]["version"] == "1.1");

    // 2.0 multi-axis: the secondary axis goes under "channels" instead.
    REQUIRE(fx.h().getFunscriptJson(fx.cv(), both, 2, OfsFunscript20, buf, sizeof(buf)) > 0);
    const auto j20 = nlohmann::json::parse(buf);
    REQUIRE(j20.contains("channels"));
    CHECK(j20["channels"].contains("R0"));
    CHECK(j20["metadata"]["version"] == "2.0");

    // 1.0 with several roles is still single-axis: only the first valid axis is serialized.
    REQUIRE(fx.h().getFunscriptJson(fx.cv(), both, 2, OfsFunscript10, buf, sizeof(buf)) > 0);
    const auto j10multi = nlohmann::json::parse(buf);
    CHECK(j10multi["actions"].size() == 2);
    CHECK_FALSE(j10multi.contains("axes"));
}

TEST_CASE("getFunscriptJson guards thread/args and skips absent, empty, and scratch axes") {
    HostApiFixture fx;
    fx.axis(kL0).showInStrip = true; // present, no actions

    char buf[256] = {1};
    const int l0[] = {kL0};
    const int r0[] = {kR0};

    // Present-but-empty and absent axes are both skipped → "" and 0 (NUL-terminated).
    CHECK(fx.h().getFunscriptJson(fx.cv(), l0, 1, OfsFunscript10, buf, sizeof(buf)) == 0);
    CHECK(std::string(buf).empty());
    CHECK(fx.h().getFunscriptJson(fx.cv(), r0, 1, OfsFunscript11, buf, sizeof(buf)) == 0);

    // A scratch axis has no funscript tag, so it is skipped even with actions.
    fx.axis(kS0).showInStrip = true;
    fx.tp.project.mutate(StandardAxis::S0, [](AxisState &a) { a.actions.insert({1.0, 50}); }, fx.tp.eq);
    fx.drain();
    const int s0[] = {kS0};
    CHECK(fx.h().getFunscriptJson(fx.cv(), s0, 1, OfsFunscript11, buf, sizeof(buf)) == 0);

    // Bad args: null roles / non-positive count → empty.
    CHECK(fx.h().getFunscriptJson(fx.cv(), nullptr, 1, OfsFunscript10, buf, sizeof(buf)) == 0);
    CHECK(fx.h().getFunscriptJson(fx.cv(), l0, 0, OfsFunscript10, buf, sizeof(buf)) == 0);

    // Off the main thread → empty fallback, never touches project state.
    int ret = -1;
    std::thread worker([&] { ret = fx.h().getFunscriptJson(fx.cv(), l0, 1, OfsFunscript10, buf, sizeof(buf)); });
    worker.join();
    CHECK(ret == 0);
}

TEST_CASE("getProjectData reads this plugin's own namespaced slot and guards absence/thread") {
    HostApiFixture fx;
    // Two plugins' data coexist in the project; the host namespaces reads by currentPluginName.
    fx.tp.project.pluginData["TestPlugin"]["a"] = nlohmann::json{{"foo", 42}};
    fx.tp.project.pluginData["TestPlugin"]["b"] = 7;
    fx.tp.project.pluginData["Other"]["a"] = "hidden";

    char buf[128] = {1};
    REQUIRE(fx.h().getProjectData(fx.cv(), "a", buf, sizeof(buf)) > 0);
    CHECK(nlohmann::json::parse(buf) == (nlohmann::json{{"foo", 42}}));
    fx.h().getProjectData(fx.cv(), "b", buf, sizeof(buf));
    CHECK(std::string(buf) == "7");

    // Unknown key, and a key that only exists under another plugin, both read as empty.
    CHECK(fx.h().getProjectData(fx.cv(), "missing", buf, sizeof(buf)) == 0);
    CHECK(buf[0] == '\0');
    // "a" exists for "Other" but not visible to TestPlugin beyond its own "a" — confirm isolation: switch
    // the calling plugin to one with no data and the same key returns empty.
    PluginManagerTestAccess::pluginCtx(fx.pm).currentPluginName = "Nobody";
    CHECK(fx.h().getProjectData(fx.cv(), "a", buf, sizeof(buf)) == 0);
    PluginManagerTestAccess::pluginCtx(fx.pm).currentPluginName = "TestPlugin";

    // Off the main thread → guarded: empty, zero length (managed GrowAndRead reads no stale bytes).
    int len = -1;
    std::thread worker([&] { len = fx.h().getProjectData(fx.cv(), "a", buf, sizeof(buf)); });
    worker.join();
    CHECK(len == 0);
    CHECK(buf[0] == '\0');
}

TEST_CASE("setProjectData pushes a namespaced, parsed event and rejects malformed JSON") {
    HostApiFixture fx;

    // A valid JSON value is parsed and forwarded under the calling plugin's name + key.
    fx.h().setProjectData(fx.cv(), "a", R"({"foo":42})");
    fx.drain();
    REQUIRE(fx.pluginData.received.size() == 1);
    CHECK(fx.pluginData.received[0].pluginName == "TestPlugin");
    CHECK(fx.pluginData.received[0].key == "a");
    CHECK(fx.pluginData.received[0].value == (nlohmann::json{{"foo", 42}}));

    // Empty / null payload is an erase: forwarded as a JSON null so the handler removes the key.
    fx.h().setProjectData(fx.cv(), "a", "");
    fx.h().setProjectData(fx.cv(), "a", nullptr);
    fx.drain();
    REQUIRE(fx.pluginData.received.size() == 3);
    CHECK(fx.pluginData.received[1].value.is_null());
    CHECK(fx.pluginData.received[2].value.is_null());

    // Malformed JSON and an empty key are dropped — no event, no corruption.
    fx.h().setProjectData(fx.cv(), "a", "{not json");
    fx.h().setProjectData(fx.cv(), "", R"({"x":1})");
    fx.drain();
    CHECK(fx.pluginData.received.size() == 3);

    // Off the main thread → no event pushed.
    std::thread worker([&] { fx.h().setProjectData(fx.cv(), "a", R"(1)"); });
    worker.join();
    fx.drain();
    CHECK(fx.pluginData.received.size() == 3);
}

TEST_CASE("getAppData/setAppData round-trip a value and clear it (incl. a no-op clear)") {
    HostApiFixture fx;
    char buf[64] = {};

    // Unset key → empty read, zero required length. (No test flushes, so no settings file exists.)
    CHECK(fx.h().getAppData(fx.cv(), "ofs_clear_probe", buf, sizeof(buf)) == 0);
    CHECK(buf[0] == '\0');

    // Set a value → it reads back as the dumped JSON.
    fx.h().setAppData(fx.cv(), "ofs_clear_probe", "42");
    CHECK(fx.h().getAppData(fx.cv(), "ofs_clear_probe", buf, sizeof(buf)) == 2);
    CHECK(std::string(buf) == "42");

    // Clearing with an empty payload erases the key (setAppSetting's erase-hit branch).
    fx.h().setAppData(fx.cv(), "ofs_clear_probe", "");
    CHECK(fx.h().getAppData(fx.cv(), "ofs_clear_probe", buf, sizeof(buf)) == 0);
    CHECK(buf[0] == '\0');

    // Clearing an already-absent key is a no-op (setAppSetting's erase-miss early return) — no crash.
    fx.h().setAppData(fx.cv(), "ofs_clear_probe", nullptr);
    CHECK(fx.h().getAppData(fx.cv(), "ofs_clear_probe", buf, sizeof(buf)) == 0);
}

TEST_CASE("registerCommand reports a distinct code for each failure mode") {
    HostApiFixture fx;
    PluginManagerTestAccess::pluginCtx(fx.pm).inOnLoad = true;

    OfsCommandDef def{};
    def.id = "doit";
    def.title = "Do It";
    def.inPalette = 1;

    // Happy path → OK, and the command lands in the registry under the namespaced id.
    CHECK(fx.h().registerCommand(fx.cv(), &def) == OfsRegisterCommandOk);
    CHECK(fx.cmdReg.find("TestPlugin.doit") != nullptr);

    // Re-registering the same id → DuplicateId (the case that used to be the catch-all).
    CHECK(fx.h().registerCommand(fx.cv(), &def) == OfsRegisterCommandErrDuplicateId);

    // Listed on no surface (neither rebind list nor palette) → NotInvokable.
    OfsCommandDef noInvoke{};
    noInvoke.id = "hidden";
    noInvoke.title = "Hidden";
    CHECK(fx.h().registerCommand(fx.cv(), &noInvoke) == OfsRegisterCommandErrNotInvokable);

    // Empty id, and a null def, → InvalidArg.
    OfsCommandDef emptyId{};
    emptyId.id = "";
    emptyId.title = "X";
    emptyId.inPalette = 1;
    CHECK(fx.h().registerCommand(fx.cv(), &emptyId) == OfsRegisterCommandErrInvalidArg);
    CHECK(fx.h().registerCommand(fx.cv(), nullptr) == OfsRegisterCommandErrInvalidArg);

    // Outside onLoad → NotOnLoad.
    PluginManagerTestAccess::pluginCtx(fx.pm).inOnLoad = false;
    OfsCommandDef late{};
    late.id = "late";
    late.title = "Late";
    late.inPalette = 1;
    CHECK(fx.h().registerCommand(fx.cv(), &late) == OfsRegisterCommandErrNotOnLoad);
}

TEST_CASE("node discrete I/O helpers clamp, bound-check, and tolerate null") {
    OfsDiscreteInput in;
    in.times = {0.0, 1.0, 2.0};
    in.positions = {10.0f, 49.6f, 80.0f};

    const HostApiFixture fx; // for the wired function pointers
    const HostApi &h = PluginManagerTestAccess::hostApi(const_cast<PluginManager &>(fx.pm));

    CHECK(h.nodeInputCount(&in) == 3);
    CHECK(h.nodeInputCount(nullptr) == 0); // null → 0

    CHECK(h.nodeInputTime(&in, 1) == doctest::Approx(1.0));
    CHECK(h.nodeInputTime(&in, -1) == doctest::Approx(0.0)); // out of range → 0
    CHECK(h.nodeInputTime(&in, 99) == doctest::Approx(0.0));
    CHECK(h.nodeInputTime(nullptr, 0) == doctest::Approx(0.0));

    CHECK(h.nodeInputPosition(&in, 1) == 50); // 49.6 rounds to 50
    CHECK(h.nodeInputPosition(&in, 99) == 0); // out of range → 0
    CHECK(h.nodeInputPosition(nullptr, 0) == 0);

    OfsDiscreteOutput out;
    h.nodeAddAction(&out, 4.0, 150);   // position clamped to 100
    h.nodeAddAction(&out, 5.0, -20);   // clamped to 0
    h.nodeAddAction(nullptr, 6.0, 50); // null → no-op
    REQUIRE(out.times.size() == 2);
    CHECK(out.positions[0] == doctest::Approx(100.0f));
    CHECK(out.positions[1] == doctest::Approx(0.0f));
}

TEST_CASE("notifyPluginFault coalesces repeat faults per plugin within the window") {
    HostApiFixture fx;

    // First fault for a plugin emits an error toast; a second within the 3s window is suppressed.
    fx.pm.notifyPluginFault("PluginA", "OnUpdate");
    fx.pm.notifyPluginFault("PluginA", "OnUpdate");
    fx.drain();
    REQUIRE(fx.notifies.received.size() == 1);
    CHECK(fx.notifies.received[0].level == NotifyLevel::Error);
    CHECK(fx.notifies.received[0].message.find("PluginA") != std::string::npos);

    // A different plugin is tracked separately, so it emits its own toast.
    fx.pm.notifyPluginFault("PluginB", "node:gen");
    fx.drain();
    REQUIRE(fx.notifies.received.size() == 2);
    CHECK(fx.notifies.received[1].message.find("PluginB") != std::string::npos);
}

TEST_CASE("hostNotify clamps an out-of-range level and prefixes the plugin name") {
    HostApiFixture fx;

    // Out-of-range level falls back to Info; the message is shown verbatim with the plugin prefix.
    fx.h().hostNotify(fx.cv(), 999, "hello");
    fx.drain();
    REQUIRE(fx.notifies.received.size() == 1);
    CHECK(fx.notifies.received[0].level == NotifyLevel::Info);
    CHECK(fx.notifies.received[0].message.find("TestPlugin") != std::string::npos);
    CHECK(fx.notifies.received[0].message.find("hello") != std::string::npos);

    // A valid level is preserved.
    fx.h().hostNotify(fx.cv(), static_cast<int>(NotifyLevel::Error), "boom");
    fx.drain();
    REQUIRE(fx.notifies.received.size() == 2);
    CHECK(fx.notifies.received[1].level == NotifyLevel::Error);
}
