#include "Services/BindingSystem.h"
#include "Core/CommandEvents.h"
#include "Core/Events.h"
#include "Localization/Translator.h"
#include "Services/BindingEvents.h"
#include "Util/FileUtil.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <system_error>

namespace ofs {

static std::filesystem::path getBindingsPath() {
    return ofs::util::getPrefPath() / "bindings.json";
}

// Built-in presets ship under the install data dir; user presets live in the pref dir.
static std::filesystem::path builtinPresetDir() {
    return ofs::util::getBasePath() / "data" / "binding_presets";
}
static std::filesystem::path userPresetDir() {
    return ofs::util::getPrefPath() / "binding_presets";
}

// On-disk schema version for bindings.json and binding_presets/*.json (same shape). Bump on an
// incompatible change; load refuses a file newer than this rather than misreading it.
static constexpr int kBindingsFileVersion = 1;

// Filename-safe slug from a display name: lowercase ASCII alphanumerics, runs of anything else
// collapsed to a single '-'. Non-ASCII is dropped (the display name keeps the original); an empty
// result falls back to "preset" so a name like "日本語" still yields a valid filename.
static std::string slugify(const std::string &name) {
    std::string out;
    for (unsigned char c : name) {
        if (std::isalnum(c) != 0)
            out += static_cast<char>(std::tolower(c));
        else if (c == '-' || c == '_')
            out += static_cast<char>(c);
        else if (!out.empty() && out.back() != '-')
            out += '-';
    }
    while (!out.empty() && out.back() == '-')
        out.pop_back();
    return out.empty() ? std::string{"preset"} : out;
}

// Encode one Trigger to its `{type, ...}` JSON object. nullopt for an empty/invalid trigger.
// Shared by the `input` field and the optional `modifier` field so both speak the same shape.
static std::optional<nlohmann::json> triggerToJson(const Trigger &t) {
    if (const auto *kc = std::get_if<KeyChord>(&t)) {
        if (kc->key == SDLK_UNKNOWN)
            return std::nullopt;
        nlohmann::json input;
        input["type"] = "key";
        input["key"] = SDL_GetKeyName(kc->key);
        nlohmann::json modsArr = nlohmann::json::array();
        if ((kc->modifiers & SDL_KMOD_CTRL) != 0)
            modsArr.push_back("ctrl");
        if ((kc->modifiers & SDL_KMOD_SHIFT) != 0)
            modsArr.push_back("shift");
        if ((kc->modifiers & SDL_KMOD_ALT) != 0)
            modsArr.push_back("alt");
        if ((kc->modifiers & SDL_KMOD_GUI) != 0)
            modsArr.push_back("gui");
        if (!modsArr.empty())
            input["mods"] = modsArr;
        return input;
    }
    if (const auto *pb = std::get_if<PadButton>(&t)) {
        if (pb->button == SDL_GAMEPAD_BUTTON_INVALID)
            return std::nullopt;
        const char *btnStr = SDL_GetGamepadStringForButton(pb->button);
        if (btnStr == nullptr)
            return std::nullopt;
        nlohmann::json input;
        input["type"] = "pad";
        input["button"] = btnStr;
        return input;
    }
    if (const auto *pa = std::get_if<PadAxis>(&t)) {
        if (pa->axis == SDL_GAMEPAD_AXIS_INVALID)
            return std::nullopt;
        const char *axStr = SDL_GetGamepadStringForAxis(pa->axis);
        if (axStr == nullptr)
            return std::nullopt;
        nlohmann::json input;
        input["type"] = "axis";
        input["axis"] = axStr;
        input["dir"] = pa->positive ? "+" : "-";
        return input;
    }
    return std::nullopt;
}

// A Modifier encodes as a Trigger `{type, ...}` object (or none) — reuse triggerToJson; monostate ⇒
// omitted. Only the two gamepad sources are encodable (the modifier layer is gamepad-only).
static std::optional<nlohmann::json> modifierToJson(const Modifier &m) {
    if (const auto *pb = std::get_if<PadButton>(&m))
        return triggerToJson(Trigger{*pb});
    if (const auto *pa = std::get_if<PadAxis>(&m))
        return triggerToJson(Trigger{*pa});
    return std::nullopt;
}

// One source of truth for the on-disk entry shape, shared by the active set and preset files.
static std::optional<nlohmann::json> bindingToEntry(const Binding &b) {
    auto input = triggerToJson(b.trigger);
    if (!input)
        return std::nullopt;
    nlohmann::json entry{{"command", b.commandId}, {"input", std::move(*input)}};
    if (auto mod = modifierToJson(b.modifier))
        entry["modifier"] = std::move(*mod);
    // Press is the common case — omit "mode" to keep the file lean (absent ⇒ Press on load).
    if (b.mode == ActivationMode::Hold)
        entry["mode"] = "hold";
    return entry;
}

// Parse one JSON entry into (commandId, trigger, mode). False ⇒ malformed or an unknown trigger type
// (forward-tolerant). Shared by loadBindings() and loadPreset(). A known type with a
// command this build lacks still parses true — the caller decides whether to apply or preserve it.
namespace {
struct ParsedBinding {
    std::string commandId;
    Trigger trigger;
    Modifier modifier;
    ActivationMode mode = ActivationMode::Press;
};
} // namespace

// Decode one `{type, ...}` object into a Trigger. False ⇒ malformed or an unknown type (forward-
// tolerant skip). Shared by the `input` and `modifier` fields.
static bool triggerFromJson(const nlohmann::json &input, Trigger &out) {
    if (!input.contains("type"))
        return false;
    const std::string type = input["type"].get<std::string>();
    if (type == "key") {
        if (!input.contains("key"))
            return false;
        SDL_Keycode key = SDL_GetKeyFromName(input["key"].get<std::string>().c_str());
        if (key == SDLK_UNKNOWN)
            return false;
        SDL_Keymod mods = SDL_KMOD_NONE;
        if (input.contains("mods")) {
            for (const auto &m : input["mods"]) {
                const std::string ms = m.get<std::string>();
                if (ms == "ctrl")
                    mods |= SDL_KMOD_CTRL;
                else if (ms == "shift")
                    mods |= SDL_KMOD_SHIFT;
                else if (ms == "alt")
                    mods |= SDL_KMOD_ALT;
                else if (ms == "gui")
                    mods |= SDL_KMOD_GUI;
            }
        }
        out = KeyChord{.key = key, .modifiers = mods};
        return true;
    }
    if (type == "pad") {
        if (!input.contains("button"))
            return false;
        SDL_GamepadButton button = SDL_GetGamepadButtonFromString(input["button"].get<std::string>().c_str());
        if (button == SDL_GAMEPAD_BUTTON_INVALID)
            return false;
        out = PadButton{.button = button};
        return true;
    }
    if (type == "axis") {
        if (!input.contains("axis"))
            return false;
        SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(input["axis"].get<std::string>().c_str());
        if (axis == SDL_GAMEPAD_AXIS_INVALID)
            return false;
        // "dir" absent ⇒ "+"; only an explicit "-" selects the negative half.
        out = PadAxis{.axis = axis, .positive = input.value("dir", std::string{"+"}) != "-"};
        return true;
    }
    return false; // unknown type → forward-tolerant skip
}

static bool parseEntry(const nlohmann::json &entry, ParsedBinding &out) {
    if (!entry.contains("command") || !entry.contains("input"))
        return false;
    out.commandId = entry["command"].get<std::string>();
    // "hold" ⇒ Hold; absent/anything else ⇒ Press.
    out.mode = (entry.value("mode", std::string{}) == "hold") ? ActivationMode::Hold : ActivationMode::Press;
    if (!triggerFromJson(entry["input"], out.trigger))
        return false;
    // Optional modifier: a gamepad source only (the held-gate layer is gamepad-only). A keyboard or
    // otherwise-unparseable object leaves the binding unmodified rather than failing the whole entry —
    // so an older file that stored a KeyMod modifier loads as a plain binding instead of being dropped.
    out.modifier = std::monostate{};
    if (entry.contains("modifier")) {
        Trigger modTrigger;
        if (triggerFromJson(entry["modifier"], modTrigger)) {
            if (const auto *pb = std::get_if<PadButton>(&modTrigger))
                out.modifier = *pb;
            else if (const auto *pa = std::get_if<PadAxis>(&modTrigger))
                out.modifier = *pa;
        }
    }
    return true;
}

BindingSystem::BindingSystem(EventQueue &eq, CommandRegistry &registry, RebindState &rebindState)
    : eventQueue_(eq), commandRegistry_(registry), rebindState_(rebindState) {

    eventQueue_.on<KeyDownEvent>([this](const KeyDownEvent &e) { onKeyDown(e); });
    eventQueue_.on<KeyUpEvent>([this](const KeyUpEvent &e) { onKeyUp(e); });
    eventQueue_.on<GamepadButtonEvent>([this](const GamepadButtonEvent &e) { onGamepadButton(e); });
    eventQueue_.on<GamepadButtonUpEvent>([this](const GamepadButtonUpEvent &e) { onGamepadButtonUp(e); });

    // Plugin unload — drop the plugin's key bindings and override entries. The commands themselves are
    // removed synchronously by PluginManager at unload (symmetric with their direct registration), so a
    // same-frame reload isn't clobbered by this deferred cleanup.
    eventQueue_.on<UnregisterPluginShortcutsEvent>([this](const UnregisterPluginShortcutsEvent &e) {
        const std::string prefix = fmt::format("{}.", e.group);
        std::erase_if(bindings_, [&prefix](const Binding &b) { return b.commandId.starts_with(prefix); });
        for (auto it = loadedOverrides_.begin(); it != loadedOverrides_.end();) {
            if (it->first.starts_with(prefix))
                it = loadedOverrides_.erase(it);
            else
                ++it;
        }
    });

    // Reload bindings from file (pushed by PluginManager on plugin (re)enable).
    eventQueue_.on<LoadShortcutBindingsEvent>([this](const LoadShortcutBindingsEvent &) { loadBindings(); });

    // A custom command was deleted: prune any bindings to it and persist. Independent of
    // CustomCommandStore's own handler for the same event — two reactions to one user intent, so no
    // service calls another. Scoped to custom deletion only: a custom
    // id is append-only and never reused, so a stale binding has no value. Provider/plugin removals are
    // deliberately NOT pruned here — their ids are stable, so a tolerated binding re-attaches.
    eventQueue_.on<RemoveCustomCommandEvent>([this](const RemoveCustomCommandEvent &e) {
        loadedOverrides_.erase(e.id);
        if (std::erase_if(bindings_, [&e](const Binding &b) { return b.commandId == e.id; }) > 0)
            saveBindings();
    });

    // Binding-table + preset write-API (pushed by the Shortcut window; see Services/BindingEvents.h).
    eventQueue_.on<BeginBindingCaptureEvent>([this](const BeginBindingCaptureEvent &e) {
        rebindState_.targetCommandId = e.commandId;
        rebindState_.capturing = true;
        rebindState_.hasResult = false;
        rebindState_.captureModifier = e.captureModifier;
        rebindState_.modifierTarget = e.modifierTarget;
        rebindState_.replaceTrigger = e.replaceTrigger;
        rebindState_.replaceTarget = e.replaceTarget;
    });
    eventQueue_.on<ApplyBindingCaptureEvent>([this](const ApplyBindingCaptureEvent &e) {
        if (e.captureModifier)
            applyModifier(e.commandId, e.modifierTarget, e.captured);
        else if (e.replaceTrigger)
            applyRecapture(e.commandId, e.replaceTarget, e.captured);
        else
            applyCapture(e.commandId, e.captured);
    });
    eventQueue_.on<RemoveBindingEvent>([this](const RemoveBindingEvent &e) {
        removeBinding(e.trigger, e.commandId);
        saveBindings();
    });
    eventQueue_.on<SetBindingModeEvent>([this](const SetBindingModeEvent &e) {
        setMode(e.trigger, e.commandId, e.mode);
        saveBindings();
    });
    eventQueue_.on<SetBindingModifierEvent>([this](const SetBindingModifierEvent &e) {
        setModifier(e.trigger, e.commandId, e.modifier);
        saveBindings();
    });
    eventQueue_.on<SaveBindingPresetEvent>([this](const SaveBindingPresetEvent &e) { saveActiveAsPreset(e.name); });
    eventQueue_.on<LoadBindingPresetEvent>([this](const LoadBindingPresetEvent &e) {
        const int unapplied = loadPreset(e.slug);
        // Surface a partial load — entries referencing a command this build lacks (missing plugin /
        // undefined custom command / unknown trigger) are kept for round-trip but never dispatch.
        if (unapplied > 0)
            eventQueue_.push(NotifyEvent{.level = NotifyLevel::Warning,
                                         .message = Str::ScPresetLoadedPartial.fmt(e.name, unapplied)});
        else
            eventQueue_.push(NotifyEvent{.level = NotifyLevel::Success, .message = Str::ScPresetLoaded.fmt(e.name)});
    });
    eventQueue_.on<DeleteBindingPresetEvent>([this](const DeleteBindingPresetEvent &e) { deletePreset(e.slug); });
    eventQueue_.on<ResetBindingsEvent>([this](const ResetBindingsEvent &) { resetToDefaults(); });
}

// ── Capture-apply orchestration (moved from ShortcutWindow) ───────────────────
void BindingSystem::applyCapture(const std::string &commandId, const Trigger &trigger) {
    // Remove any existing binding with the same trigger (conflict auto-reassign). Copy the conflicting
    // commandId before erasing to avoid iterator invalidation.
    for (const auto &b : bindings_) {
        if (b.trigger == trigger) {
            const std::string conflictId = b.commandId;
            removeBinding(trigger, conflictId);
            break;
        }
    }
    addBinding(Binding{.trigger = trigger, .commandId = commandId});
    saveBindings();
}

void BindingSystem::applyModifier(const std::string &commandId, const Trigger &targetTrigger, const Trigger &captured) {
    // The held-gate modifier layer is gamepad-only; capture only finalizes on a gamepad source, so a
    // KeyChord never reaches here. Anything else leaves the modifier cleared (monostate).
    Modifier mod;
    if (const auto *pb = std::get_if<PadButton>(&captured))
        mod = *pb;
    else if (const auto *pa = std::get_if<PadAxis>(&captured))
        mod = *pa;
    setModifier(targetTrigger, commandId, mod);
    saveBindings();
}

void BindingSystem::applyRecapture(const std::string &commandId, const Trigger &oldTrigger, const Trigger &captured) {
    if (captured == oldTrigger)
        return; // re-captured the same input — leave the existing binding (mode/modifier) untouched

    // Carry the old binding's mode and modifier onto the new trigger. The held-gate modifier is
    // gamepad-only, so it only survives when the new trigger is itself a gamepad input — a KeyChord
    // carries its own Ctrl/Shift/Alt and never reads a held modifier.
    ActivationMode mode = ActivationMode::Press;
    Modifier modifier;
    for (const auto &b : bindings_) {
        if (b.commandId == commandId && b.trigger == oldTrigger) {
            mode = b.mode;
            if (!std::holds_alternative<KeyChord>(captured))
                modifier = b.modifier;
            break;
        }
    }

    removeBinding(oldTrigger, commandId);

    // Reassign the new trigger away from any other command currently holding it (conflict auto-reassign,
    // mirroring applyCapture). Copy the conflicting id before erasing to avoid iterator invalidation.
    for (const auto &b : bindings_) {
        if (b.trigger == captured) {
            const std::string conflictId = b.commandId;
            removeBinding(captured, conflictId);
            break;
        }
    }

    addBinding(Binding{.trigger = captured, .commandId = commandId, .modifier = modifier, .mode = mode});
    saveBindings();
}

void BindingSystem::addDefault(const std::string &commandId, KeyChord chord, ActivationMode mode) {
    if (chord.key == SDLK_UNKNOWN)
        return;
    chord.modifiers = normalizeModifiers(chord.modifiers);
    defaults_.push_back({.trigger = chord, .commandId = commandId, .mode = mode});
    bindings_.push_back({.trigger = chord, .commandId = commandId, .mode = mode});
}

void BindingSystem::addBinding(Binding b) {
    if (auto *kc = std::get_if<KeyChord>(&b.trigger))
        kc->modifiers = normalizeModifiers(kc->modifiers);
    bindings_.push_back(std::move(b));
    sortByTriggerType();
}

// stable_sort keeps the relative (file / insertion) order within each trigger type. defaults_ is all
// KeyChords (addDefault takes only a KeyChord), so the bindings_ = defaults_ paths are already grouped
// and need no resort; only loaded/added entries can mix types.
void BindingSystem::sortByTriggerType() {
    std::ranges::stable_sort(bindings_,
                             [](const Binding &a, const Binding &b) { return a.trigger.index() < b.trigger.index(); });
}

void BindingSystem::removeBinding(const Trigger &trigger, const std::string &commandId) {
    std::erase_if(bindings_, [&](const Binding &b) { return b.commandId == commandId && b.trigger == trigger; });
}

void BindingSystem::setMode(const Trigger &trigger, const std::string &commandId, ActivationMode mode) {
    for (auto &b : bindings_) {
        if (b.commandId != commandId || b.trigger != trigger)
            continue;
        b.mode = mode;
        return;
    }
}

void BindingSystem::setModifier(const Trigger &trigger, const std::string &commandId, const Modifier &modifier) {
    for (auto &b : bindings_) {
        if (b.commandId != commandId || b.trigger != trigger)
            continue;
        b.modifier = modifier;
        return;
    }
}

bool BindingSystem::modifierHeld(const Modifier &m) const {
    if (std::holds_alternative<std::monostate>(m))
        return true;
    if (const auto *pb = std::get_if<PadButton>(&m))
        return pb->button != SDL_GAMEPAD_BUTTON_INVALID && pb->button < SDL_GAMEPAD_BUTTON_COUNT &&
               padButtonsDown_[pb->button];
    if (const auto *pa = std::get_if<PadAxis>(&m)) {
        if (pa->axis == SDL_GAMEPAD_AXIS_INVALID)
            return false;
        // Raw, not smoothed: a modifier is a binary held-state and must drop the instant the trigger is
        // released. The smoothing low-pass is for the analog *velocity* only (see axisRaw_).
        const float dz = std::clamp(analogConfig_.deadzone, 0.0f, 0.99f);
        const float defl = pa->positive ? axisRaw_[pa->axis] : -axisRaw_[pa->axis];
        return defl > dz;
    }
    return false;
}

bool BindingSystem::modifiedLayerActive(const Trigger &trigger) const {
    return std::ranges::any_of(bindings_, [&](const Binding &b) {
        if (b.trigger != trigger || std::holds_alternative<std::monostate>(b.modifier))
            return false;
        const Command *c = commandRegistry_.find(b.commandId);
        if (!c || !c->enabled())
            return false;
        return modifierHeld(b.modifier);
    });
}

bool BindingSystem::modifierEligible(const Binding &b) const {
    if (!modifierHeld(b.modifier))
        return false;
    // An unmodified binding yields to an active modified layer on the same trigger (Ctrl+S beats S).
    return !(std::holds_alternative<std::monostate>(b.modifier) && modifiedLayerActive(b.trigger));
}

bool BindingSystem::isModifierActiveFor(const std::string &commandId) const {
    return std::ranges::any_of(bindings_, [&](const Binding &b) {
        return b.commandId == commandId && !std::holds_alternative<std::monostate>(b.modifier) &&
               modifierHeld(b.modifier);
    });
}

void BindingSystem::onKeyDown(const KeyDownEvent &event) {
    // Rebind capture runs before the keyboard-captured guard (the rebind modal sets that flag).
    if (rebindState_.capturing) {
        if (event.key == SDLK_ESCAPE) {
            rebindState_.capturing = false;
            rebindState_.targetCommandId.clear();
            rebindState_.captureModifier = false;
        } else if (rebindState_.captureModifier || isModifierKey(event.key)) {
            // captureModifier: the held-gate modifier layer is gamepad-only, so a key can't be a
            // modifier — ignore key input while capturing one (a gamepad button / stick / trigger
            // finalizes it instead). isModifierKey: a lone modifier key-down (Left Alt, …) arrives as
            // key=SDLK_L*ALT with its own mod bit set; capturing it verbatim yields "Alt+Left Alt", so
            // keep waiting until a non-modifier key completes the chord. Either way, don't finalize here.
            return;
        } else {
            rebindState_.captured = KeyChord{.key = event.key, .modifiers = normalizeModifiers(event.modifiers)};
            rebindState_.hasResult = true;
            rebindState_.capturing = false;
        }
        return;
    }

    if (event.keyboardCaptured)
        return;

    // Ignore OS key-repeats: held keys never re-dispatch through key-down. Repetition is the Hold
    // tick's job, and a held Press binding must fire exactly once.
    if (event.repeat)
        return;

    SDL_Keymod mods = normalizeModifiers(event.modifiers);

    for (const auto &b : bindings_) {
        const auto *kc = std::get_if<KeyChord>(&b.trigger);
        if (!kc || kc->key == SDLK_UNKNOWN)
            continue;
        if (kc->key != event.key || kc->modifiers != mods)
            continue;

        // First *enabled* match: two commands may share one trigger, differentiated by
        // Command::isEnabled context; dispatch runs whichever is live. Identical to current behavior
        // while every command is enabled, but bakes contextual bindings in as a pure addition.
        const Command *cmd = commandRegistry_.find(b.commandId);
        if (!cmd || !cmd->enabled())
            continue;

        // Modifier gate: skip when the binding's modifier isn't held, or when this unmodified binding
        // is being shadowed by an active modified layer on the same trigger (gamepad modifiers).
        if (!modifierEligible(b))
            continue;

        if (b.mode == ActivationMode::Hold && cmd->holdable()) {
            // Start a hold. Dedup by key+id so a stray duplicate key-down can't grow the set; OS
            // repeats are already filtered above, so this is belt-and-suspenders.
            for (const auto &h : activeHolds_)
                if (h.src == ActiveHold::Src::Key && h.key == event.key && h.commandId == b.commandId)
                    return;
            activeHolds_.push_back(
                {.src = ActiveHold::Src::Key, .key = event.key, .commandId = b.commandId, .elapsed = 0.0f});
            return; // no run() on press — the first tick next frame applies the first step
        }

        // Press, or a Hold binding on a non-holdable command (fall back to a single fire).
        commandRegistry_.run(b.commandId);
        return;
    }
}

void BindingSystem::onKeyUp(const KeyUpEvent &event) {
    // Release matches on key alone: pressing/releasing a modifier mid-hold must not strand it.
    std::erase_if(activeHolds_,
                  [&event](const ActiveHold &h) { return h.src == ActiveHold::Src::Key && h.key == event.key; });
}

void BindingSystem::tickHolds(float dt) {
    // The rebind modal owns input while capturing — end any in-flight hold so it can't tick under it.
    if (rebindState_.capturing) {
        clearHolds();
        return;
    }

    // Drop holds whose command vanished or became disabled mid-hold (e.g. editMode toggled off while a
    // nudge key is down), so a context change ends the gesture cleanly.
    std::erase_if(activeHolds_, [this](const ActiveHold &h) {
        const Command *c = commandRegistry_.find(h.commandId);
        return !c || !c->enabled() || !c->holdable();
    });

    for (auto &h : activeHolds_) {
        const bool first = (h.elapsed == 0.0f); // exact: elapsed is set to 0 at push time
        h.elapsed += dt;
        const Command *cmd = commandRegistry_.find(h.commandId);
        if (cmd && cmd->tick)
            cmd->tick(eventQueue_, HoldTickInfo{.dt = dt, .elapsed = h.elapsed, .first = first, .analog = h.analog});
    }
}

int holdRepeats(float elapsed, float dt, const HoldRepeatParams &p) {
    // Fires whose scheduled time falls in the window (elapsed-dt, elapsed]: an immediate fire at t=0,
    // then a repeat schedule starting at initialDelay with the n-th gap = max(kFloor, interval*accel^n).
    //
    // Computed in *closed form* per regime rather than enumerated. The old loop walked the schedule one
    // gap at a time, but with accel<1 the gap shrinks to kFloor and the cumulative time then advances by
    // only kFloor per step — so a fixed iteration cap was exhausted long before reaching a multi-second
    // `elapsed`, and the function silently returned 0. That stalled every held frame-step (and any other
    // accelerating Hold) after ~1.5 s, on keyboard and gamepad alike.
    constexpr int kMaxBurst = 16; // ceiling on fires coalesced into one frame — caps the peak burst speed
    // Terminal gap an accelerating (accel<1) hold shrinks toward, and the absolute min gap overall. Set by
    // the user's maxRateHz but clamped to the hard ceiling (see kHoldRepeatMaxRateHz for the rationale).
    const double maxRate = std::min(static_cast<double>(p.maxRateHz), kHoldRepeatMaxRateHz);
    const double kFloor = 1.0 / std::max(1.0, maxRate);
    const double lo = static_cast<double>(elapsed) - static_cast<double>(dt);
    const auto hi = static_cast<double>(elapsed);
    const double base = std::max(kFloor, static_cast<double>(p.interval));
    const auto accel = static_cast<double>(p.accel);
    const auto delay = static_cast<double>(p.initialDelay);

    // Repeat fire n (n>=1) lands at delay + S(n-1), where S(k) = sum of the first k gaps. Returns the
    // number of repeat fires at or before `time` (the immediate t=0 fire is counted separately below).
    auto firesAtOrBefore = [&](double time) -> long long {
        if (time < delay)
            return 0;
        const double budget = time - delay; // summed-gap budget
        long long gaps = 0;                 // max k with S(k) <= budget
        if (accel == 1.0) {
            gaps = static_cast<long long>(std::floor(budget / base));
        } else if (accel > 1.0) {
            // Growing geometric: S(k) = base*(accel^k - 1)/(accel - 1).
            const double x = 1.0 + budget * (accel - 1.0) / base;
            gaps = static_cast<long long>(std::floor(std::log(x) / std::log(accel)));
        } else {
            // Shrinking geometric, clamped to kFloor after m gaps. S(k) is geometric for k<=m, then
            // linear at kFloor.
            const double m = std::ceil(std::log(kFloor / base) / std::log(accel));
            const long long mi = m > 0.0 ? static_cast<long long>(m) : 0;
            const double sm = base * (1.0 - std::pow(accel, static_cast<double>(mi))) / (1.0 - accel);
            if (budget < sm) {
                const double y = 1.0 - budget * (1.0 - accel) / base; // == accel^k
                gaps = y > 0.0 ? static_cast<long long>(std::floor(std::log(y) / std::log(accel))) : mi;
            } else {
                gaps = mi + static_cast<long long>(std::floor((budget - sm) / kFloor));
            }
        }
        if (gaps < 0)
            gaps = 0;
        return gaps + 1; // +1 for fire #1 at `delay` (zero gaps)
    };

    long long count = firesAtOrBefore(hi) - firesAtOrBefore(lo);
    // The immediate t=0 fire uses an inclusive left edge so it lands on the first tick (lo == 0).
    if (lo <= 0.0 && 0.0 <= hi)
        ++count;
    if (count < 0)
        count = 0;
    return count > kMaxBurst ? kMaxBurst : static_cast<int>(count);
}

void BindingSystem::onGamepadButton(const GamepadButtonEvent &event) {
    // Track held-state for PadButton modifiers before any early return, so modifierHeld() stays
    // accurate even while capturing / gamepad-captured.
    if (event.button != SDL_GAMEPAD_BUTTON_INVALID && event.button < SDL_GAMEPAD_BUTTON_COUNT)
        padButtonsDown_[event.button] = true;

    if (rebindState_.capturing) {
        rebindState_.captured = PadButton{.button = event.button};
        rebindState_.hasResult = true;
        rebindState_.capturing = false;
        return;
    }

    if (event.gamepadCaptured)
        return;

    for (const auto &b : bindings_) {
        const auto *pb = std::get_if<PadButton>(&b.trigger);
        if (!pb || pb->button != event.button)
            continue;

        // Mirror onKeyDown: first *enabled* match, Press / Hold-start / holdable fallback.
        const Command *cmd = commandRegistry_.find(b.commandId);
        if (!cmd || !cmd->enabled())
            continue;

        if (!modifierEligible(b))
            continue;

        if (b.mode == ActivationMode::Hold && cmd->holdable()) {
            for (const auto &h : activeHolds_)
                if (h.src == ActiveHold::Src::PadButton && h.button == event.button && h.commandId == b.commandId)
                    return;
            activeHolds_.push_back(
                {.src = ActiveHold::Src::PadButton, .button = event.button, .commandId = b.commandId, .elapsed = 0.0f});
            return; // no run() on press — the first tick next frame applies the first step
        }

        commandRegistry_.run(b.commandId);
        return;
    }
}

void BindingSystem::onGamepadButtonUp(const GamepadButtonUpEvent &event) {
    if (event.button != SDL_GAMEPAD_BUTTON_INVALID && event.button < SDL_GAMEPAD_BUTTON_COUNT)
        padButtonsDown_[event.button] = false;

    // Release matches on the button alone, like onKeyUp for keys.
    std::erase_if(activeHolds_, [&event](const ActiveHold &h) {
        return h.src == ActiveHold::Src::PadButton && h.button == event.button;
    });
}

void BindingSystem::tickAnalog(const std::array<float, SDL_GAMEPAD_AXIS_COUNT> &raw, bool padCaptured, float dt) {
    axisRaw_ = raw; // unsmoothed snapshot for modifier held-state (modifierHeld)

    // Frame-rate-correct exponential low-pass: alpha = 1 - exp(-dt/tau). Smooths stick/trigger jitter
    // before the deadzone test so a noisy idle stick can't flicker a hold on and off.
    const float tau = std::max(1e-3f, analogConfig_.smoothing);
    const float alpha = 1.0f - std::exp(-dt / tau);
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a)
        axisSmoothed_[a] += (raw[a] - axisSmoothed_[a]) * alpha;

