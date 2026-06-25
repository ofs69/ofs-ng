#include <doctest/doctest.h>

#include "Core/Events.h"
#include "Core/IntentEvents.h"
#include "Services/CommandProviders.h"
#include "Services/EditModeRegistry.h"
#include "Services/NavigatorRegistry.h"
#include "Services/SelectionModeRegistry.h"
#include "helpers/EventCapture.h"
#include "helpers/TestProject.h"

#include <algorithm>

using ofs::test::EventCapture;
using ofs::test::TestProject;

namespace {

// Find the first generated command whose group matches; nullptr if none.
const ofs::Command *byGroup(const std::vector<ofs::Command> &cmds, const char *group) {
    for (const auto &c : cmds)
        if (c.group == group)
            return &c;
    return nullptr;
}

const ofs::Command *byId(const std::vector<ofs::Command> &cmds, const std::string &id) {
    for (const auto &c : cmds)
        if (c.id == id)
            return &c;
    return nullptr;
}

int countGroup(const std::vector<ofs::Command> &cmds, const char *group) {
    return static_cast<int>(
        std::count_if(cmds.begin(), cmds.end(), [&](const ofs::Command &c) { return c.group == group; }));
}

// Default registries (each seeds only its native entry) for the dynamic-build / signature helpers, so
// the existing axis/nav tests don't have to thread three registry args through every call.
const ofs::EditModeRegistry kEditModes;
const ofs::NavigatorRegistry kNavigators;
const ofs::SelectionModeRegistry kSelectionModes;

uint64_t dynSig(const ofs::ScriptProject &p) {
    return ofs::dynamicCommandsSignature(p, kEditModes, kNavigators, kSelectionModes);
}

} // namespace

// ── Generation ───────────────────────────────────────────────────────────────

TEST_CASE("buildNavigationCommands lists chapters and bookmarks only") {
    TestProject tp;
    tp.project.bookmarks.chapters.push_back({.startTime = 1.0, .endTime = 2.0, .name = "Intro"});
    tp.project.bookmarks.bookmarks.push_back({.time = 3.0, .name = "Mark"});
    tp.project.axes[static_cast<size_t>(ofs::StandardAxis::L0)].showInStrip = true;

    std::vector<ofs::Command> cmds;
    ofs::buildNavigationCommands(tp.project, cmds);

    CHECK(countGroup(cmds, "Go to Chapter") == 1);
    CHECK(countGroup(cmds, "Go to Bookmark") == 1);
    // Select Axis is no longer a navigation command — it moved to the registry-resident provider set.
    CHECK(countGroup(cmds, "Select Axis") == 0);

    const auto *chapter = byGroup(cmds, "Go to Chapter");
    REQUIRE(chapter != nullptr);
    CHECK(chapter->title == "Intro");
}

TEST_CASE("navigation commands are palette-only (not in the rebind list)") {
    TestProject tp;
    tp.project.bookmarks.chapters.push_back({.startTime = 0.0, .endTime = 1.0, .name = "A"});

    std::vector<ofs::Command> cmds;
    ofs::buildNavigationCommands(tp.project, cmds);

    REQUIRE(cmds.size() == 1);
    CHECK(cmds[0].inRebindList == false);
    CHECK(cmds[0].inPalette == true);
    CHECK(cmds[0].source == ofs::CommandSource::Native);
}

TEST_CASE("empty chapter/bookmark names fall back to an index label") {
    TestProject tp;
    tp.project.bookmarks.chapters.push_back({.startTime = 0.0, .endTime = 1.0, .name = ""});
    tp.project.bookmarks.bookmarks.push_back({.time = 5.0, .name = ""});

    std::vector<ofs::Command> cmds;
    ofs::buildNavigationCommands(tp.project, cmds);

    CHECK(byGroup(cmds, "Go to Chapter")->title == "Chapter 1");
    CHECK(byGroup(cmds, "Go to Bookmark")->title == "Bookmark 1");
}

