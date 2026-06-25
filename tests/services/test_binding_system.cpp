#include "Core/Events.h"
#include "Core/ProjectLifecycleEvents.h"
#include "Services/BindingEvents.h"
#include "Services/BindingSystem.h"
#include "Services/CommandRegistry.h"
#include "Util/FileUtil.h"
#include "Util/PathUtil.h"
#include "helpers/EventCapture.h"
#include "helpers/TestProject.h"
#include <algorithm>
#include <doctest/doctest.h>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <set>
#include <system_error>

using ofs::test::EventCapture;
using ofs::test::TestProject;

// Helpers shared across tests.
namespace {

// Sets up a CommandRegistry + BindingSystem for unit testing.
// BindingSystem registers its KeyDownEvent handler in its constructor (must happen before freeze()).
struct Fixture {
    TestProject tp;
    ofs::CommandRegistry registry{tp.eq};
    ofs::RebindState rebind;
    ofs::BindingSystem bs{tp.eq, registry, rebind};

    // Convenience: add a command and a default binding, then freeze.
    void add(const char *id, const char *group, const char *title, std::function<void(ofs::EventQueue &)> run,
             SDL_Keycode key = SDLK_UNKNOWN, SDL_Keymod mod = SDL_KMOD_NONE,
             ofs::ActivationMode mode = ofs::ActivationMode::Press) {
        registry.add({.id = id, .group = group, .title = title, .run = std::move(run)});
        if (key != SDLK_UNKNOWN)
            bs.addDefault(id, {.key = key, .modifiers = mod}, mode);
    }
};

} // namespace

// ── Dispatch ─────────────────────────────────────────────────────────────────

TEST_CASE("BindingSystem fires the bound command on exact key+modifier match") {
    Fixture f;
    EventCapture<ofs::SaveProjectEvent> cap;
    cap.attach(f.tp.eq);

    f.add(
        "core.save", "Core", "Save", [](ofs::EventQueue &eq) { eq.push(ofs::SaveProjectEvent{false}); }, SDLK_S,
        SDL_KMOD_CTRL);
    f.tp.eq.freeze();

    f.bs.onKeyDown({.key = SDLK_S, .modifiers = SDL_KMOD_CTRL});
    f.tp.eq.drain();

    CHECK(cap.received.size() == 1);
}

TEST_CASE("BindingSystem: Ctrl+S fires Save, not the bare-S Sync Timestamps sharing the key") {
    // core.save (Ctrl+S) and core.sync-timestamps (bare S) share the 'S' key. Pressing Ctrl+S must
    // dispatch Save only — the unmodified Sync binding must not fire. Regression for Ctrl+S landing on
    // Sync Timestamps instead of Save.
    Fixture f;
    EventCapture<ofs::SaveProjectEvent> saveCap;
    EventCapture<ofs::SeekEvent> syncCap;
    saveCap.attach(f.tp.eq);
    syncCap.attach(f.tp.eq);

    // Register Sync first so dispatch order can't be what saves us — exact modifier match must.
    f.add(
        "core.sync-timestamps", "Core", "Sync Timestamps", [](ofs::EventQueue &eq) { eq.push(ofs::SeekEvent{0.0}); },
        SDLK_S, SDL_KMOD_NONE);
    f.add(
        "core.save", "Core", "Save", [](ofs::EventQueue &eq) { eq.push(ofs::SaveProjectEvent{false}); }, SDLK_S,
        SDL_KMOD_CTRL);
    f.tp.eq.freeze();

    f.bs.onKeyDown({.key = SDLK_S, .modifiers = SDL_KMOD_CTRL});
    f.tp.eq.drain();

    CHECK(saveCap.received.size() == 1);
    CHECK(syncCap.received.empty());

    // And the reverse: bare S fires Sync, not Save.
    f.bs.onKeyDown({.key = SDLK_S, .modifiers = SDL_KMOD_NONE});
    f.tp.eq.drain();
    CHECK(syncCap.received.size() == 1);
    CHECK(saveCap.received.size() == 1);
}

TEST_CASE("BindingSystem does not fire when modifiers differ") {
    Fixture f;
    EventCapture<ofs::SaveProjectEvent> cap;
    cap.attach(f.tp.eq);

    f.add(
        "core.save", "Core", "Save", [](ofs::EventQueue &eq) { eq.push(ofs::SaveProjectEvent{false}); }, SDLK_S,
        SDL_KMOD_CTRL);
    f.tp.eq.freeze();

    f.bs.onKeyDown({.key = SDLK_S, .modifiers = SDL_KMOD_NONE}); // no Ctrl
    f.tp.eq.drain();

    CHECK(cap.received.empty());
}

TEST_CASE("BindingSystem ignores OS key-repeat events (repetition is the Hold tick's job)") {
    Fixture f;
    EventCapture<ofs::SaveProjectEvent> cap;
    cap.attach(f.tp.eq);

    f.add(
        "core.save", "Core", "Save", [](ofs::EventQueue &eq) { eq.push(ofs::SaveProjectEvent{false}); }, SDLK_S,
        SDL_KMOD_CTRL);
    f.tp.eq.freeze();

    // The initial (non-repeat) press fires once; the OS-repeat key-downs that follow are ignored.
    f.bs.onKeyDown({.key = SDLK_S, .modifiers = SDL_KMOD_CTRL, .repeat = false});
    f.bs.onKeyDown({.key = SDLK_S, .modifiers = SDL_KMOD_CTRL, .repeat = true});
    f.bs.onKeyDown({.key = SDLK_S, .modifiers = SDL_KMOD_CTRL, .repeat = true});
    f.tp.eq.drain();

    CHECK(cap.received.size() == 1);
}

TEST_CASE("BindingSystem normalizes modifiers so right-Ctrl fires a Ctrl binding") {
    Fixture f;
    EventCapture<ofs::SaveProjectEvent> cap;
    cap.attach(f.tp.eq);

    f.add(
        "core.save", "Core", "Save", [](ofs::EventQueue &eq) { eq.push(ofs::SaveProjectEvent{false}); }, SDLK_S,
        SDL_KMOD_CTRL);
    f.tp.eq.freeze();

    // SDL_KMOD_RCTRL should normalize to SDL_KMOD_CTRL.
    f.bs.onKeyDown({.key = SDLK_S, .modifiers = SDL_KMOD_RCTRL});
    f.tp.eq.drain();

    CHECK(cap.received.size() == 1);
}

// ── Rebind capture ─────────────────────────────────────────────────────────────

TEST_CASE("BindingSystem rebind capture ignores a lone modifier key, then captures the real chord") {
    Fixture f;
    f.tp.eq.freeze();

    f.rebind.targetCommandId = "core.save";
    f.rebind.capturing = true;

    // SDL delivers a modifier key-down as key=SDLK_L*ALT/SHIFT/etc. with the matching mod bit already
    // set. Capturing it verbatim yields nonsense like "Alt+Left Alt" — capture must keep waiting.
    struct Mod {
        SDL_Keycode key;
        SDL_Keymod mod;
    };
    for (const Mod m :
         {Mod{SDLK_LCTRL, SDL_KMOD_LCTRL}, Mod{SDLK_RCTRL, SDL_KMOD_RCTRL}, Mod{SDLK_LSHIFT, SDL_KMOD_LSHIFT},
          Mod{SDLK_RSHIFT, SDL_KMOD_RSHIFT}, Mod{SDLK_LALT, SDL_KMOD_LALT}, Mod{SDLK_RALT, SDL_KMOD_RALT},
          Mod{SDLK_LGUI, SDL_KMOD_LGUI}, Mod{SDLK_RGUI, SDL_KMOD_RGUI}}) {
        f.bs.onKeyDown({.key = m.key, .modifiers = m.mod});
        CHECK_FALSE(f.rebind.hasResult);
        CHECK(f.rebind.capturing);
    }

    // The real key arrives with Alt still held → captures Alt+A, not "Alt+Left Alt".
    f.bs.onKeyDown({.key = SDLK_A, .modifiers = SDL_KMOD_ALT});
    REQUIRE(f.rebind.hasResult);
    CHECK_FALSE(f.rebind.capturing);
    const auto *kc = std::get_if<ofs::KeyChord>(&f.rebind.captured);
    REQUIRE(kc != nullptr);
    CHECK(kc->key == SDLK_A);
    CHECK((kc->modifiers & SDL_KMOD_ALT) != 0);
}

// ── Many-to-one (Phase 1 acceptance test) ───────────────────────────────

TEST_CASE("BindingSystem: many bindings can map to one command") {
    Fixture f;
    EventCapture<ofs::UndoEvent> cap;
    cap.attach(f.tp.eq);

    f.registry.add({.id = "edit.undo", .group = "Edit", .title = "Undo", .run = [](ofs::EventQueue &eq) {
                        eq.push(ofs::UndoEvent{});
                    }});
    // Two different chords → same command.
    f.bs.addDefault("edit.undo", {.key = SDLK_Z, .modifiers = SDL_KMOD_CTRL});
    f.bs.addDefault("edit.undo", {.key = SDLK_Z, .modifiers = SDL_KMOD_CTRL | SDL_KMOD_ALT});
    f.tp.eq.freeze();

    f.bs.onKeyDown({.key = SDLK_Z, .modifiers = SDL_KMOD_CTRL});
    f.tp.eq.drain();
    f.bs.onKeyDown({.key = SDLK_Z, .modifiers = SDL_KMOD_CTRL | SDL_KMOD_ALT});
    f.tp.eq.drain();

    CHECK(cap.received.size() == 2);
}

// ── inRebindList=false gates only the default rebind listing, not dispatch ─────────
// A binding the user assigns to a command not offered by default
// (a provider, a window-opener) still fires. `inRebindList` is consulted only by the Shortcut window to
// decide what to list without being asked.

TEST_CASE("BindingSystem: a binding on a !inRebindList command still dispatches via keyboard") {
    Fixture f;
    int fireCount = 0;
    f.registry.add({.id = "internal.thing",
                    .group = "Internal",
                    .title = "Thing",
                    .inRebindList = false,
                    .run = [&fireCount](ofs::EventQueue &) { ++fireCount; }});
    // A trigger the user assigned to a default-unbindable command — dispatches normally.
    f.bs.addBinding(
        {.trigger = ofs::KeyChord{.key = SDLK_T, .modifiers = SDL_KMOD_NONE}, .commandId = "internal.thing"});
    f.tp.eq.freeze();

    f.bs.onKeyDown({.key = SDLK_T, .modifiers = SDL_KMOD_NONE});
    f.tp.eq.drain();

    CHECK(fireCount == 1);
}

