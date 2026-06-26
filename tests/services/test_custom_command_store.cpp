#include "Core/CommandEvents.h"
#include "Core/EventQueue.h"
#include "Core/IntentEvents.h"
#include "Core/ScriptProject.h"
#include "Core/StandardAxis.h"
#include "Format/AppSettings.h"
#include "Services/BindingSystem.h"
#include "Services/CommandRegistry.h"
#include "Services/CustomCommandStore.h"
#include "Services/CustomCommandTemplate.h"
#include "Util/FileUtil.h"
#include "Util/PathUtil.h"
#include "helpers/EventCapture.h"
#include "helpers/TestProject.h"
#include <doctest/doctest.h>
#include <filesystem>
#include <system_error>

using ofs::test::EventCapture;
using ofs::test::TestProject;

namespace {

// Start each test from a clean file: getPrefPath() resolves to a per-binary temp dir, but the store
// writes custom_commands.json there, so a prior run's file would otherwise leak into the next.
void removeStoreFile() {
    std::error_code ec;
    std::filesystem::remove(ofs::util::getPrefPath() / "custom_commands.json", ec);
}

ofs::CustomCommand stepDef(const char *name, int dir, int reps, ofs::StepGranularity g = ofs::StepGranularity::Frame) {
    return {.name = name,
            .templateKey = "step",
            .direction = static_cast<ofs::StepDirection>(dir),
            .reps = reps,
            .granularity = g};
}

} // namespace

TEST_CASE("CustomCommandStore: Add allocates a stable id and registers a rebind-listed, holdable Command") {
    removeStoreFile();
    TestProject tp;
    ofs::AppSettings settings;
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
    ofs::CommandRegistry registry{tp.eq};
    ofs::CustomCommandStore store{tp.eq, registry, templates};
    store.load(); // empty
    tp.eq.freeze();

    tp.eq.push(ofs::AddCustomCommandEvent{stepDef("Jump 5", 1, 5)});
    tp.eq.drain();

    REQUIRE(store.commands().size() == 1);
    CHECK(store.commands()[0].id == "custom.0"); // store assigns the id; any incoming id is ignored
    CHECK(store.commands()[0].name == "Jump 5");
    const ofs::Command *c = registry.find("custom.0");
    REQUIRE(c != nullptr);
    CHECK(c->source == ofs::CommandSource::Custom);
    CHECK(c->inRebindList);
    CHECK(c->holdable());
}

TEST_CASE("CustomCommandStore::load ignores a file newer than the supported version") {
    removeStoreFile();
    // A file whose schema version exceeds this build's must not have its commands loaded.
    const std::string json = R"({
        "version": 999999,
        "nextId": 1,
        "commands": [{"id": "custom.0", "name": "From The Future", "kind": "step",
                      "direction": 1, "reps": 5, "granularity": "frame"}]
    })";
    REQUIRE(ofs::util::writeFile(ofs::util::getPrefPath() / "custom_commands.json", json));

    TestProject tp;
    ofs::AppSettings settings;
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
    ofs::CommandRegistry registry{tp.eq};
    ofs::CustomCommandStore store{tp.eq, registry, templates};
    store.load();
    tp.eq.freeze();

    CHECK(store.commands().empty());
    CHECK(registry.find("custom.0") == nullptr);

    removeStoreFile();
}

TEST_CASE("CustomCommandStore: Update keeps the id so existing bindings stay attached") {
    removeStoreFile();
    TestProject tp;
    ofs::AppSettings settings;
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
    ofs::CommandRegistry registry{tp.eq};
    ofs::CustomCommandStore store{tp.eq, registry, templates};
    store.load();
    tp.eq.freeze();

    tp.eq.push(ofs::AddCustomCommandEvent{stepDef("Jump 5", 1, 5)});
    tp.eq.drain();
    ofs::CustomCommand def = store.commands()[0];
    def.name = "Jump 7";
    def.reps = 7;
    tp.eq.push(ofs::UpdateCustomCommandEvent{def});
    tp.eq.drain();

    REQUIRE(store.commands().size() == 1);
    CHECK(store.commands()[0].id == "custom.0");
    CHECK(store.commands()[0].name == "Jump 7");
    CHECK(store.commands()[0].reps == 7);
    CHECK(registry.find("custom.0") != nullptr);
}