    // The rebind modal owns input while capturing (OfsApp handles axis capture from the raw samples);
    // ImGui gamepad-nav suppresses dispatch. Either way, end any analog holds so none ticks underneath.
    if (rebindState_.capturing || padCaptured) {
        std::erase_if(activeHolds_, [](const ActiveHold &h) { return h.src == ActiveHold::Src::PadAxis; });
        return;
    }

    const float dz = std::clamp(analogConfig_.deadzone, 0.0f, 0.99f);
    for (const auto &b : bindings_) {
        const auto *pa = std::get_if<PadAxis>(&b.trigger);
        if (!pa || pa->axis == SDL_GAMEPAD_AXIS_INVALID || b.mode != ActivationMode::Hold)
            continue;

        // Deflection in the bound direction, then a scaled deadzone so output ramps from 0 just past
        // the threshold (no jump at the edge). Triggers rest at 0, so their negative half never fires.
        const float defl = pa->positive ? axisSmoothed_[pa->axis] : -axisSmoothed_[pa->axis];
        const float mag = defl <= dz ? 0.0f : std::min((defl - dz) / (1.0f - dz), 1.0f);

        auto it = std::ranges::find_if(activeHolds_, [&](const ActiveHold &h) {
            return h.src == ActiveHold::Src::PadAxis && h.axis == pa->axis && h.positive == pa->positive &&
                   h.commandId == b.commandId;
        });

        const Command *cmd = commandRegistry_.find(b.commandId);
        const bool live = mag > 0.0f && cmd != nullptr && cmd->enabled() && cmd->holdable() && modifierEligible(b);

        if (live && it == activeHolds_.end()) {
            activeHolds_.push_back({.src = ActiveHold::Src::PadAxis,
                                    .axis = pa->axis,
                                    .positive = pa->positive,
                                    .commandId = b.commandId,
                                    .elapsed = 0.0f,
                                    .analog = mag}); // elapsed 0 ⇒ first tick seeds the pose
        } else if (live) {
            it->analog = mag; // stick deflection scales velocity each frame
        } else if (it != activeHolds_.end()) {
            activeHolds_.erase(it); // dropped below the deadzone (or disabled) — end the hold
        }
    }
}