// ── CommandRegistry::run (choke point) ───────────────────────────────────────

TEST_CASE("CommandRegistry::run invokes the command by id") {
    Fixture f;
    EventCapture<ofs::RedoEvent> cap;
    cap.attach(f.tp.eq);

    f.registry.add({.id = "edit.redo", .group = "Edit", .title = "Redo", .run = [](ofs::EventQueue &eq) {
                        eq.push(ofs::RedoEvent{});
                    }});
    f.tp.eq.freeze();

    f.registry.run("edit.redo");
    f.tp.eq.drain();

    CHECK(cap.received.size() == 1);
}

TEST_CASE("CommandRegistry::run is a no-op for unknown id") {
    Fixture f;
    f.tp.eq.freeze();
    // Should not throw or fire anything.
    f.registry.run("does.not.exist");
}

// ── removeByGroup ─────────────────────────────────────────────────────────────

TEST_CASE("CommandRegistry::removeByGroup removes only that group's commands") {
    Fixture f;
    f.registry.add({.id = "a.x", .group = "GroupA", .title = "X", .run = [](ofs::EventQueue &) {}});
    f.registry.add({.id = "b.y", .group = "GroupB", .title = "Y", .run = [](ofs::EventQueue &) {}});
    REQUIRE(f.registry.all().size() == 2);

    f.registry.removeByGroup("GroupA");

    REQUIRE(f.registry.all().size() == 1);
    CHECK(f.registry.all()[0].group == "GroupB");
}

// ── bindings.json round-trip ────────────────────────────────────────────

TEST_CASE("BindingSystem save/load bindings.json round-trip") {
    // Save a binding from one instance...
    {
        Fixture f;
        f.registry.add({.id = "core.save", .group = "Core", .title = "Save", .run = [](ofs::EventQueue &) {}});
        f.bs.addDefault("core.save", {.key = SDLK_S, .modifiers = SDL_KMOD_CTRL});
        f.tp.eq.freeze();
        f.bs.saveBindings();
    }

    // ...and load it into a fresh instance.
    TestProject tp2;
    ofs::CommandRegistry reg2(tp2.eq);
    reg2.add({.id = "core.save", .group = "Core", .title = "Save", .run = [](ofs::EventQueue &) {}});
    ofs::RebindState rebind2;
    ofs::BindingSystem bs2(tp2.eq, reg2, rebind2);
    bs2.loadBindings();
    tp2.eq.freeze();

    const ofs::Trigger *hint = bs2.findHint("core.save");
    REQUIRE(hint != nullptr);
    const auto *kc = std::get_if<ofs::KeyChord>(hint);
    REQUIRE(kc != nullptr);
    CHECK(kc->key == SDLK_S);
    CHECK((kc->modifiers & SDL_KMOD_CTRL) != 0);
}

TEST_CASE("BindingSystem persists the activation mode across save/load") {
    {
        Fixture f;
        f.registry.add({.id = "navigation.next-action",
                        .group = "Navigation",
                        .title = "Next Action",
                        .run = [](ofs::EventQueue &) {}});
        f.registry.add({.id = "core.save", .group = "Core", .title = "Save", .run = [](ofs::EventQueue &) {}});
        f.bs.addDefault("navigation.next-action", {.key = SDLK_UP, .modifiers = SDL_KMOD_NONE},
                        ofs::ActivationMode::Hold);
        f.bs.addDefault("core.save", {.key = SDLK_S, .modifiers = SDL_KMOD_CTRL}, ofs::ActivationMode::Press);
        f.tp.eq.freeze();
        f.bs.saveBindings();
    }

    TestProject tp2;
    ofs::CommandRegistry reg2(tp2.eq);
    reg2.add(
        {.id = "navigation.next-action", .group = "Navigation", .title = "Next Action", .run = [](ofs::EventQueue &) {
         }});
    reg2.add({.id = "core.save", .group = "Core", .title = "Save", .run = [](ofs::EventQueue &) {}});
    ofs::RebindState rebind2;
    ofs::BindingSystem bs2(tp2.eq, reg2, rebind2);
    bs2.loadBindings();
    tp2.eq.freeze();

    auto modeOf = [&bs2](const char *id) {
        for (const auto &b : bs2.bindings())
            if (b.commandId == id)
                return b.mode;
        return ofs::ActivationMode::Press;
    };
    CHECK(modeOf("navigation.next-action") == ofs::ActivationMode::Hold);
    CHECK(modeOf("core.save") == ofs::ActivationMode::Press);
}

TEST_CASE("BindingSystem::setMode changes the mode on a matching keyboard binding") {
    Fixture f;
    f.registry.add({.id = "core.save", .group = "Core", .title = "Save", .run = [](ofs::EventQueue &) {}});
    const ofs::KeyChord chord{.key = SDLK_S, .modifiers = SDL_KMOD_CTRL};
    f.bs.addBinding({.trigger = chord, .commandId = "core.save", .mode = ofs::ActivationMode::Press});
    f.tp.eq.freeze();

    f.bs.setMode(chord, "core.save", ofs::ActivationMode::Hold);

    REQUIRE(f.bs.bindings().size() == 1);
    CHECK(f.bs.bindings()[0].mode == ofs::ActivationMode::Hold);
}

// ── keyboardCaptured gate ─────────────────────────────────────────────────────

TEST_CASE("BindingSystem skips dispatch when keyboardCaptured is set") {
    Fixture f;
    EventCapture<ofs::SaveProjectEvent> cap;
    cap.attach(f.tp.eq);

    f.add(
        "core.save", "Core", "Save", [](ofs::EventQueue &eq) { eq.push(ofs::SaveProjectEvent{false}); }, SDLK_S,
        SDL_KMOD_CTRL);
    f.tp.eq.freeze();

    f.bs.onKeyDown({.key = SDLK_S, .modifiers = SDL_KMOD_CTRL, .keyboardCaptured = true});
    f.tp.eq.drain();

    CHECK(cap.received.empty());
}

// ── Phase 3: Gamepad dispatch ─────────────────────────────────────────────────

TEST_CASE("BindingSystem fires a command bound to a PadButton") {
    Fixture f;
    EventCapture<ofs::PlayPauseEvent> cap;
    cap.attach(f.tp.eq);

    f.registry.add(
        {.id = "player.play-pause", .group = "Player", .title = "Play/Pause", .run = [](ofs::EventQueue &eq) {
             eq.push(ofs::PlayPauseEvent{});
         }});
    f.bs.addBinding({.trigger = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_SOUTH}, .commandId = "player.play-pause"});
    f.tp.eq.freeze();

    f.bs.onGamepadButton({.button = SDL_GAMEPAD_BUTTON_SOUTH});
    f.tp.eq.drain();

    CHECK(cap.received.size() == 1);
}

TEST_CASE("BindingSystem does not fire when a different button is pressed") {
    Fixture f;
    EventCapture<ofs::PlayPauseEvent> cap;
    cap.attach(f.tp.eq);

    f.registry.add(
        {.id = "player.play-pause", .group = "Player", .title = "Play/Pause", .run = [](ofs::EventQueue &eq) {
             eq.push(ofs::PlayPauseEvent{});
         }});
    f.bs.addBinding({.trigger = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_SOUTH}, .commandId = "player.play-pause"});
    f.tp.eq.freeze();

    f.bs.onGamepadButton({.button = SDL_GAMEPAD_BUTTON_EAST});
    f.tp.eq.drain();

    CHECK(cap.received.empty());
}

TEST_CASE("BindingSystem: a binding on a !inRebindList command still dispatches via gamepad") {
    Fixture f;
    int fireCount = 0;
    f.registry.add({.id = "internal.thing",
                    .group = "Internal",
                    .title = "Thing",
                    .inRebindList = false,
                    .run = [&fireCount](ofs::EventQueue &) { ++fireCount; }});
    f.bs.addBinding({.trigger = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_EAST}, .commandId = "internal.thing"});
    f.tp.eq.freeze();

    f.bs.onGamepadButton({.button = SDL_GAMEPAD_BUTTON_EAST});
    f.tp.eq.drain();

    CHECK(fireCount == 1);
}

TEST_CASE("BindingSystem skips gamepad dispatch when gamepadCaptured is set") {
    Fixture f;
    EventCapture<ofs::PlayPauseEvent> cap;
    cap.attach(f.tp.eq);

    f.registry.add(
        {.id = "player.play-pause", .group = "Player", .title = "Play/Pause", .run = [](ofs::EventQueue &eq) {
             eq.push(ofs::PlayPauseEvent{});
         }});
    f.bs.addBinding({.trigger = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_SOUTH}, .commandId = "player.play-pause"});
    f.tp.eq.freeze();

    f.bs.onGamepadButton({.button = SDL_GAMEPAD_BUTTON_SOUTH, .gamepadCaptured = true});
    f.tp.eq.drain();

    CHECK(cap.received.empty());
}

TEST_CASE("BindingSystem save/load bindings.json round-trip with PadButton") {
    {
        Fixture f;
        f.registry.add(
            {.id = "player.play-pause", .group = "Player", .title = "Play/Pause", .run = [](ofs::EventQueue &) {}});
        f.bs.addBinding(
            {.trigger = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_SOUTH}, .commandId = "player.play-pause"});
        f.tp.eq.freeze();
        f.bs.saveBindings();
    }

    TestProject tp2;
    ofs::CommandRegistry reg2(tp2.eq);
    reg2.add({.id = "player.play-pause", .group = "Player", .title = "Play/Pause", .run = [](ofs::EventQueue &) {}});
    ofs::RebindState rebind2;
    ofs::BindingSystem bs2(tp2.eq, reg2, rebind2);
    bs2.loadBindings();
    tp2.eq.freeze();

    const ofs::Trigger *hint = bs2.findHint("player.play-pause");
    REQUIRE(hint != nullptr);
    const auto *pb = std::get_if<ofs::PadButton>(hint);
    REQUIRE(pb != nullptr);
    CHECK(pb->button == SDL_GAMEPAD_BUTTON_SOUTH);
}

