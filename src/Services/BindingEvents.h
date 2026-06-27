#pragma once

#include "Services/BindingSystem.h" // Trigger, Modifier, ActivationMode

#include <string>

namespace ofs {

// Write-API to BindingSystem's persisted binding table + preset store, pushed by the Shortcut window
// (UI mutates no service directly). BindingSystem is the sole subscriber to each; every
// handler persists (saveBindings / preset file) so the UI never calls save itself. These carry the
// Trigger/Modifier types BindingSystem owns, so they live beside it rather than in Core/.
//
// Capture handshake: *starting* a capture is an event (BeginBindingCaptureEvent below) — the window asks,
// BindingSystem sets up RebindState. The window then raises the modal on the same one-frame-deferred
// schedule, so the modal opens the frame the drained state lands, never against empty state. The capture
// modal's own in-flight transitions (cancel / re-capture) and the input-side writes (onKeyDown/onPadButton
// recording the pressed trigger, the gamepad-axis sampler in OfsApp) stay direct on the shared RebindState
// — a synchronous read-modify loop that must not lag a frame. The finished outcome is ApplyBindingCaptureEvent.

// Begin a binding capture: BindingSystem populates RebindState (capturing, no result yet) and the window
// raises the capture modal next frame. One struct covers all three starts — add a new binding
// (commandId only), re-capture an existing trigger (replaceTrigger + replaceTarget), or set a held
// modifier (captureModifier + modifierTarget).
struct BeginBindingCaptureEvent {
    std::string commandId;
    bool captureModifier = false;
    Trigger modifierTarget; // captureModifier: the binding whose modifier we're capturing
    bool replaceTrigger = false;
    Trigger replaceTarget; // replaceTrigger: the binding being re-captured
};

// Apply a finished trigger capture. Mirrors the resolved RebindState: a fresh capture (add the
// binding, auto-reassigning the trigger off any command already holding it), a re-capture
// (replaceTrigger: swap an existing binding's trigger, carrying its mode + held modifier), or a
// modifier capture (captureModifier: assign the captured pad input as an existing binding's held
// modifier). BindingSystem owns the conflict-resolution + mode/modifier-carry orchestration.
struct ApplyBindingCaptureEvent {
    std::string commandId;
    Trigger captured;
    bool captureModifier = false;
    Trigger modifierTarget; // captureModifier: the binding whose modifier we're setting
    bool replaceTrigger = false;
    Trigger replaceTarget; // replaceTrigger: the binding being re-captured
};

// Remove one binding (trigger + command).
struct RemoveBindingEvent {
    Trigger trigger;
    std::string commandId;
};

// Set the activation mode (Press / Hold) of one binding.
struct SetBindingModeEvent {
    Trigger trigger;
    std::string commandId;
    ActivationMode mode = ActivationMode::Press;
};

// Set or clear (monostate) the held modifier of one binding.
struct SetBindingModifierEvent {
    Trigger trigger;
    std::string commandId;
    Modifier modifier;
};

// Save the active bindings as a named user preset.
struct SaveBindingPresetEvent {
    std::string name;
};

// Overwrite the active bindings from a preset (by slug). The handler emits a NotifyEvent summarizing
// the load and any entries it could not bind; `name` is the display name for that message.
struct LoadBindingPresetEvent {
    std::string slug;
    std::string name;
};

// Delete a user preset by slug (built-ins are ignored).
struct DeleteBindingPresetEvent {
    std::string slug;
};

// Restore every binding to its registered code default and persist (the "Reset Shortcuts?" action).
struct ResetBindingsEvent {};

} // namespace ofs