TEST_CASE("CustomCommandStore: Remove drops the definition and the registry Command") {
    removeStoreFile();
    TestProject tp;
    ofs::AppSettings settings;
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
    ofs::CommandRegistry registry{tp.eq};
    ofs::CustomCommandStore store{tp.eq, registry, templates};
    store.load();
    tp.eq.freeze();

    tp.eq.push(ofs::AddCustomCommandEvent{stepDef("Jump 5", 1, 5)});
    tp.eq.drain();
    tp.eq.push(ofs::RemoveCustomCommandEvent{"custom.0"});
    tp.eq.drain();

    CHECK(store.commands().empty());
    CHECK(registry.find("custom.0") == nullptr);
}

TEST_CASE("CustomCommandStore: definitions persist across a reload; ids are append-only") {
    removeStoreFile();
    {
        TestProject tp;
        ofs::AppSettings settings;
        ofs::CustomCommandTemplateRegistry templates;
        ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
        ofs::CommandRegistry registry{tp.eq};
        ofs::CustomCommandStore store{tp.eq, registry, templates};
        store.load();
        tp.eq.freeze();
        tp.eq.push(ofs::AddCustomCommandEvent{stepDef("A", 1, 1)});
        tp.eq.push(
            ofs::AddCustomCommandEvent{ofs::CustomCommand{.name = "B", .templateKey = "move-position", .delta = 7}});
        tp.eq.drain();
        REQUIRE(store.commands().size() == 2);
        CHECK(store.commands()[1].id == "custom.1");
    }

    // A fresh store reads the file written above.
    TestProject tp;
    ofs::AppSettings settings;
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
    ofs::CommandRegistry registry{tp.eq};
    ofs::CustomCommandStore store{tp.eq, registry, templates};
    store.load();
    REQUIRE(store.commands().size() == 2);
    CHECK(store.commands()[0].id == "custom.0");
    CHECK(store.commands()[1].id == "custom.1");
    CHECK(store.commands()[1].templateKey == "move-position");
    CHECK(store.commands()[1].delta == 7);
    CHECK(registry.find("custom.1") != nullptr);

    // nextId survived the reload, so the next add takes custom.2 — a deleted/loaded id is never reused.
    tp.eq.freeze();
    tp.eq.push(ofs::AddCustomCommandEvent{stepDef("C", -1, 2)});
    tp.eq.drain();
    REQUIRE(store.commands().size() == 3);
    CHECK(store.commands()[2].id == "custom.2");
}

TEST_CASE("CustomCommandStore: Update/Remove of an unknown id are no-ops") {
    removeStoreFile();
    TestProject tp;
    ofs::AppSettings settings;
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
    ofs::CommandRegistry registry{tp.eq};
    ofs::CustomCommandStore store{tp.eq, registry, templates};
    store.load();
    tp.eq.freeze();

    tp.eq.push(ofs::AddCustomCommandEvent{stepDef("Keep", 1, 1)});
    tp.eq.drain();

    tp.eq.push(ofs::UpdateCustomCommandEvent{stepDef("Ghost", -1, 9)}); // no id set ⇒ no match
    tp.eq.push(ofs::RemoveCustomCommandEvent{"custom.999"});            // not present
    tp.eq.drain();

    REQUIRE(store.commands().size() == 1);
    CHECK(store.commands()[0].id == "custom.0");
    CHECK(store.commands()[0].name == "Keep"); // the stray Update did not overwrite it
}

TEST_CASE("CustomCommandStore: a corrupt commands file loads as an empty set") {
    removeStoreFile();
    ofs::util::writeFile(ofs::util::getPrefPath() / "custom_commands.json", "{ this is not valid json ]");

    TestProject tp;
    ofs::AppSettings settings;
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
    ofs::CommandRegistry registry{tp.eq};
    ofs::CustomCommandStore store{tp.eq, registry, templates};
    store.load();

    CHECK(store.commands().empty());
}