TEST_CASE("BindingSystem save/load bindings.json round-trip with PadAxis (axis + dir)") {
    {
        Fixture f;
        f.registry.add({.id = "test.axis.hold-a",
                        .group = "Simulator",
                        .title = "Sway -",
                        .tick = [](ofs::EventQueue &, const ofs::HoldTickInfo &) {}});
        f.bs.addBinding({.trigger = ofs::PadAxis{.axis = SDL_GAMEPAD_AXIS_LEFTX, .positive = false},
                         .commandId = "test.axis.hold-a",
                         .mode = ofs::ActivationMode::Hold});
        f.tp.eq.freeze();
        f.bs.saveBindings();
    }

    TestProject tp2;
    ofs::CommandRegistry reg2(tp2.eq);
    reg2.add({.id = "test.axis.hold-a",
              .group = "Simulator",
              .title = "Sway -",
              .tick = [](ofs::EventQueue &, const ofs::HoldTickInfo &) {}});
    ofs::RebindState rebind2;
    ofs::BindingSystem bs2(tp2.eq, reg2, rebind2);
    bs2.loadBindings();
    tp2.eq.freeze();

    const ofs::Trigger *hint = bs2.findHint("test.axis.hold-a");
    REQUIRE(hint != nullptr);
    const auto *pa = std::get_if<ofs::PadAxis>(hint);
    REQUIRE(pa != nullptr);
    CHECK(pa->axis == SDL_GAMEPAD_AXIS_LEFTX);
    CHECK(pa->positive == false);
}

// ── Held bindings ────────────────────────────────────────────────────────

TEST_CASE("BindingSystem: a gamepad button Hold ticks while held and stops on button-up") {
    Fixture f;
    int runs = 0;
    int ticks = 0;
    f.registry.add({.id = "navigation.next-step",
                    .group = "Navigation",
                    .title = "Next Frame",
                    .run = [&runs](ofs::EventQueue &) { ++runs; },
                    .tick = [&ticks](ofs::EventQueue &, const ofs::HoldTickInfo &) { ++ticks; }});
    f.bs.addBinding({.trigger = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
                     .commandId = "navigation.next-step",
                     .mode = ofs::ActivationMode::Hold});
    f.tp.eq.freeze();

    f.bs.onGamepadButton({.button = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER});
    CHECK(runs == 0); // Hold start: no run() on press
    CHECK(ticks == 0);
    f.bs.tickHolds(0.016f);
    f.bs.tickHolds(0.016f);
    CHECK(ticks == 2);

    f.bs.onGamepadButtonUp({.button = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER});
    f.bs.tickHolds(0.016f);
    CHECK(ticks == 2); // released — no further ticks
    CHECK(runs == 0);
}

TEST_CASE("BindingSystem: an analog axis starts a hold past the deadzone and stops below it") {
    Fixture f;
    int firstCount = 0;
    float lastAnalog = -1.0f;
    f.registry.add({.id = "test.axis.hold-b",
                    .group = "Simulator",
                    .title = "Sway +",
                    .tick = [&](ofs::EventQueue &, const ofs::HoldTickInfo &info) {
                        if (info.first)
                            ++firstCount;
                        lastAnalog = info.analog;
                    }});
    f.bs.addBinding({.trigger = ofs::PadAxis{.axis = SDL_GAMEPAD_AXIS_LEFTX, .positive = true},
                     .commandId = "test.axis.hold-b",
                     .mode = ofs::ActivationMode::Hold});
    // smoothing 0 ⇒ the low-pass passes the raw value through in one frame so the test is deterministic.
    f.bs.setAnalogConfig({.deadzone = 0.15f, .smoothing = 0.0f});
    f.tp.eq.freeze();

    std::array<float, SDL_GAMEPAD_AXIS_COUNT> axes{};

    // Inside the deadzone: no hold starts.
    axes[SDL_GAMEPAD_AXIS_LEFTX] = 0.1f;
    f.bs.tickAnalog(axes, /*padCaptured=*/false, 0.016f);
    f.bs.tickHolds(0.016f);
    CHECK(firstCount == 0);

    // Past the deadzone: hold starts, first tick fires, analog is the scaled post-deadzone magnitude.
    axes[SDL_GAMEPAD_AXIS_LEFTX] = 0.8f;
    f.bs.tickAnalog(axes, false, 0.016f);
    f.bs.tickHolds(0.016f);
    CHECK(firstCount == 1);
    CHECK(lastAnalog == doctest::Approx((0.8f - 0.15f) / (1.0f - 0.15f)));

    // The negative half of the same axis must not fire a positive-half binding.
    axes[SDL_GAMEPAD_AXIS_LEFTX] = -0.8f;
    f.bs.tickAnalog(axes, false, 0.016f);
    f.bs.tickHolds(0.016f);
    CHECK(firstCount == 1); // hold ended (no new start)

    // Back to idle: no further ticks.
    int before = firstCount;
    axes[SDL_GAMEPAD_AXIS_LEFTX] = 0.0f;
    f.bs.tickAnalog(axes, false, 0.016f);
    f.bs.tickHolds(0.016f);
    CHECK(firstCount == before);
}

TEST_CASE("BindingSystem: gamepad-nav capture suppresses analog dispatch") {
    Fixture f;
    int ticks = 0;
    f.registry.add({.id = "test.axis.hold-b",
                    .group = "Simulator",
                    .title = "Sway +",
                    .tick = [&ticks](ofs::EventQueue &, const ofs::HoldTickInfo &) { ++ticks; }});
    f.bs.addBinding({.trigger = ofs::PadAxis{.axis = SDL_GAMEPAD_AXIS_LEFTX, .positive = true},
                     .commandId = "test.axis.hold-b",
                     .mode = ofs::ActivationMode::Hold});
    f.bs.setAnalogConfig({.deadzone = 0.15f, .smoothing = 0.0f});
    f.tp.eq.freeze();

    std::array<float, SDL_GAMEPAD_AXIS_COUNT> axes{};
    axes[SDL_GAMEPAD_AXIS_LEFTX] = 0.9f;
    f.bs.tickAnalog(axes, /*padCaptured=*/true, 0.016f);
    f.bs.tickHolds(0.016f);
    CHECK(ticks == 0);
}

// ── Modifiers (held-gate layer: gamepad only) ───────────────────────────────────

TEST_CASE("BindingSystem: a held analog modifier shadows the unmodified binding on the same axis") {
    Fixture f;
    int primaryTicks = 0;
    int altTicks = 0;
    f.registry.add({.id = "test.axis.primary",
                    .group = "Test",
                    .title = "Primary",
                    .tick = [&primaryTicks](ofs::EventQueue &, const ofs::HoldTickInfo &) { ++primaryTicks; }});
    f.registry.add({.id = "test.axis.alt",
                    .group = "Test",
                    .title = "Alt",
                    .tick = [&altTicks](ofs::EventQueue &, const ofs::HoldTickInfo &) { ++altTicks; }});
    // Same trigger (LEFTY-, i.e. push the stick up): the primary command is unmodified, the alt is gated
    // behind a held LEFT_TRIGGER. The modified binding must win while the trigger is held, the plain one
    // otherwise.
    f.bs.addBinding({.trigger = ofs::PadAxis{.axis = SDL_GAMEPAD_AXIS_LEFTY, .positive = false},
                     .commandId = "test.axis.primary",
                     .mode = ofs::ActivationMode::Hold});
    f.bs.addBinding({.trigger = ofs::PadAxis{.axis = SDL_GAMEPAD_AXIS_LEFTY, .positive = false},
                     .commandId = "test.axis.alt",
                     .modifier = ofs::PadAxis{.axis = SDL_GAMEPAD_AXIS_LEFT_TRIGGER, .positive = true},
                     .mode = ofs::ActivationMode::Hold});
    f.bs.setAnalogConfig({.deadzone = 0.15f, .smoothing = 0.0f});
    f.tp.eq.freeze();

    std::array<float, SDL_GAMEPAD_AXIS_COUNT> axes{};

    // Modifier idle: pushing the stick up drives the primary command only.
    axes[SDL_GAMEPAD_AXIS_LEFTY] = -0.8f;
    f.bs.tickAnalog(axes, false, 0.016f);
    f.bs.tickHolds(0.016f);
    CHECK(primaryTicks == 1);
    CHECK(altTicks == 0);
    CHECK_FALSE(f.bs.isModifierActiveFor("test.axis.alt"));

    // Hold the trigger modifier: the alt takes over, the primary is suppressed (its hold dropped).
    axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 0.9f;
    f.bs.tickAnalog(axes, false, 0.016f);
    f.bs.tickHolds(0.016f);
    CHECK(altTicks == 1);
    CHECK(primaryTicks == 1);
    CHECK(f.bs.isModifierActiveFor("test.axis.alt"));

    // Release the modifier: the primary resumes, the alt stops.
    axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 0.0f;
    f.bs.tickAnalog(axes, false, 0.016f);
    f.bs.tickHolds(0.016f);
    CHECK(primaryTicks == 2);
    CHECK(altTicks == 1);
}

TEST_CASE("BindingSystem: a PadButton modifier gates dispatch and reports its held state") {
    Fixture f;
    int runs = 0;
    f.registry.add(
        {.id = "test.button.cmd", .group = "Test", .title = "Cmd", .run = [&runs](ofs::EventQueue &) { ++runs; }});
    f.bs.addBinding({.trigger = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_SOUTH},
                     .commandId = "test.button.cmd",
                     .modifier = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}});
    f.tp.eq.freeze();

    // Modifier not held: the binding is inert.
    f.bs.onGamepadButton({.button = SDL_GAMEPAD_BUTTON_SOUTH});
    CHECK(runs == 0);
    CHECK_FALSE(f.bs.isModifierActiveFor("test.button.cmd"));

    // Hold the modifier button, then press the trigger: it fires, and the held state is reported.
    f.bs.onGamepadButton({.button = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER});
    CHECK(f.bs.isModifierActiveFor("test.button.cmd"));
    f.bs.onGamepadButton({.button = SDL_GAMEPAD_BUTTON_SOUTH});
    CHECK(runs == 1);

    // Release the modifier: inert again.
    f.bs.onGamepadButtonUp({.button = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER});
    CHECK_FALSE(f.bs.isModifierActiveFor("test.button.cmd"));
    f.bs.onGamepadButton({.button = SDL_GAMEPAD_BUTTON_SOUTH});
    CHECK(runs == 1);
}