void BindingSystem::clearHolds() {
    activeHolds_.clear();
}

void BindingSystem::clearKeyHolds() {
    std::erase_if(activeHolds_, [](const ActiveHold &h) { return h.src == ActiveHold::Src::Key; });
}

// ── Persistence ───────────────────────────────────────────────────────────────

void BindingSystem::loadBindings() {
    bindings_.clear();
    loadedOverrides_.clear();
    presetUnapplied_.clear(); // the active set is per-machine and preserves no unknown entries

    try {
        auto text = ofs::util::readFile(getBindingsPath());
        if (!text) {
            // No file → restore code defaults.
            bindings_ = defaults_;
            return;
        }

        nlohmann::json j = nlohmann::json::parse(*text);
        if (!j.contains("version") || !j.contains("bindings")) {
            bindings_ = defaults_;
            return;
        }
        if (j.value("version", kBindingsFileVersion) > kBindingsFileVersion) {
            OFS_CORE_ERROR("bindings.json version {} is newer than supported version {}; using defaults.",
                           j.value("version", 0), kBindingsFileVersion);
            bindings_ = defaults_;
            return;
        }

        // Parse all entries into loadedOverrides_; apply them to commands currently in the registry.
        for (const auto &entry : j["bindings"]) {
            ParsedBinding pe;
            if (!parseEntry(entry, pe)) // malformed / unknown type → forward-tolerant skip
                continue;
            loadedOverrides_[pe.commandId].emplace_back(pe.trigger, pe.modifier, pe.mode);
        }

        // Apply overrides for commands currently in the registry; an id this build lacks is silently
        // skipped and dropped on the next saveBindings() call. `inRebindList` is not consulted — a binding
        // the user assigned to a command that isn't offered by default (a provider, an opener) still applies.
        for (const auto &[id, entries] : loadedOverrides_) {
            const Command *cmd = commandRegistry_.find(id);
            if (!cmd)
                continue;
            for (const auto &lb : entries)
                bindings_.push_back({.trigger = lb.trigger, .commandId = id, .modifier = lb.modifier, .mode = lb.mode});
        }
        sortByTriggerType();
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("BindingSystem: failed to load bindings: {}", e.what());
        bindings_ = defaults_;
    }
}