TEST_CASE("CustomCommandStore: an entry with no id is skipped; a foreign id still bumps the allocator") {
    removeStoreFile();
    // idNumber returns -1 for a non-"custom.N" id (foreign/hand-edited); it must not poison nextId, and
    // a fresh add still lands above any numeric id actually seen.
    ofs::util::writeFile(ofs::util::getPrefPath() / "custom_commands.json", R"({
        "version": 1, "nextId": 0,
        "commands": [
            { "id": "", "name": "NoId", "kind": "step", "direction": 1, "reps": 1 },
            { "id": "mybind", "name": "Foreign", "kind": "step", "direction": 1, "reps": 1 },
            { "id": "custom.4", "name": "Numbered", "kind": "step", "direction": 1, "reps": 1 }
        ]
    })");

    TestProject tp;
    ofs::AppSettings settings;
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
    ofs::CommandRegistry registry{tp.eq};
    ofs::CustomCommandStore store{tp.eq, registry, templates};
    store.load();

    REQUIRE(store.commands().size() == 2); // empty-id entry dropped, the other two kept
    tp.eq.freeze();
    tp.eq.push(ofs::AddCustomCommandEvent{stepDef("Next", 1, 1)});
    tp.eq.drain();
    CHECK(store.commands().back().id == "custom.5"); // bumped past custom.4, ignoring the foreign id
}

TEST_CASE("CustomCommandStore: an unknown kind is forward-tolerantly skipped on load") {
    removeStoreFile();
    // Hand-write a file with one valid and one unknown-kind entry; only the valid one should register.
    ofs::util::writeFile(ofs::util::getPrefPath() / "custom_commands.json", R"({
        "version": 1, "nextId": 9,
        "commands": [
            { "id": "custom.3", "name": "Good", "kind": "step", "direction": 1, "reps": 2, "granularity": "frame" },
            { "id": "custom.4", "name": "FromTheFuture", "kind": "warp-drive", "factor": 9 }
        ]
    })");

    TestProject tp;
    ofs::AppSettings settings;
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
    ofs::CommandRegistry registry{tp.eq};
    ofs::CustomCommandStore store{tp.eq, registry, templates};
    store.load();

    REQUIRE(store.commands().size() == 1);
    CHECK(store.commands()[0].id == "custom.3");
    CHECK(store.commands()[0].templateKey == "step");
    CHECK(registry.find("custom.3") != nullptr);
    CHECK(registry.find("custom.4") == nullptr);
}

TEST_CASE("CustomCommandTemplate: a Step command pushes StepRequestEvent with the authored params") {
    TestProject tp;
    ofs::AppSettings settings;
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
    EventCapture<ofs::StepRequestEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    const ofs::CustomCommandTemplate *t = templates.find("step");
    REQUIRE(t != nullptr);
    ofs::Command c = t->build(stepDef("Back 3 actions", -1, 3, ofs::StepGranularity::Action));
    REQUIRE(c.run);
    c.run(tp.eq);
    tp.eq.drain();

    REQUIRE(cap.received.size() == 1);
    CHECK(cap.received[0].direction == ofs::StepDirection::Backward);
    CHECK(cap.received[0].reps == 3); // base reps on a single press (burst == 1)
    CHECK(cap.received[0].granularity == ofs::StepGranularity::Action);
}

TEST_CASE("CustomCommandStore: step granularity round-trips through save and reload") {
    removeStoreFile();
    // ActionAllAxes and Action exercise both granularityToString and granularityFromString — the
    // symbolic-name (de)serialization the bindings.json rule mandates over the raw enum integer.
    {
        TestProject tp;
        ofs::AppSettings settings;
        ofs::CustomCommandTemplateRegistry templates;
        ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
        ofs::CommandRegistry registry{tp.eq};
        ofs::CustomCommandStore store{tp.eq, registry, templates};
        store.load();
        tp.eq.freeze();
        tp.eq.push(ofs::AddCustomCommandEvent{stepDef("AllAxes", -1, 4, ofs::StepGranularity::ActionAllAxes)});
        tp.eq.push(ofs::AddCustomCommandEvent{stepDef("PerAxis", 1, 2, ofs::StepGranularity::Action)});
        tp.eq.drain();
    }

    TestProject tp;
    ofs::AppSettings settings;
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
    ofs::CommandRegistry registry{tp.eq};
    ofs::CustomCommandStore store{tp.eq, registry, templates};
    store.load();

    REQUIRE(store.commands().size() == 2);
    CHECK(store.commands()[0].granularity == ofs::StepGranularity::ActionAllAxes);
    CHECK(store.commands()[0].direction == ofs::StepDirection::Backward);
    CHECK(store.commands()[0].reps == 4);
    CHECK(store.commands()[1].granularity == ofs::StepGranularity::Action);
}