TEST_CASE("BindingSystem save/load bindings.json round-trip preserves a binding modifier") {
    {
        Fixture f;
        f.registry.add({.id = "test.axis.alt",
                        .group = "Test",
                        .title = "Alt",
                        .tick = [](ofs::EventQueue &, const ofs::HoldTickInfo &) {}});
        f.bs.addBinding({.trigger = ofs::PadAxis{.axis = SDL_GAMEPAD_AXIS_LEFTY, .positive = false},
                         .commandId = "test.axis.alt",
                         .modifier = ofs::PadAxis{.axis = SDL_GAMEPAD_AXIS_LEFT_TRIGGER, .positive = true},
                         .mode = ofs::ActivationMode::Hold});
        f.tp.eq.freeze();
        f.bs.saveBindings();
    }

    TestProject tp2;
    ofs::CommandRegistry reg2(tp2.eq);
    reg2.add({.id = "test.axis.alt",
              .group = "Test",
              .title = "Alt",
              .tick = [](ofs::EventQueue &, const ofs::HoldTickInfo &) {}});
    ofs::RebindState rebind2;
    ofs::BindingSystem bs2(tp2.eq, reg2, rebind2);
    bs2.loadBindings();
    tp2.eq.freeze();

    REQUIRE(bs2.bindings().size() == 1);
    const ofs::Modifier &mod = bs2.bindings().front().modifier;
    const auto *pa = std::get_if<ofs::PadAxis>(&mod);
    REQUIRE(pa != nullptr);
    CHECK(pa->axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    CHECK(pa->positive == true);
}

TEST_CASE("BindingSystem load drops a keyboard (KeyMod) modifier from an older file, keeping the binding") {
    // The held-gate modifier layer is gamepad-only now. A bindings.json written by an older build may
    // carry a `"modifier": {"type":"key", ...}`; the loader must keep the binding but drop the modifier
    // rather than failing the whole entry.
    const std::string json = R"({
        "version": 1,
        "bindings": [
            {"command": "test.key.gated", "input": {"type": "key", "key": "Right"},
             "modifier": {"type": "key", "key": "L"}}
        ]
    })";
    const auto path = ofs::util::getPrefPath() / "bindings.json";
    std::error_code ec;
    std::filesystem::create_directories(ofs::util::getPrefPath(), ec);
    REQUIRE(ofs::util::writeFile(path, json));

    TestProject tp2;
    ofs::CommandRegistry reg2(tp2.eq);
    reg2.add({.id = "test.key.gated", .group = "Test", .title = "Gated", .run = [](ofs::EventQueue &) {}});
    ofs::RebindState rebind2;
    ofs::BindingSystem bs2(tp2.eq, reg2, rebind2);
    bs2.loadBindings();
    tp2.eq.freeze();

    REQUIRE(bs2.bindings().size() == 1);
    CHECK(std::holds_alternative<std::monostate>(bs2.bindings().front().modifier));
}

TEST_CASE("BindingSystem: a Hold binding starts no run on press, ticks while held, stops on key-up") {
    Fixture f;
    int runs = 0;
    int ticks = 0;
    f.registry.add({.id = "navigation.next-step",
                    .group = "Navigation",
                    .title = "Next Frame",
                    .run = [&runs](ofs::EventQueue &) { ++runs; },
                    .tick = [&ticks](ofs::EventQueue &, const ofs::HoldTickInfo &) { ++ticks; }});
    f.bs.addDefault("navigation.next-step", {.key = SDLK_RIGHT, .modifiers = SDL_KMOD_NONE}, ofs::ActivationMode::Hold);
    f.tp.eq.freeze();

    // Press starts the hold but neither runs nor ticks immediately (the first tick does the work).
    f.bs.onKeyDown({.key = SDLK_RIGHT, .modifiers = SDL_KMOD_NONE});
    CHECK(runs == 0);
    CHECK(ticks == 0);

    f.bs.tickHolds(0.016f);
    f.bs.tickHolds(0.016f);
    CHECK(ticks == 2);

    // Release ends the hold; no further ticks.
    f.bs.onKeyUp({.key = SDLK_RIGHT, .modifiers = SDL_KMOD_NONE});
    f.bs.tickHolds(0.016f);
    CHECK(ticks == 2);
    CHECK(runs == 0);
}

TEST_CASE("BindingSystem: a held key delivers info.first on exactly the first tick") {
    Fixture f;
    int total = 0;
    int firstCount = 0;
    f.registry.add({.id = "navigation.next-step",
                    .group = "Navigation",
                    .title = "Next Frame",
                    .tick = [&](ofs::EventQueue &, const ofs::HoldTickInfo &info) {
                        ++total;
                        if (info.first)
                            ++firstCount;
                    }});
    f.bs.addDefault("navigation.next-step", {.key = SDLK_RIGHT}, ofs::ActivationMode::Hold);
    f.tp.eq.freeze();

    f.bs.onKeyDown({.key = SDLK_RIGHT, .modifiers = SDL_KMOD_NONE});
    f.bs.tickHolds(0.016f);
    f.bs.tickHolds(0.016f);
    f.bs.tickHolds(0.016f);
    CHECK(total == 3);
    CHECK(firstCount == 1);
}

TEST_CASE("BindingSystem: release matches on key alone even if a modifier changed mid-hold") {
    Fixture f;
    int ticks = 0;
    f.registry.add({.id = "navigation.next-step",
                    .group = "Navigation",
                    .title = "Next Frame",
                    .tick = [&ticks](ofs::EventQueue &, const ofs::HoldTickInfo &) { ++ticks; }});
    f.bs.addDefault("navigation.next-step", {.key = SDLK_RIGHT}, ofs::ActivationMode::Hold);
    f.tp.eq.freeze();

    f.bs.onKeyDown({.key = SDLK_RIGHT, .modifiers = SDL_KMOD_NONE});
    f.bs.tickHolds(0.016f);
    // Key-up arrives with a modifier now held — must still end the hold (key-only match).
    f.bs.onKeyUp({.key = SDLK_RIGHT, .modifiers = SDL_KMOD_SHIFT});
    f.bs.tickHolds(0.016f);
    CHECK(ticks == 1);
}

TEST_CASE("BindingSystem: a Hold binding on a non-holdable command falls back to a single run") {
    Fixture f;
    EventCapture<ofs::SaveProjectEvent> cap;
    cap.attach(f.tp.eq);

    // No tick ⇒ not holdable. A Hold binding should still fire run() once on press.
    f.add(
        "core.save", "Core", "Save", [](ofs::EventQueue &eq) { eq.push(ofs::SaveProjectEvent{false}); }, SDLK_S,
        SDL_KMOD_CTRL, ofs::ActivationMode::Hold);
    f.tp.eq.freeze();

    f.bs.onKeyDown({.key = SDLK_S, .modifiers = SDL_KMOD_CTRL});
    f.bs.tickHolds(0.016f); // nothing to tick
    f.tp.eq.drain();

    CHECK(cap.received.size() == 1);
}

TEST_CASE("BindingSystem: clearHolds ends an in-flight hold (capture/focus loss)") {
    Fixture f;
    int ticks = 0;
    f.registry.add({.id = "navigation.next-step",
                    .group = "Navigation",
                    .title = "Next Frame",
                    .tick = [&ticks](ofs::EventQueue &, const ofs::HoldTickInfo &) { ++ticks; }});
    f.bs.addDefault("navigation.next-step", {.key = SDLK_RIGHT}, ofs::ActivationMode::Hold);
    f.tp.eq.freeze();

    f.bs.onKeyDown({.key = SDLK_RIGHT, .modifiers = SDL_KMOD_NONE});
    f.bs.tickHolds(0.016f);
    f.bs.clearHolds();
    f.bs.tickHolds(0.016f);
    CHECK(ticks == 1);
}

TEST_CASE("BindingSystem: a hold ends when its command becomes disabled mid-press") {
    Fixture f;
    int ticks = 0;
    bool enabled = true;
    f.registry.add({.id = "test.hold",
                    .group = "Test",
                    .title = "Hold",
                    .tick = [&ticks](ofs::EventQueue &, const ofs::HoldTickInfo &) { ++ticks; },
                    .isEnabled = [&enabled] { return enabled; }});
    f.bs.addDefault("test.hold", {.key = SDLK_RIGHT}, ofs::ActivationMode::Hold);
    f.tp.eq.freeze();

    f.bs.onKeyDown({.key = SDLK_RIGHT, .modifiers = SDL_KMOD_NONE});
    f.bs.tickHolds(0.016f);
    enabled = false; // context toggled off while the key is still down
    f.bs.tickHolds(0.016f);
    f.bs.tickHolds(0.016f);
    CHECK(ticks == 1);
}

// ── First-enabled-match dispatch ──────────────────────────────────────

TEST_CASE("BindingSystem dispatches the first *enabled* command sharing a trigger") {
    Fixture f;
    EventCapture<ofs::SeekEvent> seekCap;      // pushed by the context-gated command
    EventCapture<ofs::PlayPauseEvent> playCap; // pushed by the always-on command
    seekCap.attach(f.tp.eq);
    playCap.attach(f.tp.eq);

    bool firstEnabled = false;
    f.registry.add({.id = "a.gated",
                    .group = "A",
                    .title = "Gated",
                    .run = [](ofs::EventQueue &eq) { eq.push(ofs::SeekEvent{1.0}); },
                    .isEnabled = [&firstEnabled] { return firstEnabled; }});
    f.registry.add({.id = "b.always", .group = "B", .title = "Always", .run = [](ofs::EventQueue &eq) {
                        eq.push(ofs::PlayPauseEvent{});
                    }});
    // Both bound to the same trigger; the first-registered one is currently disabled.
    f.bs.addBinding({.trigger = ofs::KeyChord{.key = SDLK_G}, .commandId = "a.gated"});
    f.bs.addBinding({.trigger = ofs::KeyChord{.key = SDLK_G}, .commandId = "b.always"});
    f.tp.eq.freeze();

    // First is disabled ⇒ dispatch falls through to the second.
    f.bs.onKeyDown({.key = SDLK_G, .modifiers = SDL_KMOD_NONE});
    f.tp.eq.drain();
    CHECK(seekCap.received.empty());
    CHECK(playCap.received.size() == 1);

    // Enable the first ⇒ it now wins the shared trigger.
    firstEnabled = true;
    f.bs.onKeyDown({.key = SDLK_G, .modifiers = SDL_KMOD_NONE});
    f.tp.eq.drain();
    CHECK(seekCap.received.size() == 1);
    CHECK(playCap.received.size() == 1);
}