void BindingSystem::saveBindings() const {
    try {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto &b : bindings_)
            if (auto entry = bindingToEntry(b))
                arr.push_back(std::move(*entry));
        nlohmann::json j;
        j["version"] = kBindingsFileVersion;
        j["bindings"] = arr;
        ofs::util::writeFileAtomic(getBindingsPath(), j.dump(4));
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("BindingSystem: failed to save bindings: {}", e.what());
    }
}

// ── Presets ────────────────────────────────────────────────────────────────

std::vector<PresetInfo> BindingSystem::listPresets() const {
    // Dedup by slug; a user preset shadows a built-in of the same slug, so scan user first and let a
    // later built-in fill in only slugs the user dir didn't already provide.
    std::map<std::string, PresetInfo> bySlug;
    auto scan = [&bySlug](const std::filesystem::path &dir, bool builtin) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec))
            return;
        for (const auto &de : std::filesystem::directory_iterator(dir, ec)) {
            if (ofs::util::toUtf8(de.path().extension()) != ".json")
                continue;
            const std::string slug = ofs::util::toUtf8(de.path().stem());
            if (bySlug.contains(slug))
                continue; // user already claimed this slug
            std::string name = slug;
            if (auto text = ofs::util::readFile(de.path())) {
                try {
                    name = nlohmann::json::parse(*text).value("name", slug);
                } catch (const std::exception &) {
                    // Unparseable file → list it under its slug rather than hiding it.
                }
            }
            bySlug.emplace(slug, PresetInfo{.name = name, .slug = slug, .builtin = builtin});
        }
    };
    scan(userPresetDir(), false);
    scan(builtinPresetDir(), true);

    std::vector<PresetInfo> out;
    out.reserve(bySlug.size());
    for (auto &[slug, info] : bySlug)
        out.push_back(std::move(info));
    std::ranges::sort(out, [](const PresetInfo &a, const PresetInfo &b) { return a.name < b.name; });
    return out;
}