// ── run() closures push the right navigation events ───────────────────────────

TEST_CASE("chapter command seeks to the chapter start") {
    TestProject tp;
    tp.project.bookmarks.chapters.push_back({.startTime = 12.5, .endTime = 20.0, .name = "Scene"});

    std::vector<ofs::Command> cmds;
    ofs::buildNavigationCommands(tp.project, cmds);
    const auto *chapter = byGroup(cmds, "Go to Chapter");
    REQUIRE(chapter != nullptr);

    EventCapture<ofs::SeekEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    chapter->run(tp.eq);
    tp.eq.drain();

    REQUIRE(cap.received.size() == 1);
    CHECK(cap.received[0].time == doctest::Approx(12.5));
}

// ── Registry-resident axis providers: Select Axis & Toggle in Panel ───────────────────────────────

TEST_CASE("Select Axis is present for every standard axis, source=Dynamic, opt-in bindable") {
    TestProject tp; // nothing shown
    std::vector<ofs::Command> cmds;
    ofs::buildAxisProviderCommands(tp.project, cmds);

    // Every standard axis (L0–A1) always carries a Select Axis command so a binding has a stable target;
    // scratch axes only appear once they exist (none here).
    CHECK(countGroup(cmds, "Select Axis") == 10);
    const auto *l0 = byId(cmds, "nav.axis.L0");
    REQUIRE(l0 != nullptr);
    CHECK(l0->title == "L0 (Stroke)");
    CHECK(l0->source == ofs::CommandSource::Dynamic);
    CHECK(l0->inRebindList == false); // opt-in: surfaced via the Shortcut window's "show providers" toggle
    CHECK_FALSE(l0->enabled());       // not shown → no useful effect → disabled
}

TEST_CASE("Select Axis is enabled only when the axis is shown and not already active") {
    TestProject tp;
    tp.project.axes[static_cast<size_t>(ofs::StandardAxis::R0)].showInStrip = true;
    tp.project.state.activeAxis = ofs::StandardAxis::A1; // R0 shown, not active

    std::vector<ofs::Command> cmds;
    ofs::buildAxisProviderCommands(tp.project, cmds);
    const auto *r0 = byId(cmds, "nav.axis.R0");
    REQUIRE(r0 != nullptr);
    CHECK(r0->enabled());

    // isEnabled reads the live project, so activating R0 disables its own switch-to command with no
    // rebuild — the binding stays attached while the command is merely greyed.
    tp.project.state.activeAxis = ofs::StandardAxis::R0;
    CHECK_FALSE(r0->enabled());
}

TEST_CASE("Select Axis run selects the axis") {
    TestProject tp;
    std::vector<ofs::Command> cmds;
    ofs::buildAxisProviderCommands(tp.project, cmds);
    const auto *axis = byId(cmds, "nav.axis.R1");
    REQUIRE(axis != nullptr);

    EventCapture<ofs::AxisSelectedEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    axis->run(tp.eq);
    tp.eq.drain();

    REQUIRE(cap.received.size() == 1);
    CHECK(cap.received[0].role == ofs::StandardAxis::R1);
}

TEST_CASE("Toggle in Panel exists for every axis except L0 (pinned)") {
    TestProject tp;
    std::vector<ofs::Command> cmds;
    ofs::buildAxisProviderCommands(tp.project, cmds);

    CHECK(countGroup(cmds, "Toggle in Panel") == 9); // all standard axes except the pinned L0
    CHECK(byId(cmds, "axis.toggle-panel.L0") == nullptr);
    const auto *r0 = byId(cmds, "axis.toggle-panel.R0");
    REQUIRE(r0 != nullptr);
    CHECK(r0->source == ofs::CommandSource::Dynamic);
}