// ── holdRepeats cadence ─────────────────────────────────────────────────

TEST_CASE("holdRepeats fires once immediately on the first tick") {
    const ofs::HoldRepeatParams p{.initialDelay = 0.40f, .interval = 0.10f, .accel = 1.0f};
    // First tick: elapsed == dt, window (0, dt] plus the inclusive t=0 fire ⇒ exactly one.
    CHECK(ofs::holdRepeats(0.016f, 0.016f, p) == 1);
}

TEST_CASE("holdRepeats is silent during the initial delay, then fires") {
    const ofs::HoldRepeatParams p{.initialDelay = 0.40f, .interval = 0.10f, .accel = 1.0f};
    CHECK(ofs::holdRepeats(0.30f, 0.016f, p) == 0); // still inside initialDelay
    CHECK(ofs::holdRepeats(0.40f, 0.05f, p) == 1);  // window crosses initialDelay
    CHECK(ofs::holdRepeats(0.60f, 0.05f, p) == 1);  // window crosses initialDelay + interval*2
}

TEST_CASE("holdRepeats emits a catch-up burst when the interval is shorter than a frame") {
    const ofs::HoldRepeatParams p{.initialDelay = 0.0f, .interval = 0.01f, .accel = 1.0f};
    // Over a 0.1s frame past the delay, ~10 fires of a 0.01s interval should accumulate.
    const int n = ofs::holdRepeats(1.0f, 0.1f, p);
    CHECK(n >= 9);
    CHECK(n <= 11);
}

TEST_CASE("holdRepeats clamps a pathological interval to a bounded burst") {
    const ofs::HoldRepeatParams p{.initialDelay = 0.0f, .interval = 0.0f, .accel = 1.0f};
    // interval ≈ 0 must not emit an unbounded burst in one frame.
    CHECK(ofs::holdRepeats(1.0f, 1.0f, p) <= 32);
}

TEST_CASE("holdRepeats keeps firing through a long accelerating hold (regression)") {
    // Frame-step / move-actions params: accel<1 shrinks the gap to the floor. The old enumeration
    // overran its iteration cap once the gap floored and returned 0, stalling the hold after ~1.5s.
    const ofs::HoldRepeatParams p{.initialDelay = 0.40f, .interval = 0.06f, .accel = 0.92f};
    CHECK(ofs::holdRepeats(2.0f, 0.016f, p) > 0);
    CHECK(ofs::holdRepeats(5.0f, 0.016f, p) > 0);
    CHECK(ofs::holdRepeats(30.0f, 0.016f, p) > 0);
    CHECK(ofs::holdRepeats(5.0f, 0.016f, p) <= 32); // still bounded
}

// ── Binding presets ───────────────────────────────────────────────────────

namespace {
// User presets live under getPrefPath()/binding_presets — a temp subdir for test binaries.
std::filesystem::path presetDir() {
    return ofs::util::getPrefPath() / "binding_presets";
}
void wipePresets() {
    std::error_code ec;
    std::filesystem::remove_all(presetDir(), ec);
}
} // namespace

TEST_CASE("BindingSystem: saveActiveAsPreset / listPresets / loadPreset round-trip") {
    wipePresets();

    // Save the active set out to a user preset.
    {
        Fixture f;
        f.registry.add({.id = "core.save", .group = "Core", .title = "Save", .run = [](ofs::EventQueue &) {}});
        f.bs.addBinding(
            {.trigger = ofs::KeyChord{.key = SDLK_S, .modifiers = SDL_KMOD_CTRL}, .commandId = "core.save"});
        f.tp.eq.freeze();
        f.bs.saveActiveAsPreset("My Preset");
    }

    // listPresets sees it, with a filename-safe slug and the display name preserved.
    {
        Fixture f;
        f.tp.eq.freeze();
        // listPresets unions the user dir with the shipped built-ins (e.g. "Simulator Posing"), so find
        // our entry by slug rather than assuming it is the only one.
        const auto presets = f.bs.listPresets();
        auto it = std::ranges::find_if(presets, [](const ofs::PresetInfo &p) { return p.slug == "my-preset"; });
        REQUIRE(it != presets.end());
        CHECK(it->name == "My Preset");
        CHECK(it->builtin == false);
    }

    // loadPreset applies it into a fresh instance's active set.
    {
        Fixture f;
        f.registry.add({.id = "core.save", .group = "Core", .title = "Save", .run = [](ofs::EventQueue &) {}});
        f.tp.eq.freeze();
        f.bs.loadPreset("my-preset");
        const ofs::Trigger *hint = f.bs.findHint("core.save");
        REQUIRE(hint != nullptr);
        const auto *kc = std::get_if<ofs::KeyChord>(hint);
        REQUIRE(kc != nullptr);
        CHECK(kc->key == SDLK_S);
        CHECK((kc->modifiers & SDL_KMOD_CTRL) != 0);
    }

    wipePresets();
}

TEST_CASE("BindingSystem: a preset preserves entries it can't bind across load + save") {
    wipePresets();
    std::error_code ec;
    std::filesystem::create_directories(presetDir(), ec);

    // Author a preset as if from a newer build: one entry this build can bind, plus a command it lacks
    // and an unknown trigger type — both must survive an edit-and-resave rather than being stripped.
    const char *json = R"({
      "version": 1, "name": "Future",
      "bindings": [
        {"command": "core.save", "input": {"type": "key", "key": "S", "mods": ["ctrl"]}},
        {"command": "future.command", "input": {"type": "key", "key": "X"}},
        {"command": "weird.thing", "input": {"type": "mouse", "button": "left"}}
      ]
    })";
    ofs::util::writeFile(presetDir() / ofs::util::fromUtf8("future.json"), json);

    Fixture f;
    f.registry.add({.id = "core.save", .group = "Core", .title = "Save", .run = [](ofs::EventQueue &) {}});
    f.tp.eq.freeze();

    const int unapplied = f.bs.loadPreset("future");
    // Only the known command is bound for dispatch; the two unknown entries are skipped — and reported so
    // the UI can warn the user instead of failing silently.
    CHECK(unapplied == 2);
    CHECK(f.bs.findHint("core.save") != nullptr);
    CHECK(f.bs.findHint("future.command") == nullptr);

    // Re-save under a new name; the unbindable entries must round-trip verbatim.
    f.bs.saveActiveAsPreset("Future 2");
    auto text = ofs::util::readFile(presetDir() / ofs::util::fromUtf8("future-2.json"));
    REQUIRE(text.has_value());
    const auto saved = nlohmann::json::parse(*text);
    std::set<std::string> commands;
    for (const auto &e : saved["bindings"])
        commands.insert(e["command"].get<std::string>());
    CHECK(commands.count("core.save") == 1);
    CHECK(commands.count("future.command") == 1);
    CHECK(commands.count("weird.thing") == 1);

    wipePresets();
}

TEST_CASE("BindingSystem: deletePreset removes a user preset") {
    wipePresets();

    Fixture f;
    f.registry.add({.id = "core.save", .group = "Core", .title = "Save", .run = [](ofs::EventQueue &) {}});
    f.bs.addBinding({.trigger = ofs::KeyChord{.key = SDLK_S}, .commandId = "core.save"});
    f.tp.eq.freeze();
    f.bs.saveActiveAsPreset("Temp");
    auto hasTemp = [&] {
        const auto ps = f.bs.listPresets();
        return std::ranges::any_of(ps, [](const ofs::PresetInfo &p) { return p.slug == "temp"; });
    };
    REQUIRE(hasTemp()); // present alongside the shipped built-ins

    f.bs.deletePreset("temp");
    CHECK_FALSE(hasTemp());

    wipePresets();
}

TEST_CASE("BindingSystem::saveActiveAsPreset slugifies separators and trims trailing punctuation") {
    wipePresets();
    Fixture f;
    f.registry.add({.id = "core.save", .group = "Core", .title = "Save", .run = [](ofs::EventQueue &) {}});
    f.bs.addBinding({.trigger = ofs::KeyChord{.key = SDLK_S}, .commandId = "core.save"});
    f.tp.eq.freeze();

    // '_' and '-' are kept verbatim; a trailing run of other punctuation collapses to a '-' that is then
    // trimmed, so "My_Cool-Preset!!" → "my_cool-preset".
    f.bs.saveActiveAsPreset("My_Cool-Preset!!");
    const auto ps = f.bs.listPresets();
    CHECK(std::ranges::any_of(ps, [](const ofs::PresetInfo &p) { return p.slug == "my_cool-preset"; }));

    wipePresets();
}

// ── JSON serialization edge cases (via save/load round-trips) ────────────────

TEST_CASE("BindingSystem round-trips a GUI (Super/Win) keyboard modifier") {
    {
        Fixture f;
        f.registry.add({.id = "core.cmd", .group = "Core", .title = "Cmd", .run = [](ofs::EventQueue &) {}});
        f.bs.addDefault("core.cmd", {.key = SDLK_K, .modifiers = SDL_KMOD_GUI});
        f.tp.eq.freeze();
        f.bs.saveBindings();
    }

    TestProject tp2;
    ofs::CommandRegistry reg2(tp2.eq);
    reg2.add({.id = "core.cmd", .group = "Core", .title = "Cmd", .run = [](ofs::EventQueue &) {}});
    ofs::RebindState rebind2;
    ofs::BindingSystem bs2(tp2.eq, reg2, rebind2);
    bs2.loadBindings();
    tp2.eq.freeze();

    const ofs::Trigger *hint = bs2.findHint("core.cmd");
    REQUIRE(hint != nullptr);
    const auto *kc = std::get_if<ofs::KeyChord>(hint);
    REQUIRE(kc != nullptr);
    CHECK((kc->modifiers & SDL_KMOD_GUI) != 0);
}

