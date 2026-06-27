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
#include <nlohmann/json.hpp>
#include <string>
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

// Params are an opaque per-template json bag now, so the builders write the same wire keys the templates
// read: `direction` as the ±1 enum int, `granularity` as the symbolic string, `reps`/`delta` ints,
// `seekAfter` bool.
const char *granName(ofs::StepGranularity g) {
    switch (g) {
    case ofs::StepGranularity::Action:
        return "action";
    case ofs::StepGranularity::ActionAllAxes:
        return "action-all-axes";
    case ofs::StepGranularity::Frame:
        break;
    }
    return "frame";
}
ofs::CustomCommand stepDef(const char *name, int dir, int reps, ofs::StepGranularity g = ofs::StepGranularity::Frame) {
    return {.name = name,
            .templateKey = "step",
            .params = {{"direction", dir}, {"reps", reps}, {"granularity", granName(g)}}};
}
ofs::CustomCommand movePosDef(const char *name, int delta) {
    return {.name = name, .templateKey = "move-position", .params = {{"delta", delta}}};
}
ofs::CustomCommand moveTimeDef(const char *name, ofs::StepDirection dir, int reps, bool seek) {
    return {.name = name,
            .templateKey = "move-time",
            .params = {{"direction", static_cast<int>(dir)}, {"reps", reps}, {"seekAfter", seek}}};
}

// Read a param off a stored definition's bag with a default.
int paramInt(const ofs::CustomCommand &c, const char *key) {
    return c.params.value(key, 0);
}
std::string paramStr(const ofs::CustomCommand &c, const char *key) {
    return c.params.value(key, std::string{});
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
    def.params["reps"] = 7;
    tp.eq.push(ofs::UpdateCustomCommandEvent{def});
    tp.eq.drain();

    REQUIRE(store.commands().size() == 1);
    CHECK(store.commands()[0].id == "custom.0");
    CHECK(store.commands()[0].name == "Jump 7");
    CHECK(paramInt(store.commands()[0], "reps") == 7);
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
        tp.eq.push(ofs::AddCustomCommandEvent{movePosDef("B", 7)});
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
    CHECK(paramInt(store.commands()[1], "delta") == 7);
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
    CHECK(paramStr(store.commands()[0], "granularity") == "action-all-axes");
    CHECK(paramInt(store.commands()[0], "direction") == static_cast<int>(ofs::StepDirection::Backward));
    CHECK(paramInt(store.commands()[0], "reps") == 4);
    CHECK(paramStr(store.commands()[1], "granularity") == "action");
}

TEST_CASE("CustomCommandStore: a present-but-unknown granularity is kept and degrades at build") {
    removeStoreFile();
    // The param bag is the wire format, so an unrecognized "granularity" is no longer dropped: the entry
    // loads, the value round-trips verbatim, and only the built Command falls back to the Frame default.
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

    REQUIRE(store.commands().size() == 2);
    CHECK(paramStr(store.commands()[0], "granularity") == "action");
    CHECK(paramStr(store.commands()[1], "granularity") == "warp"); // round-trips verbatim, not dropped
    CHECK(registry.find("custom.1") != nullptr);                   // it still registers
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
    ofs::Command c = t->build(moveTimeDef("Back 2", ofs::StepDirection::Backward, 2, true));
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

    ofs::Command c = templates.find("move-time")->build(moveTimeDef("Shift", ofs::StepDirection::Forward, 1, false));
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
        tp.eq.push(ofs::AddCustomCommandEvent{moveTimeDef("Shift", ofs::StepDirection::Backward, 3, true)});
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
    CHECK(paramInt(store.commands()[0], "direction") == static_cast<int>(ofs::StepDirection::Backward));
    CHECK(paramInt(store.commands()[0], "reps") == 3);
    CHECK(store.commands()[0].params.value("seekAfter", false));
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

    ofs::Command c = templates.find("move-position")->build(movePosDef("Nudge", 7));
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

    ofs::Command c = templates.find("move-position")->build(movePosDef("Nudge", 5));
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
    ofs::Command c = t->build(movePosDef("Nudge +7", 7));
    c.run(tp.eq);
    tp.eq.drain();

    REQUIRE(cap.received.size() == 1);
    CHECK(cap.received[0].intent.kind == ofs::EditIntentKind::MoveSelection);
    CHECK(cap.received[0].intent.axis == ofs::StandardAxis::L0);
    CHECK(cap.received[0].intent.pos == 7);
    CHECK(cap.received[0].gesture == ofs::GesturePhase::Begin); // single press opens its own undo step
}

// ── Auto-name + summary (the row title / dimmed subtitle the Shortcut window shows) ──────────────────

namespace {
// Resolve a built Command's display title (a TrString) to a plain string for comparison.
std::string cmdTitle(const ofs::Command &c) {
    return c.title.c_str();
}
} // namespace