TEST_CASE("Toggle in Panel flips the axis's current visibility") {
    TestProject tp;
    auto &r0 = tp.project.axes[static_cast<size_t>(ofs::StandardAxis::R0)];
    r0.showInStrip = false;

    std::vector<ofs::Command> cmds;
    ofs::buildAxisProviderCommands(tp.project, cmds);
    const auto *toggle = byId(cmds, "axis.toggle-panel.R0");
    REQUIRE(toggle != nullptr);

    EventCapture<ofs::ToggleAxisPanelVisibilityEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    toggle->run(tp.eq); // currently hidden → asks to show
    tp.eq.drain();
    REQUIRE(cap.received.size() == 1);
    CHECK(cap.received[0].axisRole == ofs::StandardAxis::R0);
    CHECK(cap.received[0].inPanel == true);

    // The run reads the live state, so once the axis is shown the same command asks to hide it.
    r0.showInStrip = true;
    toggle->run(tp.eq);
    tp.eq.drain();
    REQUIRE(cap.received.size() == 2);
    CHECK(cap.received[1].inPanel == false);
}

TEST_CASE("Toggle in Panel is disabled for an empty scratch axis") {
    TestProject tp;
    auto &s0 = tp.project.axes[static_cast<size_t>(ofs::StandardAxis::S0)];
    s0.showInStrip = true; // empty scratch → exists() (present) but managed via Add/Delete, not toggled

    std::vector<ofs::Command> cmds;
    ofs::buildAxisProviderCommands(tp.project, cmds);
    const auto *toggle = byId(cmds, "axis.toggle-panel.S0");
    REQUIRE(toggle != nullptr);
    CHECK_FALSE(toggle->enabled());

    // Gaining data makes it a standard-like axis, so its toggle becomes usable (live isEnabled, no rebuild).
    s0.actions.insert({1.0, 50});
    CHECK(toggle->enabled());
}

TEST_CASE("scratch axis providers appear only while the axis exists") {
    TestProject tp; // S0 hidden + empty → does not exist
    std::vector<ofs::Command> cmds;
    ofs::buildAxisProviderCommands(tp.project, cmds);
    CHECK(byId(cmds, "nav.axis.S0") == nullptr);
    CHECK(byId(cmds, "axis.toggle-panel.S0") == nullptr);

    // A hidden scratch axis that holds data exists(), so its select/toggle commands appear (the stable
    // binding target the per-frame palette set never offered).
    tp.project.axes[static_cast<size_t>(ofs::StandardAxis::S0)].actions.insert({1.0, 50});
    cmds.clear();
    ofs::buildAxisProviderCommands(tp.project, cmds);
    CHECK(byId(cmds, "nav.axis.S0") != nullptr);
    CHECK(byId(cmds, "axis.toggle-panel.S0") != nullptr);
}

// ── Axis management: Delete Scratch Axis (still palette-only) ──────────────────

TEST_CASE("Delete Scratch Axis lists only present scratch axes") {
    TestProject tp;
    tp.project.axes[static_cast<size_t>(ofs::StandardAxis::L0)].showInStrip = true; // standard, not deletable
    tp.project.axes[static_cast<size_t>(ofs::StandardAxis::S0)].showInStrip = true; // scratch, deletable
    tp.project.axes[static_cast<size_t>(ofs::StandardAxis::S1)].showInStrip = true; // scratch, deletable

    std::vector<ofs::Command> cmds;
    ofs::buildAxisCommands(tp.project, cmds);

    CHECK(countGroup(cmds, "Delete Scratch Axis") == 2);
    CHECK(byGroup(cmds, "Delete Scratch Axis")->title == "S0");
}

TEST_CASE("Delete Scratch Axis command removes the axis") {
    TestProject tp;
    tp.project.axes[static_cast<size_t>(ofs::StandardAxis::S0)].showInStrip = true;

    std::vector<ofs::Command> cmds;
    ofs::buildAxisCommands(tp.project, cmds);
    const auto *del = byGroup(cmds, "Delete Scratch Axis");
    REQUIRE(del != nullptr);

    EventCapture<ofs::RemoveAxisEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    del->run(tp.eq);
    tp.eq.drain();

    REQUIRE(cap.received.size() == 1);
    CHECK(cap.received[0].axisRole == ofs::StandardAxis::S0);
}