TEST_CASE("BindingSystem round-trips a PadButton held modifier") {
    {
        Fixture f;
        f.registry.add({.id = "test.button.cmd", .group = "Test", .title = "Cmd", .run = [](ofs::EventQueue &) {}});
        f.bs.addBinding({.trigger = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_SOUTH},
                         .commandId = "test.button.cmd",
                         .modifier = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}});
        f.tp.eq.freeze();
        f.bs.saveBindings();
    }

    TestProject tp2;
    ofs::CommandRegistry reg2(tp2.eq);
    reg2.add({.id = "test.button.cmd", .group = "Test", .title = "Cmd", .run = [](ofs::EventQueue &) {}});
    ofs::RebindState rebind2;
    ofs::BindingSystem bs2(tp2.eq, reg2, rebind2);
    bs2.loadBindings();
    tp2.eq.freeze();

    REQUIRE(bs2.bindings().size() == 1);
    const auto *pb = std::get_if<ofs::PadButton>(&bs2.bindings().front().modifier);
    REQUIRE(pb != nullptr);
    CHECK(pb->button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
}

TEST_CASE("BindingSystem::saveBindings drops invalid triggers (they serialize to nothing)") {
    Fixture f;
    f.registry.add({.id = "a.unknown", .group = "A", .title = "U", .run = [](ofs::EventQueue &) {}});
    f.registry.add({.id = "b.badpad", .group = "B", .title = "P", .run = [](ofs::EventQueue &) {}});
    f.registry.add({.id = "c.badaxis", .group = "C", .title = "X", .run = [](ofs::EventQueue &) {}});
    f.registry.add({.id = "d.ok", .group = "D", .title = "OK", .run = [](ofs::EventQueue &) {}});
    // An unbound KeyChord, an invalid pad button, and an invalid axis each serialize to no entry; only
    // the valid binding survives the round-trip.
    f.bs.addBinding({.trigger = ofs::KeyChord{.key = SDLK_UNKNOWN}, .commandId = "a.unknown"});
    f.bs.addBinding({.trigger = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_INVALID}, .commandId = "b.badpad"});
    f.bs.addBinding({.trigger = ofs::PadAxis{.axis = SDL_GAMEPAD_AXIS_INVALID}, .commandId = "c.badaxis"});
    f.bs.addBinding({.trigger = ofs::KeyChord{.key = SDLK_S, .modifiers = SDL_KMOD_CTRL}, .commandId = "d.ok"});
    f.tp.eq.freeze();
    f.bs.saveBindings();

    TestProject tp2;
    ofs::CommandRegistry reg2(tp2.eq);
    for (const char *id : {"a.unknown", "b.badpad", "c.badaxis", "d.ok"})
        reg2.add({.id = id, .group = "G", .title = "T", .run = [](ofs::EventQueue &) {}});
    ofs::RebindState rebind2;
    ofs::BindingSystem bs2(tp2.eq, reg2, rebind2);
    bs2.loadBindings();
    tp2.eq.freeze();

    REQUIRE(bs2.bindings().size() == 1);
    CHECK(bs2.bindings().front().commandId == "d.ok");
}

// ── loadBindings fallbacks & forward-tolerant parsing ────────────────────────

namespace {
void writeBindingsFile(const std::string &json) {
    const auto path = ofs::util::getPrefPath() / "bindings.json";
    std::error_code ec;
    std::filesystem::create_directories(ofs::util::getPrefPath(), ec);
    REQUIRE(ofs::util::writeFile(path, json));
}
void removeBindingsFile() {
    std::error_code ec;
    std::filesystem::remove(ofs::util::getPrefPath() / "bindings.json", ec);
}
} // namespace

TEST_CASE("BindingSystem::loadBindings falls back to defaults on malformed JSON") {
    writeBindingsFile("{ this is not valid json ]");

    TestProject tp2;
    ofs::CommandRegistry reg2(tp2.eq);
    reg2.add({.id = "core.save", .group = "Core", .title = "Save", .run = [](ofs::EventQueue &) {}});
    ofs::RebindState rebind2;
    ofs::BindingSystem bs2(tp2.eq, reg2, rebind2);
    bs2.addDefault("core.save", {.key = SDLK_S, .modifiers = SDL_KMOD_CTRL});
    bs2.loadBindings(); // parse throws → catch restores code defaults
    tp2.eq.freeze();

    CHECK(bs2.findHint("core.save") != nullptr);
    removeBindingsFile();
}

TEST_CASE("BindingSystem::loadBindings falls back to defaults when version/bindings are absent") {
    writeBindingsFile(R"({"unrelated": true})");

    TestProject tp2;
    ofs::CommandRegistry reg2(tp2.eq);
    reg2.add({.id = "core.save", .group = "Core", .title = "Save", .run = [](ofs::EventQueue &) {}});
    ofs::RebindState rebind2;
    ofs::BindingSystem bs2(tp2.eq, reg2, rebind2);
    bs2.addDefault("core.save", {.key = SDLK_S, .modifiers = SDL_KMOD_CTRL});
    bs2.loadBindings();
    tp2.eq.freeze();

    CHECK(bs2.findHint("core.save") != nullptr); // defaults, not an empty set
    removeBindingsFile();
}

TEST_CASE("BindingSystem::loadBindings skips malformed entries, unknown trigger types, and unknown commands") {
    // Every entry but the last is dropped: missing command, missing input, missing/invalid trigger fields
    // for each type, an unknown trigger type, and a known type bound to a command this build lacks. The
    // loader must skip them all and still bind the one valid entry.
    writeBindingsFile(R"({
        "version": 1,
        "bindings": [
            {"input": {"type": "key", "key": "S"}},
            {"command": "x.nokey", "input": {"type": "key"}},
            {"command": "x.badkey", "input": {"type": "key", "key": "NotARealKey"}},
            {"command": "x.notype", "input": {"foo": "bar"}},
            {"command": "x.nobutton", "input": {"type": "pad"}},
            {"command": "x.badbutton", "input": {"type": "pad", "button": "nonsense"}},
            {"command": "x.noaxis", "input": {"type": "axis"}},
            {"command": "x.badaxis", "input": {"type": "axis", "axis": "nonsense"}},
            {"command": "x.weird", "input": {"type": "mouse", "button": "left"}},
            {"command": "ghost.command", "input": {"type": "key", "key": "G"}},
            {"command": "core.save", "input": {"type": "key", "key": "S", "mods": ["ctrl"]}}
        ]
    })");

    TestProject tp2;
    ofs::CommandRegistry reg2(tp2.eq);
    reg2.add({.id = "core.save", .group = "Core", .title = "Save", .run = [](ofs::EventQueue &) {}});
    ofs::RebindState rebind2;
    ofs::BindingSystem bs2(tp2.eq, reg2, rebind2);
    bs2.loadBindings();
    tp2.eq.freeze();

    REQUIRE(bs2.bindings().size() == 1); // only the valid, registered command is applied
    CHECK(bs2.bindings().front().commandId == "core.save");
    CHECK(bs2.findHint("ghost.command") == nullptr); // unknown command not bound for dispatch
    removeBindingsFile();
}

// ── setModifier ──────────────────────────────────────────────────────────────

TEST_CASE("BindingSystem::setModifier sets the modifier on a match and is a no-op otherwise") {
    Fixture f;
    f.registry.add({.id = "t.cmd", .group = "T", .title = "C", .run = [](ofs::EventQueue &) {}});
    const ofs::Trigger trig = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_SOUTH};
    f.bs.addBinding({.trigger = trig, .commandId = "t.cmd"});
    f.tp.eq.freeze();

    // No matching (trigger, command) pair → the loop completes without assigning.
    f.bs.setModifier(ofs::KeyChord{.key = SDLK_Q}, "t.cmd", ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_NORTH});
    CHECK(std::holds_alternative<std::monostate>(f.bs.bindings().front().modifier));

    // Exact match → modifier assigned.
    f.bs.setModifier(trig, "t.cmd", ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER});
    const auto *pb = std::get_if<ofs::PadButton>(&f.bs.bindings().front().modifier);
    REQUIRE(pb != nullptr);
    CHECK(pb->button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
}

// ── Rebind capture: Escape cancels ───────────────────────────────────────────

TEST_CASE("BindingSystem: Escape cancels rebind capture without producing a result") {
    Fixture f;
    f.tp.eq.freeze();

    f.rebind.targetCommandId = "core.save";
    f.rebind.capturing = true;
    f.rebind.captureModifier = true;

    f.bs.onKeyDown({.key = SDLK_ESCAPE, .modifiers = SDL_KMOD_NONE});

    CHECK_FALSE(f.rebind.capturing);
    CHECK_FALSE(f.rebind.hasResult);
    CHECK(f.rebind.targetCommandId.empty());
    CHECK_FALSE(f.rebind.captureModifier);
}

// ── addDefault / dispatch ignore an unbound (SDLK_UNKNOWN) chord ──────────────

TEST_CASE("BindingSystem::addDefault ignores an unbound (SDLK_UNKNOWN) chord") {
    Fixture f;
    f.registry.add({.id = "core.x", .group = "Core", .title = "X", .run = [](ofs::EventQueue &) {}});
    f.bs.addDefault("core.x", {.key = SDLK_UNKNOWN, .modifiers = SDL_KMOD_NONE});
    f.tp.eq.freeze();
    CHECK(f.bs.bindings().empty()); // nothing registered for an unbound default
}

TEST_CASE("BindingSystem dispatch skips a binding whose chord is unbound (SDLK_UNKNOWN)") {
    Fixture f;
    int runs = 0;
    f.registry.add({.id = "core.x", .group = "Core", .title = "X", .run = [&runs](ofs::EventQueue &) { ++runs; }});
    // addBinding (unlike addDefault) keeps an UNKNOWN chord; dispatch must skip it via the key guard.
    f.bs.addBinding({.trigger = ofs::KeyChord{.key = SDLK_UNKNOWN}, .commandId = "core.x"});
    f.tp.eq.freeze();

    f.bs.onKeyDown({.key = SDLK_UNKNOWN, .modifiers = SDL_KMOD_NONE});
    f.tp.eq.drain();
    CHECK(runs == 0);
}