int BindingSystem::loadPreset(const std::string &slug) {
    // User preset shadows a built-in of the same slug (lets a user fork a built-in).
    std::filesystem::path path = userPresetDir() / ofs::util::fromUtf8(slug + ".json");
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        path = builtinPresetDir() / ofs::util::fromUtf8(slug + ".json");

    auto text = ofs::util::readFile(path);
    if (!text) {
        OFS_CORE_ERROR("BindingSystem: preset '{}' not found", slug);
        return 0;
    }

    try {
        nlohmann::json j = nlohmann::json::parse(*text);
        if (!j.contains("bindings"))
            return 0;
        if (j.value("version", kBindingsFileVersion) > kBindingsFileVersion) {
            OFS_CORE_ERROR("Binding preset '{}' version {} is newer than supported version {}.", slug,
                           j.value("version", 0), kBindingsFileVersion);
            return 0;
        }

        bindings_.clear();
        loadedOverrides_.clear();
        presetUnapplied_.clear();

        for (const auto &entry : j["bindings"]) {
            ParsedBinding pe;
            const Command *cmd = parseEntry(entry, pe) ? commandRegistry_.find(pe.commandId) : nullptr;
            if (cmd == nullptr) {
                // A command this build lacks (or an unknown trigger type): keep the raw entry so an
                // edit-and-resave doesn't strip a newer user's binding. Preset files only.
                presetUnapplied_.push_back(entry.dump());
                continue;
            }
            loadedOverrides_[pe.commandId].emplace_back(pe.trigger, pe.modifier, pe.mode);
            bindings_.push_back(
                {.trigger = pe.trigger, .commandId = pe.commandId, .modifier = pe.modifier, .mode = pe.mode});
        }
        sortByTriggerType();

        // Persist into the active set so the load survives restart. The active bindings.json
        // does not retain presetUnapplied_ — those live only to round-trip back out to a preset file.
        saveBindings();
        return static_cast<int>(presetUnapplied_.size());
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("BindingSystem: failed to load preset '{}': {}", slug, e.what());
        return 0;
    }
}