// A scratch axis that holds actions behaves like a standard axis: it can't be deleted (only emptied
// first), and it toggles its panel visibility via the registry-resident provider instead.
TEST_CASE("Delete Scratch Axis excludes a scratch axis that holds actions") {
    TestProject tp;
    auto &s0 = tp.project.axes[static_cast<size_t>(ofs::StandardAxis::S0)];
    auto &s1 = tp.project.axes[static_cast<size_t>(ofs::StandardAxis::S1)];
    s0.showInStrip = true; // empty scratch → deletable
    s1.showInStrip = true; // data-bearing scratch → not deletable
    s1.actions.insert({1.0, 50});

    std::vector<ofs::Command> cmds;
    ofs::buildAxisCommands(tp.project, cmds);

    REQUIRE(countGroup(cmds, "Delete Scratch Axis") == 1);
    CHECK(byGroup(cmds, "Delete Scratch Axis")->title == "S0");

    // S1 holds data, so instead of Delete it gets a usable Toggle-in-Panel from the registry provider.
    std::vector<ofs::Command> providers;
    ofs::buildAxisProviderCommands(tp.project, providers);
    const auto *s1Toggle = byId(providers, "axis.toggle-panel.S1");
    REQUIRE(s1Toggle != nullptr);
    CHECK(s1Toggle->enabled());
}

// ── Interaction-mode switch commands ──────────────────────────────────────────

TEST_CASE("buildModeSwitchCommands emits every mode; the active one is present but disabled") {
    TestProject tp; // defaults: all three active ids are the native ids
    ofs::EditModeRegistry editModes;
    editModes.add({.id = "plugin.alt", .displayName = "Alt Mode", .owningPlugin = "Ofs.Core"});
    ofs::NavigatorRegistry navigators;
    navigators.add({.id = "plugin.peak", .displayName = "Next peak", .owningPlugin = "Ofs.Core"});
    ofs::SelectionModeRegistry selectionModes;
    selectionModes.add({.id = "plugin.even", .displayName = "Even only", .owningPlugin = "Ofs.Core"});

    std::vector<ofs::Command> cmds;
    ofs::buildModeSwitchCommands(tp.project, editModes, navigators, selectionModes, cmds);

    // Both the native (active) and plugin entry are present in each registry — a binding to the active
    // mode must survive activation, so its command can't blink out.
    REQUIRE(countGroup(cmds, "Switch Edit Mode") == 2);
    REQUIRE(countGroup(cmds, "Switch Navigator") == 2);
    REQUIRE(countGroup(cmds, "Switch Selection Mode") == 2);

    // The non-active (plugin) command is enabled; the active (native) one is present but disabled.
    const auto *alt = byId(cmds, "mode.edit.plugin.alt");
    REQUIRE(alt != nullptr);
    CHECK(alt->title == "Alt Mode");
    CHECK(alt->enabled());
    CHECK(alt->inRebindList == false); // not offered for binding by default; the user opts in
    CHECK(alt->source == ofs::CommandSource::Dynamic);
    const auto *activeEdit = byId(cmds, "mode.edit." + tp.project.activeEditMode);
    REQUIRE(activeEdit != nullptr);
    CHECK_FALSE(activeEdit->enabled()); // switching to the active mode is a no-op
}

TEST_CASE("a mode with no display name falls back to its id") {
    TestProject tp;
    ofs::EditModeRegistry editModes;
    editModes.add({.id = "plugin.bare", .owningPlugin = "Ofs.Core"}); // no displayName
    ofs::NavigatorRegistry navigators;
    ofs::SelectionModeRegistry selectionModes;

    std::vector<ofs::Command> cmds;
    ofs::buildModeSwitchCommands(tp.project, editModes, navigators, selectionModes, cmds);

    const auto *bare = byId(cmds, "mode.edit.plugin.bare");
    REQUIRE(bare != nullptr);
    CHECK(bare->title == "plugin.bare");
}