// ── findHint: PadButton hint + null for unbound command ───────────────────────

TEST_CASE("BindingSystem::findHint returns a pad-button trigger and null for an unbound command") {
    Fixture f;
    f.registry.add({.id = "p.cmd", .group = "P", .title = "C", .run = [](ofs::EventQueue &) {}});
    f.bs.addBinding({.trigger = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_NORTH}, .commandId = "p.cmd"});
    f.tp.eq.freeze();

    CHECK(f.bs.findHint("not.bound") == nullptr);
    const ofs::Trigger *hint = f.bs.findHint("p.cmd");
    REQUIRE(hint != nullptr);
    const auto *pb = std::get_if<ofs::PadButton>(hint);
    REQUIRE(pb != nullptr);
    CHECK(pb->button == SDL_GAMEPAD_BUTTON_NORTH);
}

// ── Modifier held-state edge cases ────────────────────────────────────────────

TEST_CASE("BindingSystem: an invalid-axis modifier never reports as held") {
    Fixture f;
    f.registry.add({.id = "t.cmd", .group = "T", .title = "C", .run = [](ofs::EventQueue &) {}});
    f.bs.addBinding({.trigger = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_SOUTH},
                     .commandId = "t.cmd",
                     .modifier = ofs::PadAxis{.axis = SDL_GAMEPAD_AXIS_INVALID, .positive = true}});
    f.tp.eq.freeze();
    CHECK_FALSE(f.bs.isModifierActiveFor("t.cmd"));
}

TEST_CASE("BindingSystem: a disabled modified binding does not shadow the unmodified one") {
    Fixture f;
    int plainRuns = 0;
    bool altEnabled = false;
    f.registry.add(
        {.id = "t.plain", .group = "T", .title = "Plain", .run = [&plainRuns](ofs::EventQueue &) { ++plainRuns; }});
    f.registry.add({.id = "t.alt",
                    .group = "T",
                    .title = "Alt",
                    .run = [](ofs::EventQueue &) {},
                    .isEnabled = [&altEnabled] { return altEnabled; }});
    // Both bound to SOUTH; t.alt requires a held LEFT_SHOULDER modifier but is currently disabled.
    f.bs.addBinding({.trigger = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_SOUTH}, .commandId = "t.plain"});
    f.bs.addBinding({.trigger = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_SOUTH},
                     .commandId = "t.alt",
                     .modifier = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}});
    f.tp.eq.freeze();

    // Hold the modifier, then press SOUTH. The modified layer's command is disabled, so it does not shadow
    // the plain binding — the plain command fires.
    f.bs.onGamepadButton({.button = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER});
    f.bs.onGamepadButton({.button = SDL_GAMEPAD_BUTTON_SOUTH});
    f.tp.eq.drain();
    CHECK(plainRuns == 1);
}

// ── Presets: listing & loading edge cases ─────────────────────────────────────

TEST_CASE("BindingSystem::listPresets skips non-json files and lists an unparseable preset under its slug") {
    wipePresets();
    std::error_code ec;
    std::filesystem::create_directories(presetDir(), ec);
    ofs::util::writeFile(presetDir() / ofs::util::fromUtf8("notes.txt"), "not a preset");
    ofs::util::writeFile(presetDir() / ofs::util::fromUtf8("broken.json"), "{ this is broken");

    Fixture f;
    f.tp.eq.freeze();
    const auto ps = f.bs.listPresets();

    auto broken = std::ranges::find_if(ps, [](const ofs::PresetInfo &p) { return p.slug == "broken"; });
    REQUIRE(broken != ps.end());
    CHECK(broken->name == "broken"); // unparseable → name falls back to slug
    CHECK(std::ranges::none_of(ps, [](const ofs::PresetInfo &p) { return p.slug == "notes"; }));

    wipePresets();
}

TEST_CASE("BindingSystem::loadPreset is a no-op for a missing, empty, or malformed preset") {
    wipePresets();
    std::error_code ec;
    std::filesystem::create_directories(presetDir(), ec);

    Fixture f;
    f.registry.add({.id = "core.save", .group = "Core", .title = "Save", .run = [](ofs::EventQueue &) {}});
    f.bs.addBinding({.trigger = ofs::KeyChord{.key = SDLK_S, .modifiers = SDL_KMOD_CTRL}, .commandId = "core.save"});
    f.tp.eq.freeze();

    // Missing file: error + return, active set untouched.
    f.bs.loadPreset("does-not-exist");
    CHECK(f.bs.findHint("core.save") != nullptr);

    // Present but no "bindings": returns before clearing the active set.
    ofs::util::writeFile(presetDir() / ofs::util::fromUtf8("empty.json"), R"({"version":1,"name":"Empty"})");
    f.bs.loadPreset("empty");
    CHECK(f.bs.findHint("core.save") != nullptr);

    // Malformed JSON: parse throws → caught, active set untouched.
    ofs::util::writeFile(presetDir() / ofs::util::fromUtf8("broken.json"), "{ not json");
    f.bs.loadPreset("broken");
    CHECK(f.bs.findHint("core.save") != nullptr);

    wipePresets();
}

// ── holdRepeats: accelerating (accel > 1) schedule ────────────────────────────

TEST_CASE("holdRepeats handles an accelerating (accel>1) schedule") {
    // accel>1 takes the growing-geometric closed form; it must still produce bounded, monotone fires.
    const ofs::HoldRepeatParams p{.initialDelay = 0.0f, .interval = 0.1f, .accel = 1.5f};
    CHECK(ofs::holdRepeats(0.5f, 0.05f, p) >= 1);
    CHECK(ofs::holdRepeats(2.0f, 0.05f, p) <= 16); // clamped to the per-frame burst ceiling
}

TEST_CASE("holdRepeats fires in the pre-floor geometric regime of a shrinking schedule") {
    // accel<1 with a small budget stays in the geometric regime (before the gap floors to kFloor),
    // exercising the closed-form branch distinct from the long-hold (floored) path.
    const ofs::HoldRepeatParams p{.initialDelay = 0.40f, .interval = 0.06f, .accel = 0.9f};
    CHECK(ofs::holdRepeats(0.45f, 0.05f, p) >= 1); // window crosses the first post-delay fire
    CHECK(ofs::holdRepeats(0.50f, 0.02f, p) >= 0);
}

// ── Hold dedup & gamepad rebind/interceptor ──────────────────────────────────

TEST_CASE("BindingSystem: a duplicate key-down for an already-held key does not double-start the hold") {
    Fixture f;
    int ticks = 0;
    f.registry.add({.id = "nav.step",
                    .group = "Nav",
                    .title = "Step",
                    .tick = [&ticks](ofs::EventQueue &, const ofs::HoldTickInfo &) { ++ticks; }});
    f.bs.addDefault("nav.step", {.key = SDLK_RIGHT}, ofs::ActivationMode::Hold);
    f.tp.eq.freeze();

    // Two non-repeat key-downs (belt-and-suspenders dedup); the second must hit the dedup guard, not add
    // a second hold — so a single tick advances exactly one hold.
    f.bs.onKeyDown({.key = SDLK_RIGHT, .modifiers = SDL_KMOD_NONE});
    f.bs.onKeyDown({.key = SDLK_RIGHT, .modifiers = SDL_KMOD_NONE});
    f.bs.tickHolds(0.016f);
    CHECK(ticks == 1);
}

TEST_CASE("BindingSystem: a duplicate gamepad button-down for an already-held button does not double-start") {
    Fixture f;
    int ticks = 0;
    f.registry.add({.id = "nav.step",
                    .group = "Nav",
                    .title = "Step",
                    .tick = [&ticks](ofs::EventQueue &, const ofs::HoldTickInfo &) { ++ticks; }});
    f.bs.addBinding({.trigger = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
                     .commandId = "nav.step",
                     .mode = ofs::ActivationMode::Hold});
    f.tp.eq.freeze();

    f.bs.onGamepadButton({.button = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER});
    f.bs.onGamepadButton({.button = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER});
    f.bs.tickHolds(0.016f);
    CHECK(ticks == 1);
}

TEST_CASE("BindingSystem: rebind capture finalizes on a gamepad button") {
    Fixture f;
    f.tp.eq.freeze();

    f.rebind.targetCommandId = "p.cmd";
    f.rebind.capturing = true;
    f.bs.onGamepadButton({.button = SDL_GAMEPAD_BUTTON_EAST});

    REQUIRE(f.rebind.hasResult);
    CHECK_FALSE(f.rebind.capturing);
    const auto *pb = std::get_if<ofs::PadButton>(&f.rebind.captured);
    REQUIRE(pb != nullptr);
    CHECK(pb->button == SDL_GAMEPAD_BUTTON_EAST);
}

TEST_CASE("BindingSystem: an active analog hold updates its magnitude on the next frame while still deflected") {
    Fixture f;
    float lastAnalog = -1.0f;
    f.registry.add(
        {.id = "axis.cmd",
         .group = "Test",
         .title = "Cmd",
         .tick = [&lastAnalog](ofs::EventQueue &, const ofs::HoldTickInfo &info) { lastAnalog = info.analog; }});
    f.bs.addBinding({.trigger = ofs::PadAxis{.axis = SDL_GAMEPAD_AXIS_LEFTX, .positive = true},
                     .commandId = "axis.cmd",
                     .mode = ofs::ActivationMode::Hold});
    f.bs.setAnalogConfig({.deadzone = 0.15f, .smoothing = 0.0f});
    f.tp.eq.freeze();

    std::array<float, SDL_GAMEPAD_AXIS_COUNT> axes{};

    // Frame 1: past the deadzone → starts the hold.
    axes[SDL_GAMEPAD_AXIS_LEFTX] = 0.9f;
    f.bs.tickAnalog(axes, false, 0.016f);
    f.bs.tickHolds(0.016f);

    // Frame 2: still deflected but at a different magnitude → updates the existing hold's analog value.
    axes[SDL_GAMEPAD_AXIS_LEFTX] = 0.5f;
    f.bs.tickAnalog(axes, false, 0.016f);
    f.bs.tickHolds(0.016f);
    CHECK(lastAnalog == doctest::Approx((0.5f - 0.15f) / (1.0f - 0.15f)));
}