TEST_CASE("CustomCommandStore: a present-but-unknown granularity skips just that entry") {
    removeStoreFile();
    // readParams returns false when "granularity" is present but unrecognized — the whole entry is
    // dropped (forward-tolerant), while a sibling with a valid granularity still loads.
    ofs::util::writeFile(ofs::util::getPrefPath() / "custom_commands.json", R"({
        "version": 1, "nextId": 5,
        "commands": [
            { "id": "custom.0", "name": "Good", "kind": "step", "direction": 1, "reps": 2, "granularity": "action" },
            { "id": "custom.1", "name": "Bad", "kind": "step", "direction": 1, "reps": 2, "granularity": "warp" }
        ]
    })");

    TestProject tp;
    ofs::AppSettings settings;
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
    ofs::CommandRegistry registry{tp.eq};
    ofs::CustomCommandStore store{tp.eq, registry, templates};
    store.load();

    REQUIRE(store.commands().size() == 1);
    CHECK(store.commands()[0].id == "custom.0");
    CHECK(store.commands()[0].granularity == ofs::StepGranularity::Action);
    CHECK(registry.find("custom.1") == nullptr);
}

TEST_CASE("CustomCommandTemplate: a held Step scales the authored reps by the frame's burst count") {
    TestProject tp;
    ofs::AppSettings settings;
    settings.holdRepeat = {
        .initialDelay = 0.0f, .interval = 0.01f, .accel = 1.0f, .maxRateHz = 250.0f}; // many fires per frame
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
    EventCapture<ofs::StepRequestEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    const ofs::CustomCommandTemplate *t = templates.find("step");
    REQUIRE(t != nullptr);
    ofs::Command c = t->build(stepDef("Burst", 1, 3));
    REQUIRE(c.tick);

    const ofs::HoldRepeatParams hp{.initialDelay = 0.0f, .interval = 0.01f, .accel = 1.0f, .maxRateHz = 250.0f};
    const int burst = ofs::holdRepeats(1.0f, 0.1f, hp); // ~10 catch-up fires over a 0.1s frame
    REQUIRE(burst > 1);
    c.tick(tp.eq, ofs::HoldTickInfo{.dt = 0.1f, .elapsed = 1.0f, .first = false});
    tp.eq.drain();

    REQUIRE(cap.received.size() == 1);
    CHECK(cap.received[0].reps == 3 * burst); // base reps × burst, not just the base
}

TEST_CASE("CustomCommandTemplate: a held Step inside the initial delay emits nothing") {
    TestProject tp;
    ofs::AppSettings settings; // default 0.40s initial delay
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
    EventCapture<ofs::StepRequestEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    ofs::Command c = templates.find("step")->build(stepDef("Burst", 1, 3));
    c.tick(tp.eq, ofs::HoldTickInfo{.dt = 0.016f, .elapsed = 0.10f, .first = false}); // still in the delay
    tp.eq.drain();

    CHECK(cap.received.empty()); // burst == 0 ⇒ no event pushed
}

TEST_CASE("CustomCommandTemplate: a MoveTime command shifts the selection in time with the authored params") {
    TestProject tp;
    ofs::AppSettings settings;
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
    tp.project.state.activeAxis = ofs::StandardAxis::L0;
    EventCapture<ofs::EditRequestEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    const ofs::CustomCommandTemplate *t = templates.find("move-time");
    REQUIRE(t != nullptr);
    ofs::Command c = t->build(ofs::CustomCommand{.name = "Back 2",
                                                 .templateKey = "move-time",
                                                 .direction = ofs::StepDirection::Backward,
                                                 .reps = 2,
                                                 .seekAfter = true});
    c.run(tp.eq); // single press
    tp.eq.drain();

    REQUIRE(cap.received.size() == 1);
    const auto &intent = cap.received[0].intent;
    CHECK(intent.kind == ofs::EditIntentKind::MoveSelection);
    CHECK(intent.axis == ofs::StandardAxis::L0);
    CHECK(intent.direction == ofs::StepDirection::Backward); // a time nudge (not a position nudge)
    CHECK(intent.reps == 2);
    CHECK(intent.seekAfter);
    CHECK(cap.received[0].gesture == ofs::GesturePhase::Begin);
}

TEST_CASE("CustomCommandTemplate: a MoveTime command is a no-op with no active axis") {
    TestProject tp;
    ofs::AppSettings settings;
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
    tp.project.state.activeAxis = ofs::StandardAxis::Count; // the build guard's bail-out path
    EventCapture<ofs::EditRequestEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    ofs::Command c = templates.find("move-time")
                         ->build(ofs::CustomCommand{
                             .name = "Shift", .templateKey = "move-time", .direction = ofs::StepDirection::Forward});
    c.run(tp.eq);
    tp.eq.drain();

    CHECK(cap.received.empty());
}