TEST_CASE("the active mode's command is disabled while the others stay enabled") {
    TestProject tp;
    tp.project.activeEditMode = "plugin.alt"; // a plugin mode is active
    ofs::EditModeRegistry editModes;
    editModes.add({.id = "plugin.alt", .displayName = "Alt Mode", .owningPlugin = "Ofs.Core"});
    ofs::NavigatorRegistry navigators;
    ofs::SelectionModeRegistry selectionModes;

    std::vector<ofs::Command> cmds;
    ofs::buildModeSwitchCommands(tp.project, editModes, navigators, selectionModes, cmds);

    // plugin.alt is active → its command is disabled; native is now the enabled switch target.
    const auto *alt = byId(cmds, "mode.edit.plugin.alt");
    REQUIRE(alt != nullptr);
    CHECK_FALSE(alt->enabled());
    const auto *nativeEdit = byId(cmds, "mode.edit.native");
    REQUIRE(nativeEdit != nullptr);
    CHECK(nativeEdit->enabled());
}

TEST_CASE("edit-mode command pushes SetActiveEditModeEvent with the mode id") {
    TestProject tp;
    ofs::EditModeRegistry editModes;
    editModes.add({.id = "plugin.alt", .displayName = "Alt Mode", .owningPlugin = "Ofs.Core"});
    ofs::NavigatorRegistry navigators;
    ofs::SelectionModeRegistry selectionModes;

    std::vector<ofs::Command> cmds;
    ofs::buildModeSwitchCommands(tp.project, editModes, navigators, selectionModes, cmds);
    const auto *cmd = byId(cmds, "mode.edit.plugin.alt");
    REQUIRE(cmd != nullptr);

    EventCapture<ofs::SetActiveEditModeEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    cmd->run(tp.eq);
    tp.eq.drain();

    REQUIRE(cap.received.size() == 1);
    CHECK(cap.received[0].id == "plugin.alt");
}

TEST_CASE("navigator and selection commands push their respective SetActive events") {
    TestProject tp;
    ofs::EditModeRegistry editModes;
    ofs::NavigatorRegistry navigators;
    navigators.add({.id = "plugin.peak", .displayName = "Next peak", .owningPlugin = "Ofs.Core"});
    ofs::SelectionModeRegistry selectionModes;
    selectionModes.add({.id = "plugin.even", .displayName = "Even only", .owningPlugin = "Ofs.Core"});

    std::vector<ofs::Command> cmds;
    ofs::buildModeSwitchCommands(tp.project, editModes, navigators, selectionModes, cmds);

    EventCapture<ofs::SetActiveNavigatorEvent> nav;
    EventCapture<ofs::SetActiveSelectionModeEvent> sel;
    nav.attach(tp.eq);
    sel.attach(tp.eq);
    tp.eq.freeze();

    byId(cmds, "mode.navigator.plugin.peak")->run(tp.eq);
    byId(cmds, "mode.select.plugin.even")->run(tp.eq);
    tp.eq.drain();

    REQUIRE(nav.received.size() == 1);
    CHECK(nav.received[0].id == "plugin.peak");
    REQUIRE(sel.received.size() == 1);
    CHECK(sel.received[0].id == "plugin.even");
}

// ── Tool-options commands (registry-resident) ─────────────────────────────────

namespace {
// A non-null onUi marks a mode as supplying an options UI; its body never runs in these tests.
void dummyUi(void *) {}
} // namespace