void BindingSystem::saveActiveAsPreset(const std::string &name) {
    try {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto &b : bindings_)
            if (auto entry = bindingToEntry(b))
                arr.push_back(std::move(*entry));
        // Re-emit, verbatim, the entries the last loadPreset() could not bind.
        for (const auto &raw : presetUnapplied_)
            arr.push_back(nlohmann::json::parse(raw));

        nlohmann::json j;
        j["version"] = kBindingsFileVersion;
        j["name"] = name;
        j["bindings"] = arr;

        std::error_code ec;
        std::filesystem::create_directories(userPresetDir(), ec);
        ofs::util::writeFileAtomic(userPresetDir() / ofs::util::fromUtf8(slugify(name) + ".json"), j.dump(4));
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("BindingSystem: failed to save preset '{}': {}", name, e.what());
    }
}

void BindingSystem::deletePreset(const std::string &slug) {
    // User presets only — built-ins are read-only and never written or removed.
    std::error_code ec;
    std::filesystem::remove(userPresetDir() / ofs::util::fromUtf8(slug + ".json"), ec);
}

void BindingSystem::resetToDefaults() {
    bindings_ = defaults_;
    presetUnapplied_.clear();
    saveBindings();
}

// ── Helpers ───────────────────────────────────────────────────────────────────