TEST_CASE("CustomCommandTemplate: an unnamed command uses its summary as the title, a named one as the subtitle") {
    TestProject tp;
    ofs::AppSettings settings;
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);

    // One unnamed + one named definition per kind, identical params otherwise.
    struct KindCase {
        const char *key;
        ofs::CustomCommand unnamed, named;
    };
    const KindCase cases[] = {
        {"step", stepDef("", 1, 3), stepDef("My Step", 1, 3)},
        {"move-position", movePosDef("", 7), movePosDef("My Nudge", 7)},
        {"move-time", moveTimeDef("", ofs::StepDirection::Backward, 2, true),
         moveTimeDef("My Shift", ofs::StepDirection::Backward, 2, true)},
    };

    for (const auto &kc : cases) {
        CAPTURE(kc.key);
        const ofs::CustomCommandTemplate *t = templates.find(kc.key);
        REQUIRE(t != nullptr);
        const ofs::Command cu = t->build(kc.unnamed);
        const ofs::Command cn = t->build(kc.named);

        // Unnamed: the live summary IS the title; there is no dimmed subtitle.
        CHECK_FALSE(cmdTitle(cu).empty());
        CHECK(cu.subtitle.empty());
        // Named: the user name is the title and that same summary moves to the dimmed subtitle.
        CHECK(cmdTitle(cn) == kc.named.name);
        CHECK(cn.subtitle == cmdTitle(cu));
    }
}

TEST_CASE("CustomCommandTemplate: the summary reflects the params it describes") {
    TestProject tp;
    ofs::AppSettings settings;
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);

    const ofs::CustomCommandTemplate *step = templates.find("step");
    const ofs::CustomCommandTemplate *movePos = templates.find("move-position");
    const ofs::CustomCommandTemplate *moveTime = templates.find("move-time");
    REQUIRE(step != nullptr);
    REQUIRE(movePos != nullptr);
    REQUIRE(moveTime != nullptr);

    // Step: the count and each of the three granularities change the summary (granularityWord's branches).
    CHECK(cmdTitle(step->build(stepDef("", 1, 3))) != cmdTitle(step->build(stepDef("", 1, 9))));
    const std::string frame = cmdTitle(step->build(stepDef("", 1, 3, ofs::StepGranularity::Frame)));
    const std::string action = cmdTitle(step->build(stepDef("", 1, 3, ofs::StepGranularity::Action)));
    const std::string allAxes = cmdTitle(step->build(stepDef("", 1, 3, ofs::StepGranularity::ActionAllAxes)));
    CHECK(frame != action);
    CHECK(action != allAxes);
    CHECK(frame != allAxes);

    // Move-position: the signed amount is shown.
    CHECK(cmdTitle(movePos->build(movePosDef("", 7))) != cmdTitle(movePos->build(movePosDef("", -25))));

    // Move-time: direction (directionWord's two branches) and the appended "+ seek" suffix each change it.
    CHECK(cmdTitle(moveTime->build(moveTimeDef("", ofs::StepDirection::Forward, 2, false))) !=
          cmdTitle(moveTime->build(moveTimeDef("", ofs::StepDirection::Backward, 2, false))));
    const std::string noSeek = cmdTitle(moveTime->build(moveTimeDef("", ofs::StepDirection::Forward, 2, false)));
    const std::string seek = cmdTitle(moveTime->build(moveTimeDef("", ofs::StepDirection::Forward, 2, true)));
    CHECK(noSeek != seek);
    CHECK(seek.size() > noSeek.size()); // the suffix is appended, not substituted
}

TEST_CASE("CustomCommandStore: a command saved with no name persists and registers with its summary as title") {
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
        tp.eq.push(ofs::AddCustomCommandEvent{movePosDef("", 7)}); // blank name — the editor now allows it
        tp.eq.drain();

        REQUIRE(store.commands().size() == 1);
        CHECK(store.commands()[0].name.empty());
        const ofs::Command *c = registry.find("custom.0");
        REQUIRE(c != nullptr);
        CHECK_FALSE(std::string{c->title.c_str()}.empty()); // the summary stands in as the title
        CHECK(c->subtitle.empty());                         // unnamed ⇒ no subtitle
    }

    // The empty name survives a reload — it is not coerced to a placeholder on disk.
    TestProject tp;
    ofs::AppSettings settings;
    ofs::CustomCommandTemplateRegistry templates;
    ofs::registerBuiltinCommandTemplates(templates, tp.project, settings);
    ofs::CommandRegistry registry{tp.eq};
    ofs::CustomCommandStore store{tp.eq, registry, templates};
    store.load();

    REQUIRE(store.commands().size() == 1);
    CHECK(store.commands()[0].name.empty());
    CHECK(registry.find("custom.0") != nullptr);
    removeStoreFile();
}