TEST_CASE("buildToolOptionsCommands emits one per onUi mode; only the active one is enabled") {
    TestProject tp; // native edit mode is active and has no onUi
    ofs::EditModeRegistry editModes;
    editModes.add({.id = "plugin.a", .displayName = "Mode A", .owningPlugin = "Ofs.Core", .onUi = dummyUi});
    editModes.add({.id = "plugin.b", .displayName = "Mode B", .owningPlugin = "Ofs.Core", .onUi = dummyUi});
    ofs::NavigatorRegistry navigators;
    ofs::SelectionModeRegistry selectionModes;

    std::vector<ofs::Command> cmds;
    ofs::buildToolOptionsCommands(tp.project, editModes, navigators, selectionModes, cmds);

    // The native edit mode has no onUi → no command; both plugin modes do. (No navigator/selection mode
    // here supplies onUi, so none of those are emitted.)
    CHECK(countGroup(cmds, "Edit Mode Options") == 2);
    const auto *a = byId(cmds, "tooloptions.edit.plugin.a");
    REQUIRE(a != nullptr);
    CHECK(a->source == ofs::CommandSource::Dynamic);
    CHECK(a->inRebindList == false);
    CHECK_FALSE(a->enabled()); // plugin.a is not the active edit mode

    // Activating plugin.a enables its options command (live isEnabled); plugin.b stays disabled.
    tp.project.activeEditMode = "plugin.a";
    CHECK(a->enabled());
    CHECK_FALSE(byId(cmds, "tooloptions.edit.plugin.b")->enabled());
}

TEST_CASE("tool-options run opens its extension point's options modal") {
    TestProject tp;
    tp.project.activeEditMode = "plugin.a";
    ofs::EditModeRegistry editModes;
    editModes.add({.id = "plugin.a", .displayName = "Mode A", .owningPlugin = "Ofs.Core", .onUi = dummyUi});
    ofs::NavigatorRegistry navigators;
    ofs::SelectionModeRegistry selectionModes;

    std::vector<ofs::Command> cmds;
    ofs::buildToolOptionsCommands(tp.project, editModes, navigators, selectionModes, cmds);
    const auto *a = byId(cmds, "tooloptions.edit.plugin.a");
    REQUIRE(a != nullptr);

    EventCapture<ofs::OpenToolOptionsEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    a->run(tp.eq);
    tp.eq.drain();

    REQUIRE(cap.received.size() == 1);
    CHECK(cap.received[0].target == ofs::ToolOptionTarget::Edit);
}

// ── Signature (lazy-rebuild trigger) ──────────────────────────────────────────

TEST_CASE("navigationSignature is stable when nothing changes") {
    TestProject tp;
    tp.project.bookmarks.chapters.push_back({.startTime = 1.0, .endTime = 2.0, .name = "A"});
    CHECK(ofs::navigationSignature(tp.project) == ofs::navigationSignature(tp.project));
}

TEST_CASE("navigationSignature changes when nav state changes") {
    TestProject tp;
    const uint64_t base = ofs::navigationSignature(tp.project);

    // Adding a chapter changes it.
    tp.project.bookmarks.chapters.push_back({.startTime = 1.0, .endTime = 2.0, .name = "A"});
    const uint64_t withChapter = ofs::navigationSignature(tp.project);
    CHECK(withChapter != base);

    // Renaming a chapter (same count) still changes it.
    tp.project.bookmarks.chapters[0].name = "B";
    CHECK(ofs::navigationSignature(tp.project) != withChapter);

    // Adding a bookmark changes it.
    const uint64_t beforeBookmark = ofs::navigationSignature(tp.project);
    tp.project.bookmarks.bookmarks.push_back({.time = 5.0, .name = "M"});
    CHECK(ofs::navigationSignature(tp.project) != beforeBookmark);
}

static uint64_t regSig(const ofs::ScriptProject &p) {
    return ofs::registryProviderSignature(p, kEditModes, kNavigators, kSelectionModes);
}