const Trigger *BindingSystem::findHint(const std::string &commandId) const {
    for (const auto &b : bindings_) {
        if (b.commandId != commandId)
            continue;
        if (const auto *kc = std::get_if<KeyChord>(&b.trigger)) {
            if (kc->key != SDLK_UNKNOWN)
                return &b.trigger;
        } else {
            return &b.trigger; // PadButton is always valid if present
        }
    }
    return nullptr;
}

bool BindingSystem::isModifierKey(SDL_Keycode key) {
    switch (key) {
    case SDLK_LCTRL:
    case SDLK_RCTRL:
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
    case SDLK_LALT:
    case SDLK_RALT:
    case SDLK_LGUI:
    case SDLK_RGUI:
        return true;
    default:
        return false;
    }
}

SDL_Keymod BindingSystem::normalizeModifiers(SDL_Keymod mods) {
    SDL_Keymod result = SDL_KMOD_NONE;
    if ((mods & SDL_KMOD_CTRL) != 0)
        result |= SDL_KMOD_CTRL;
    if ((mods & SDL_KMOD_SHIFT) != 0)
        result |= SDL_KMOD_SHIFT;
    if ((mods & SDL_KMOD_ALT) != 0)
        result |= SDL_KMOD_ALT;
    if ((mods & SDL_KMOD_GUI) != 0)
        result |= SDL_KMOD_GUI;
    return result;
}

} // namespace ofs
