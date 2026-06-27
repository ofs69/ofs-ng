#pragma once

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keycode.h>

namespace ofs {

// Posted by OfsApp::onEvent from SDL_EVENT_KEY_DOWN. keyboardCaptured is set from
// ImGui::GetIO().WantCaptureKeyboard at the time the SDL event fires (previous frame's ImGui
// state). Processed during drain(); BindingSystem is the sole subscriber.
struct KeyDownEvent {
    SDL_Keycode key;
    SDL_Keymod modifiers;
    bool repeat;
    bool keyboardCaptured = false;
};

// Posted by OfsApp::onEvent from SDL_EVENT_KEY_UP, mirroring KeyDownEvent. No keyboardCaptured field:
// a key-up must always be delivered so an in-flight Hold binding cannot get stuck when ImGui capture
// changes mid-press. BindingSystem is the sole subscriber (ends the matching active hold).
struct KeyUpEvent {
    SDL_Keycode key;
    SDL_Keymod modifiers;
};

// Posted by OfsApp::onEvent from SDL_EVENT_GAMEPAD_BUTTON_DOWN. gamepadCaptured mirrors
// ImGui::GetIO().WantCaptureGamepad at event time. BindingSystem is the sole subscriber.
struct GamepadButtonEvent {
    SDL_JoystickID which;
    SDL_GamepadButton button;
    bool gamepadCaptured = false;
};

// Posted by OfsApp::onEvent from SDL_EVENT_GAMEPAD_BUTTON_UP, mirroring KeyUpEvent. Always delivered
// (no capture gate) so an in-flight Hold binding on a gamepad button cannot get stuck. BindingSystem
// is the sole subscriber (ends the matching active hold).
struct GamepadButtonUpEvent {
    SDL_JoystickID which;
    SDL_GamepadButton button;
};

// Requests the title-bar command palette to open this frame. Pushed by the rebindable "open command
// palette" shortcut; OfsApp latches it and forwards it to renderTitleBar.
struct OpenCommandPaletteEvent {};

} // namespace ofs