TEST_CASE("dynamicCommandsSignature is blind to standard-axis panel visibility and the active axis") {
    TestProject tp;
    auto &axis = tp.project.axes[static_cast<size_t>(ofs::StandardAxis::L0)];
    axis.showInStrip = true;
    tp.project.state.activeAxis = ofs::StandardAxis::L0;
    const uint64_t before = dynSig(tp.project);

    // Select/Toggle are registry-resident now (gated live by isEnabled), so neither hiding a standard axis
    // nor switching the active axis changes which *palette-only* commands exist — no per-frame rebuild.
    axis.showInStrip = false;
    tp.project.state.activeAxis = ofs::StandardAxis::R0;
    CHECK(dynSig(tp.project) == before);
}

TEST_CASE("dynamicCommandsSignature reacts to a scratch axis gaining/losing data (Delete Scratch Axis)") {
    TestProject tp;
    auto &s0 = tp.project.axes[static_cast<size_t>(ofs::StandardAxis::S0)];
    s0.showInStrip = true; // empty, shown scratch → Delete Scratch Axis offered
    const uint64_t before = dynSig(tp.project);

    // Gaining its first action makes it no longer deletable, so the palette-only set changes.
    s0.actions.insert({1.0, 50});
    CHECK(dynSig(tp.project) != before);
}

TEST_CASE("registryProviderSignature reacts to a mode registry gaining an entry") {
    TestProject tp;
    ofs::EditModeRegistry editModes;
    const uint64_t before = ofs::registryProviderSignature(tp.project, editModes, kNavigators, kSelectionModes);

    // A plugin registering an edit mode adds a registry-resident "Switch Edit Mode" command (and, if it
    // has options, a tool-options command), so the registry-resident set must rebuild.
    editModes.add({.id = "plugin.alt", .displayName = "Alt Mode", .owningPlugin = "Ofs.Core"});
    CHECK(ofs::registryProviderSignature(tp.project, editModes, kNavigators, kSelectionModes) != before);
}

TEST_CASE("registryProviderSignature reacts to a scratch axis coming into / out of existence") {
    TestProject tp;
    const uint64_t before = regSig(tp.project);

    // Showing a scratch axis makes it exist() → its Select/Toggle commands enter the registry-resident set.
    auto &s0 = tp.project.axes[static_cast<size_t>(ofs::StandardAxis::S0)];
    s0.showInStrip = true;
    const uint64_t withScratch = regSig(tp.project);
    CHECK(withScratch != before);

    // Hiding it again (still empty) drops it back out of existence → the set shrinks.
    s0.showInStrip = false;
    CHECK(regSig(tp.project) == before);
}

TEST_CASE("registryProviderSignature is blind to the active mode and active axis") {
    TestProject tp;
    ofs::EditModeRegistry editModes;
    editModes.add({.id = "plugin.alt", .displayName = "Alt Mode", .owningPlugin = "Ofs.Core"});
    for (auto role : {ofs::StandardAxis::L0, ofs::StandardAxis::R0})
        tp.project.axes[static_cast<size_t>(role)].showInStrip = true;
    tp.project.state.activeAxis = ofs::StandardAxis::L0;
    const uint64_t before = ofs::registryProviderSignature(tp.project, editModes, kNavigators, kSelectionModes);

    // Activating a different mode or axis only flips isEnabled on already-present commands — no rebuild.
    tp.project.activeEditMode = "plugin.alt";
    tp.project.state.activeAxis = ofs::StandardAxis::R0;
    CHECK(ofs::registryProviderSignature(tp.project, editModes, kNavigators, kSelectionModes) == before);
}

// ── Command::isEnabled seam ───────────────────────────────────────────────────

TEST_CASE("Command::enabled reflects the isEnabled predicate") {
    ofs::Command always{.id = "x.a", .group = "G", .title = "A"};
    CHECK(always.enabled()); // null predicate = always enabled

    bool gate = false;
    ofs::Command gated{.id = "x.b", .group = "G", .title = "B", .isEnabled = [&gate] { return gate; }};
    CHECK_FALSE(gated.enabled());
    gate = true;
    CHECK(gated.enabled());
}
