#pragma once

#include "Core/EventQueue.h"
#include "Services/CommandRegistry.h"
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keycode.h>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace ofs {

struct KeyDownEvent;         // Core/Events.h
struct KeyUpEvent;           // Core/Events.h
struct GamepadButtonEvent;   // Core/Events.h
struct GamepadButtonUpEvent; // Core/Events.h

struct ApplyBindingCaptureEvent; // Services/BindingEvents.h
struct RemoveBindingEvent;       // Services/BindingEvents.h
struct SetBindingModeEvent;      // Services/BindingEvents.h
struct SetBindingModifierEvent;  // Services/BindingEvents.h
struct SaveBindingPresetEvent;   // Services/BindingEvents.h
struct LoadBindingPresetEvent;   // Services/BindingEvents.h
struct DeleteBindingPresetEvent; // Services/BindingEvents.h
struct ResetBindingsEvent;       // Services/BindingEvents.h

// ── Trigger types ────────────────────────────────────────────────────────────

struct KeyChord {
    SDL_Keycode key = SDLK_UNKNOWN;
    SDL_Keymod modifiers = SDL_KMOD_NONE;
    // `repeat` removed — OS key-repeat is no longer used for dispatch; held re-firing is the Hold
    // tick's job (see ActivationMode + BindingSystem::tickHolds).
    bool operator==(const KeyChord &) const = default;
};

// A single digital gamepad button (face buttons, D-pad, shoulders, stick clicks, start/back/guide).
struct PadButton {
    SDL_GamepadButton button = SDL_GAMEPAD_BUTTON_INVALID;
    bool operator==(const PadButton &) const = default;
};

// One half of an analog gamepad axis used as a (continuous) hold source. A bidirectional stick axis
// (LEFTX/Y, RIGHTX/Y) is split into two triggers by `positive`; the unidirectional triggers
// (LEFT/RIGHTTRIGGER, 0..1) are always `positive`. BindingSystem polls the axis each frame, applies a
// deadzone + smoothing, and drives the bound command's tick with HoldTickInfo::analog = the deflection
// magnitude past the deadzone (see tickAnalog).
struct PadAxis {
    SDL_GamepadAxis axis = SDL_GAMEPAD_AXIS_INVALID;
    bool positive = true;
    bool operator==(const PadAxis &) const = default;
};

using Trigger = std::variant<KeyChord, PadButton, PadAxis>;

// ── Modifier ──────────────────────────────────────────────────────────────────

// An optional secondary *gamepad* input that must be *held* for a binding to be eligible — a held pad
// button or an analog stick/trigger half deflected past the deadzone. `monostate` ⇒ no modifier (the
// binding is always eligible). While a modifier is held, a binding that requires it *shadows* an
// unmodified binding sharing the same trigger — so the same stick/button can drive one command normally
// and another while the gate is held (see BindingSystem::modifiedLayerActive). This is a gamepad-only
// layer: a keyboard chord already carries its own Ctrl/Shift/Alt, so keyboard bindings use that, not a
// held-gate modifier.
using Modifier = std::variant<std::monostate, PadButton, PadAxis>;

// ── Activation mode ───────────────────────────────────────────────────────────

// Press fires run() once on key-down. Hold ticks the command every frame while the key is down
// (BindingSystem::tickHolds), until key-up or interruption. Mode is a property of the *binding*, so a
// command can be bound Press in one preset and Hold in another — but it only does something in Hold
// mode if its Command supplies a `tick` (Command::holdable()).
enum class ActivationMode : uint8_t { Press, Hold };

// ── Binding ──────────────────────────────────────────────────────────────────

// Maps one input Trigger to a command id. Multiple Bindings may share the same commandId
// (many-to-one); a trigger resolves to at most one command (enforced by BindingSystem).
struct Binding {
    Trigger trigger;
    std::string commandId;
    Modifier modifier; // optional held secondary input (monostate ⇒ none)
    ActivationMode mode = ActivationMode::Press;
};

// ── Binding presets ───────────────────────────────────────────────────────────

// One entry in the preset list: a named, savable/loadable binding set. `slug` is the
// filename-safe id; `builtin` presets ship read-only under the install dir, user presets live in
// the pref dir and are mutable. A user preset shadows a built-in of the same slug.
struct PresetInfo {
    std::string name;
    std::string slug;
    bool builtin = false;
};

// ── Discrete hold cadence ─────────────────────────────────────────────────────

// Parameters for holdRepeats(): every former OS-repeat command wants the same feel — fire
// immediately, brief delay, then a steady (optionally accelerating) cadence the app owns.
struct HoldRepeatParams {
    float initialDelay = 0.40f; // seconds before the second fire
    float interval = 0.06f;     // seconds between repeats after the delay
    float accel = 1.0f;         // <1 shrinks interval over time (e.g. 0.9 per repeat); 1 = steady
};

// Maximum sustained hold-repeat cadence (Hz): an accelerating (accel<1) hold's gap floors here, and
// it's the absolute rate ceiling regardless of params. Stays below the per-frame burst clamp at any
// sane frame rate (<16 fires/frame down to ~16 fps), keeping the cadence frame-rate independent.
// Shared with the Shortcut-window cadence preview so the displayed ceiling can't drift from the floor.
inline constexpr double kHoldRepeatMaxRateHz = 250.0;

// Number of discrete fires whose scheduled time falls in (elapsed-dt, elapsed]. Schedule: a fire at
// t=0 (immediate), then at initialDelay, initialDelay+interval, … with interval scaled by accel^k.
// Stateless: depends only on (elapsed, dt, params). Returns *all* fires in the window (a catch-up
// burst when the interval is shorter than a frame), clamped to a small ceiling so pathological params
// can't emit an unbounded burst in one frame.
int holdRepeats(float elapsed, float dt, const HoldRepeatParams &p);

// ── Rebind capture state ─────────────────────────────────────────────────────

// Shared between BindingSystem (producer) and ShortcutWindow (consumer). OfsApp owns this
// and passes it by reference to both.
struct RebindState {
    std::string targetCommandId; // non-empty = capture active
    bool capturing = false;
    Trigger captured; // valid when hasResult == true (KeyChord, PadButton, or PadAxis)
    bool hasResult = false;
    // Modifier-capture mode: when true the captured input is assigned as the *modifier* of an existing
    // binding (identified by modifierTarget) rather than creating a new binding. The held-gate modifier
    // is gamepad-only, so capture finalizes on a PadButton/PadAxis source; keyboard input is ignored
    // (see BindingSystem::onKeyDown).
    bool captureModifier = false;
    Trigger modifierTarget; // the trigger of the binding whose modifier we're setting
    // Re-capture mode: when true the captured input *replaces* an existing binding's trigger
    // (identified by replaceTarget) — the old trigger is removed and the new one inherits the old
    // binding's mode/modifier — rather than adding a second binding. Mutually exclusive with
    // captureModifier.
    bool replaceTrigger = false;
    Trigger replaceTarget; // the trigger of the binding being re-captured
};

// ── BindingSystem ─────────────────────────────────────────────────────────────

// Owns the flat Binding table plus load/save. Replaces ShortcutSystem.
// Keyboard dispatch: subscribes to KeyDownEvent (registered in constructor, before freeze()).
// All methods are main-thread-only except push(); EventQueue::push() is thread-safe.
class BindingSystem {
  public:
    BindingSystem(EventQueue &eq, CommandRegistry &registry, RebindState &rebindState);

    // Register a code-default binding. Call during init, before loadBindings().
    // SDLK_UNKNOWN = no default trigger (command stays unbound until user assigns one).
    void addDefault(const std::string &commandId, KeyChord chord, ActivationMode mode = ActivationMode::Press);

    // Binding management — called from BindingWindow.
    void addBinding(Binding b);
    void removeBinding(const Trigger &trigger, const std::string &commandId);

    // Set the activation mode of one matching binding. No-op if the trigger+command isn't bound.
    void setMode(const Trigger &trigger, const std::string &commandId, ActivationMode mode);

    // Set (or clear, with monostate) the held modifier of one matching binding. No-op if the
    // trigger+command isn't bound.
    void setModifier(const Trigger &trigger, const std::string &commandId, const Modifier &modifier);

    // Persistence (new versioned format).
    void loadBindings();
    void saveBindings() const;

    // Binding presets. The active bindings.json is the only thing dispatch reads; a preset is a
    // snapshot you load into it or save it out to. No "current preset" tracking — loading overwrites
    // the active set and the relationship ends there.
    [[nodiscard]] std::vector<PresetInfo> listPresets() const; // deduped union, user shadows built-in
    // Overwrite the active set from a preset, then saveBindings(). Returns the number of entries that could
    // not be bound (a command this build lacks / a custom command not defined here / an unknown trigger
    // type) — they are kept verbatim for round-trip but never dispatch, so the caller can warn the user.
    int loadPreset(const std::string &slug);
    void saveActiveAsPreset(const std::string &name); // write current bindings_ to a user preset
    void deletePreset(const std::string &slug);       // user presets only; no-op on a built-in

    // Restore all bindings to the registered code defaults and persist.
    void resetToDefaults();

    // First trigger for a command (for palette hint and rebind-window display).
    // Returns nullptr if the command has no bindings.
    [[nodiscard]] const Trigger *findHint(const std::string &commandId) const;

    // True if `commandId` has at least one binding whose (non-empty) modifier is being held right
    // now. The simulator overlay uses this to light up the alt-DOF label (Surge/Twist) when its
    // modifier trigger is down — derived from the live bindings, so a rebind of the modifier updates
    // the badge automatically (no hardcoded button).
    [[nodiscard]] bool isModifierActiveFor(const std::string &commandId) const;

    // Advance every active hold by dt and run its command's tick. Called once per rendered frame from
    // OfsApp::onUpdate, in the update phase (after drain(), beside the other service updates). A tick
    // may push events that drain() applies next frame — the standard one-frame latency.
    void tickHolds(float dt);

    // Tunables for analog (stick/trigger) hold sources: a scaled deadzone that kills jitter at rest,
    // and an exponential low-pass time constant that smooths the noisy raw signal.
    struct AnalogConfig {
        float deadzone = 0.15f; // 0..1; below this the axis reads as idle
        float smoothing = 0.5f; // low-pass time constant in seconds; larger = smoother but laggier
    };
    void setAnalogConfig(AnalogConfig c) { analogConfig_ = c; }

    // Poll-driven counterpart to tickHolds for PadAxis triggers. OfsApp samples every gamepad axis
    // (normalized: sticks ±1, triggers 0..1) and calls this each frame before tickHolds. Smooths the
    // raw values, applies the deadzone, and starts/advances/ends a continuous hold per PadAxis binding
    // whose deflection is in the bound direction — passing the post-deadzone magnitude as the hold's
    // analog value. `padCaptured` (ImGui gamepad-nav active) suppresses dispatch, like onGamepadButton.
    void tickAnalog(const std::array<float, SDL_GAMEPAD_AXIS_COUNT> &raw, bool padCaptured, float dt);

    // Drop all active holds. Called on window focus loss and when entering rebind capture — contexts
    // that may never deliver a held input's release.
    void clearHolds();

    // Drop only *keyboard* holds. The ImGui keyboard-capture rising edge (a text field focusing)
    // strands a held key's key-up, so its hold must be cancelled — but gamepad button-up and analog
    // return are always delivered, so those holds end naturally. Clearing them here would wrongly kill
    // a held frame-step the moment ImGui grabs keyboard focus, which gamepad nav does on L1/R1 (it
    // raises WantCaptureKeyboard while cycling window focus). Source-targeted so pad holds survive.
    void clearKeyHolds();

    // True while any Hold is in flight (held key/button, or a deflected analog axis). A held input
    // emits no further SDL events, so the app's idle path must not sleep while this is set — otherwise
    // tickHolds/tickAnalog stop running and the hold freezes mid-gesture.
    [[nodiscard]] bool hasActiveHolds() const { return !activeHolds_.empty(); }

    [[nodiscard]] const std::vector<Binding> &bindings() const { return bindings_; }
    [[nodiscard]] RebindState &rebindState() { return rebindState_; }

    // Called by BindingSystem's internal event handlers; exposed for testing.
    void onKeyDown(const KeyDownEvent &event);
    void onKeyUp(const KeyUpEvent &event);
    void onGamepadButton(const GamepadButtonEvent &event);
    void onGamepadButtonUp(const GamepadButtonUpEvent &event);

  private:
    static SDL_Keymod normalizeModifiers(SDL_Keymod mods);
    // True for the standalone modifier keycodes (Left/Right Ctrl/Shift/Alt/GUI). A lone modifier
    // key-down must not finalize a rebind capture — see onKeyDown.
    static bool isModifierKey(SDL_Keycode key);

    // Group bindings_ by Trigger variant type (KeyChord, then PadButton, then PadAxis) so the Shortcut
    // window renders a stable type order without sorting per frame. Runs only when bindings_ membership
    // changes (load/add), never in the input or render hot path.
    void sortByTriggerType();

    // Is this modifier input held right now? monostate ⇒ true; PadButton ⇒ button down;
    // PadAxis ⇒ smoothed deflection past the deadzone in the bound direction.
    [[nodiscard]] bool modifierHeld(const Modifier &m) const;
    // Does any enabled binding for this exact trigger require a modifier that is held? If so, the
    // plain (unmodified) bindings on the same trigger are suppressed so the modified layer wins.
    [[nodiscard]] bool modifiedLayerActive(const Trigger &trigger) const;
    // Eligibility gate shared by all three dispatch paths: the binding's modifier is held, and it is
    // not an unmodified binding being shadowed by an active modified layer on the same trigger.
    [[nodiscard]] bool modifierEligible(const Binding &b) const;

    // Capture-apply orchestration — the Shortcut window's former apply* helpers, moved here so the UI
    // routes a finished capture through ApplyBindingCaptureEvent rather than mutating the table itself.
    // Each persists. applyCapture adds the trigger, auto-reassigning it off any conflicting command.
    // applyRecapture swaps an existing binding's trigger, carrying its mode + (non-keyboard) modifier.
    // applyModifier narrows the captured pad input to a Modifier and sets it on the target binding.
    void applyCapture(const std::string &commandId, const Trigger &trigger);
    void applyRecapture(const std::string &commandId, const Trigger &oldTrigger, const Trigger &captured);
    void applyModifier(const std::string &commandId, const Trigger &targetTrigger, const Trigger &captured);

    EventQueue &eventQueue_;
    CommandRegistry &commandRegistry_;
    RebindState &rebindState_;

    std::vector<Binding> bindings_;
    std::vector<Binding> defaults_; // code defaults, restored when no file exists

    // One entry per active Hold binding (keyboard key, gamepad button, or analog axis). tickHolds
    // advances `elapsed` and forwards `analog` into HoldTickInfo. Release matches on the source
    // identity alone (key / button / axis+dir) — a modifier change mid-hold must not strand it.
    struct ActiveHold {
        enum class Src : uint8_t { Key, PadButton, PadAxis };
        Src src = Src::Key;
        SDL_Keycode key = SDLK_UNKNOWN;
        SDL_GamepadButton button = SDL_GAMEPAD_BUTTON_INVALID;
        SDL_GamepadAxis axis = SDL_GAMEPAD_AXIS_INVALID;
        bool positive = true;
        std::string commandId;
        float elapsed = 0.0f;
        float analog = 1.0f; // 1.0 for digital keys/buttons; deflection magnitude (0..1) for analog axes
    };
    std::vector<ActiveHold> activeHolds_;

    AnalogConfig analogConfig_;
    // Per-axis low-pass state for tickAnalog, indexed by SDL_GamepadAxis. Single active pad (last
    // opened); a multi-pad setup would need per-SDL_JoystickID state.
    std::array<float, SDL_GAMEPAD_AXIS_COUNT> axisSmoothed_{};
    // Latest *raw* (unsmoothed) axis values, captured each tickAnalog. The analog *velocity* rides
    // axisSmoothed_, but a modifier's binary held-state must use the raw value — the smoothing time
    // constant (~0.5 s) would otherwise leave the modifier "on" for nearly a second after release.
    std::array<float, SDL_GAMEPAD_AXIS_COUNT> axisRaw_{};
    // Live held-state of digital pad buttons, for PadButton modifiers (the analog half reads
    // axisRaw_). Updated in onGamepadButton / onGamepadButtonUp before any early return.
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> padButtonsDown_{};

    // Triggers (with activation mode) loaded from bindings.json, keyed by command id. Used by the G4
    // shim so plugin commands (registered after loadBindings()) get their saved bindings.
    struct LoadedBinding {
        Trigger trigger;
        Modifier modifier;
        ActivationMode mode = ActivationMode::Press;
    };
    std::map<std::string, std::vector<LoadedBinding>> loadedOverrides_;

    // Raw JSON text of the entries the last loadPreset() could not bind (a command this build lacks, or
    // an unknown trigger type). Re-emitted verbatim by saveActiveAsPreset so an older build doesn't
    // silently strip a newer user's bindings on edit-and-resave. Cleared by loadBindings() /
    // resetToDefaults() — the active bindings.json is per-machine and preserves nothing.
    std::vector<std::string> presetUnapplied_;
};

} // namespace ofs