// ── Capture-apply orchestration (ApplyBindingCaptureEvent → apply{Capture,Recapture,Modifier}) ───
//
// These drive the finished-capture write path the Shortcut window uses: a fresh capture, a re-capture
// (swap an existing binding's trigger), and a modifier capture (assign a held pad input). The window
// never mutates the table itself — it pushes ApplyBindingCaptureEvent and BindingSystem resolves the
// conflict / mode-carry / modifier-narrow orchestration. Each handler persists via saveBindings().

namespace {
// Finds the (single) binding for a command in the table; null if the command holds none.
const ofs::Binding *bindingFor(const ofs::BindingSystem &bs, const std::string &commandId) {
    for (const auto &b : bs.bindings())
        if (b.commandId == commandId)
            return &b;
    return nullptr;
}
} // namespace

TEST_CASE("BindingSystem: applyCapture adds a fresh binding for the captured trigger") {
    Fixture f;
    f.registry.add({.id = "core.save", .group = "Core", .title = "Save", .run = [](ofs::EventQueue &) {}});
    f.tp.eq.freeze();

    f.tp.eq.push(ofs::ApplyBindingCaptureEvent{.commandId = "core.save",
                                               .captured = ofs::KeyChord{.key = SDLK_S, .modifiers = SDL_KMOD_CTRL}});
    f.tp.eq.drain();

    const ofs::Binding *b = bindingFor(f.bs, "core.save");
    REQUIRE(b != nullptr);
    const auto *kc = std::get_if<ofs::KeyChord>(&b->trigger);
    REQUIRE(kc != nullptr);
    CHECK(kc->key == SDLK_S);
    CHECK((kc->modifiers & SDL_KMOD_CTRL) != 0);
}

TEST_CASE("BindingSystem: applyCapture auto-reassigns the trigger off a command already holding it") {
    Fixture f;
    f.registry.add({.id = "a.cmd", .group = "T", .title = "A", .run = [](ofs::EventQueue &) {}});
    f.registry.add({.id = "b.cmd", .group = "T", .title = "B", .run = [](ofs::EventQueue &) {}});
    const ofs::KeyChord chord{.key = SDLK_G, .modifiers = SDL_KMOD_NONE};
    f.bs.addBinding({.trigger = chord, .commandId = "a.cmd"});
    f.tp.eq.freeze();

    // Capturing the same trigger for b.cmd must steal it from a.cmd (a trigger maps to one command).
    f.tp.eq.push(ofs::ApplyBindingCaptureEvent{.commandId = "b.cmd", .captured = chord});
    f.tp.eq.drain();

    CHECK(bindingFor(f.bs, "a.cmd") == nullptr);
    const ofs::Binding *b = bindingFor(f.bs, "b.cmd");
    REQUIRE(b != nullptr);
    CHECK(std::get<ofs::KeyChord>(b->trigger) == chord);
}

TEST_CASE("BindingSystem: applyRecapture swaps the trigger, carrying mode and held modifier") {
    Fixture f;
    f.registry.add({.id = "p.cmd", .group = "T", .title = "P", .run = [](ofs::EventQueue &) {}});
    const ofs::Trigger oldTrig = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_SOUTH};
    const ofs::Modifier mod = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
    f.bs.addBinding({.trigger = oldTrig, .commandId = "p.cmd", .modifier = mod, .mode = ofs::ActivationMode::Hold});
    f.tp.eq.freeze();

    // Re-capture to another gamepad button — the held modifier survives (the new trigger is itself a
    // gamepad input) and the Hold mode carries over.
    const ofs::Trigger newTrig = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_NORTH};
    f.tp.eq.push(ofs::ApplyBindingCaptureEvent{
        .commandId = "p.cmd", .captured = newTrig, .replaceTrigger = true, .replaceTarget = oldTrig});
    f.tp.eq.drain();

    REQUIRE(f.bs.bindings().size() == 1);
    const ofs::Binding *b = bindingFor(f.bs, "p.cmd");
    REQUIRE(b != nullptr);
    CHECK(std::get<ofs::PadButton>(b->trigger).button == SDL_GAMEPAD_BUTTON_NORTH);
    CHECK(b->mode == ofs::ActivationMode::Hold);
    const auto *pb = std::get_if<ofs::PadButton>(&b->modifier);
    REQUIRE(pb != nullptr);
    CHECK(pb->button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
}

TEST_CASE("BindingSystem: applyRecapture to the same trigger leaves the binding untouched") {
    Fixture f;
    f.registry.add({.id = "p.cmd", .group = "T", .title = "P", .run = [](ofs::EventQueue &) {}});
    const ofs::Trigger trig = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_EAST};
    f.bs.addBinding({.trigger = trig, .commandId = "p.cmd", .mode = ofs::ActivationMode::Hold});
    f.tp.eq.freeze();

    // Re-captured the very same input → early return, no remove/re-add.
    f.tp.eq.push(ofs::ApplyBindingCaptureEvent{
        .commandId = "p.cmd", .captured = trig, .replaceTrigger = true, .replaceTarget = trig});
    f.tp.eq.drain();

    REQUIRE(f.bs.bindings().size() == 1);
    const ofs::Binding *b = bindingFor(f.bs, "p.cmd");
    REQUIRE(b != nullptr);
    CHECK(std::get<ofs::PadButton>(b->trigger).button == SDL_GAMEPAD_BUTTON_EAST);
    CHECK(b->mode == ofs::ActivationMode::Hold);
}

TEST_CASE("BindingSystem: applyRecapture to a keyboard chord drops the gamepad-only held modifier") {
    Fixture f;
    f.registry.add({.id = "p.cmd", .group = "T", .title = "P", .run = [](ofs::EventQueue &) {}});
    const ofs::Trigger oldTrig = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_SOUTH};
    f.bs.addBinding({.trigger = oldTrig,
                     .commandId = "p.cmd",
                     .modifier = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
                     .mode = ofs::ActivationMode::Hold});
    f.tp.eq.freeze();

    // A KeyChord carries its own Ctrl/Shift/Alt and never reads a held modifier, so the modifier is
    // dropped on re-capture while the mode still carries.
    f.tp.eq.push(ofs::ApplyBindingCaptureEvent{.commandId = "p.cmd",
                                               .captured = ofs::KeyChord{.key = SDLK_K},
                                               .replaceTrigger = true,
                                               .replaceTarget = oldTrig});
    f.tp.eq.drain();

    const ofs::Binding *b = bindingFor(f.bs, "p.cmd");
    REQUIRE(b != nullptr);
    CHECK(std::get<ofs::KeyChord>(b->trigger).key == SDLK_K);
    CHECK(b->mode == ofs::ActivationMode::Hold);
    CHECK(std::holds_alternative<std::monostate>(b->modifier));
}

TEST_CASE("BindingSystem: applyRecapture auto-reassigns the new trigger off another command") {
    Fixture f;
    f.registry.add({.id = "a.cmd", .group = "T", .title = "A", .run = [](ofs::EventQueue &) {}});
    f.registry.add({.id = "b.cmd", .group = "T", .title = "B", .run = [](ofs::EventQueue &) {}});
    const ofs::Trigger aTrig = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_SOUTH};
    const ofs::Trigger bTrig = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_NORTH};
    f.bs.addBinding({.trigger = aTrig, .commandId = "a.cmd"});
    f.bs.addBinding({.trigger = bTrig, .commandId = "b.cmd"});
    f.tp.eq.freeze();

    // Re-capture a.cmd onto b.cmd's trigger — b.cmd loses it (conflict reassign), a.cmd holds it alone.
    f.tp.eq.push(ofs::ApplyBindingCaptureEvent{
        .commandId = "a.cmd", .captured = bTrig, .replaceTrigger = true, .replaceTarget = aTrig});
    f.tp.eq.drain();

    CHECK(bindingFor(f.bs, "b.cmd") == nullptr);
    const ofs::Binding *a = bindingFor(f.bs, "a.cmd");
    REQUIRE(a != nullptr);
    CHECK(std::get<ofs::PadButton>(a->trigger).button == SDL_GAMEPAD_BUTTON_NORTH);
}

TEST_CASE("BindingSystem: applyModifier narrows a captured PadButton into the target's held modifier") {
    Fixture f;
    f.registry.add({.id = "p.cmd", .group = "T", .title = "P", .run = [](ofs::EventQueue &) {}});
    const ofs::Trigger trig = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_DPAD_UP};
    f.bs.addBinding({.trigger = trig, .commandId = "p.cmd"});
    f.tp.eq.freeze();

    f.tp.eq.push(ofs::ApplyBindingCaptureEvent{.commandId = "p.cmd",
                                               .captured = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
                                               .captureModifier = true,
                                               .modifierTarget = trig});
    f.tp.eq.drain();

    const ofs::Binding *b = bindingFor(f.bs, "p.cmd");
    REQUIRE(b != nullptr);
    const auto *pb = std::get_if<ofs::PadButton>(&b->modifier);
    REQUIRE(pb != nullptr);
    CHECK(pb->button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
}

TEST_CASE("BindingSystem: applyModifier narrows a captured PadAxis into the target's held modifier") {
    Fixture f;
    f.registry.add({.id = "p.cmd", .group = "T", .title = "P", .run = [](ofs::EventQueue &) {}});
    const ofs::Trigger trig = ofs::PadButton{.button = SDL_GAMEPAD_BUTTON_DPAD_DOWN};
    f.bs.addBinding({.trigger = trig, .commandId = "p.cmd"});
    f.tp.eq.freeze();

    f.tp.eq.push(
        ofs::ApplyBindingCaptureEvent{.commandId = "p.cmd",
                                      .captured = ofs::PadAxis{.axis = SDL_GAMEPAD_AXIS_LEFT_TRIGGER, .positive = true},
                                      .captureModifier = true,
                                      .modifierTarget = trig});
    f.tp.eq.drain();

    const ofs::Binding *b = bindingFor(f.bs, "p.cmd");
    REQUIRE(b != nullptr);
    const auto *pa = std::get_if<ofs::PadAxis>(&b->modifier);
    REQUIRE(pa != nullptr);
    CHECK(pa->axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    CHECK(pa->positive == true);
}