TEST_CASE("CustomCommandStore: move-time params round-trip through save and reload") {
    removeStoreFile();
    {
        TestProject tp;
        ofs::AppSettings settings;
        ofs::CustomCommandTemplateRegistry templates;
        ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
        ofs::CommandRegistry registry{tp.eq};
        ofs::CustomCommandStore store{tp.eq, registry, templates};
        store.load();
        tp.eq.freeze();
        tp.eq.push(ofs::AddCustomCommandEvent{ofs::CustomCommand{.name = "Shift",
                                                                 .templateKey = "move-time",
                                                                 .direction = ofs::StepDirection::Backward,
                                                                 .reps = 3,
                                                                 .seekAfter = true}});
        tp.eq.drain();
    }

    TestProject tp;
    ofs::AppSettings settings;
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
    ofs::CommandRegistry registry{tp.eq};
    ofs::CustomCommandStore store{tp.eq, registry, templates};
    store.load();

    REQUIRE(store.commands().size() == 1);
    CHECK(store.commands()[0].templateKey == "move-time");
    CHECK(store.commands()[0].direction == ofs::StepDirection::Backward);
    CHECK(store.commands()[0].reps == 3);
    CHECK(store.commands()[0].seekAfter);
}

TEST_CASE("CustomCommandTemplate: a MovePosition command is a no-op with no active axis") {
    TestProject tp;
    ofs::AppSettings settings;
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
    tp.project.state.activeAxis = ofs::StandardAxis::Count; // the build guard's bail-out path
    EventCapture<ofs::EditRequestEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    ofs::Command c = templates.find("move-position")
                         ->build(ofs::CustomCommand{.name = "Nudge", .templateKey = "move-position", .delta = 7});
    c.run(tp.eq);
    tp.eq.drain();

    CHECK(cap.received.empty()); // role >= Count ⇒ nothing pushed
}

TEST_CASE("CustomCommandTemplate: a held MovePosition scales delta and continues one undo step") {
    TestProject tp;
    ofs::AppSettings settings;
    settings.holdRepeat = {.initialDelay = 0.0f, .interval = 0.01f, .accel = 1.0f, .maxRateHz = 250.0f};
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
    tp.project.state.activeAxis = ofs::StandardAxis::L0;
    EventCapture<ofs::EditRequestEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    ofs::Command c = templates.find("move-position")
                         ->build(ofs::CustomCommand{.name = "Nudge", .templateKey = "move-position", .delta = 5});
    const ofs::HoldRepeatParams hp{.initialDelay = 0.0f, .interval = 0.01f, .accel = 1.0f, .maxRateHz = 250.0f};
    const int burst = ofs::holdRepeats(1.0f, 0.1f, hp);
    REQUIRE(burst > 1);
    c.tick(tp.eq, ofs::HoldTickInfo{.dt = 0.1f, .elapsed = 1.0f, .first = false}); // a later hold tick
    tp.eq.drain();

    REQUIRE(cap.received.size() == 1);
    CHECK(cap.received[0].intent.pos == 5 * burst);
    CHECK(cap.received[0].intent.direction == ofs::StepDirection::None); // delta nudge, never a time nudge
    CHECK(cap.received[0].gesture == ofs::GesturePhase::Continue);       // first==false folds into the open step
}

TEST_CASE("CustomCommandTemplate: a MovePosition command nudges the active axis selection") {
    TestProject tp;
    ofs::AppSettings settings;
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
    tp.project.state.activeAxis = ofs::StandardAxis::L0; // must be a real axis for the move to emit
    EventCapture<ofs::EditRequestEvent> cap;
    cap.attach(tp.eq);
    tp.eq.freeze();

    const ofs::CustomCommandTemplate *t = templates.find("move-position");
    REQUIRE(t != nullptr);
    ofs::Command c = t->build(ofs::CustomCommand{.name = "Nudge +7", .templateKey = "move-position", .delta = 7});
    c.run(tp.eq);
    tp.eq.drain();

    REQUIRE(cap.received.size() == 1);
    CHECK(cap.received[0].intent.kind == ofs::EditIntentKind::MoveSelection);
    CHECK(cap.received[0].intent.axis == ofs::StandardAxis::L0);
    CHECK(cap.received[0].intent.pos == 7);
    CHECK(cap.received[0].gesture == ofs::GesturePhase::Begin); // single press opens its own undo step
}
