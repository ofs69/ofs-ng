#pragma once

#include "Services/BindingSystem.h" // Trigger, Modifier, ActivationMode

#include <string>

namespace ofs {

// Write-API to BindingSystem's persisted binding table + preset store, pushed by the Shortcut window
// (UI mutates no service directly). BindingSystem is the sole subscriber to each; every
// handler persists (saveBindings / preset file) so the UI never calls save itself. These carry the
// Trigger/Modifier types BindingSystem owns, so they live beside it rather than in Core/.
//
// The transient rebind *capture* handshake (RebindState) is deliberately NOT routed here: it is a
// synchronous shared struct co-owned by the window and BindingSystem (both write it, incl. onKeyDown),
// and the capture modal reads it the same frame it opens — deferring those writes by a frame would
// close the modal before the state landed. Only the binding-table outcome of a capture is an event.

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
