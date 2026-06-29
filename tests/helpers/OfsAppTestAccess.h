#pragma once

// Test-only window into OfsApp's private service handles. OfsApp grants this struct friendship (a
// single line in the production header); every accessor body lives in OfsAppTestAccess.cpp, which is
// compiled only into the ui/fuzz test targets. This keeps OfsApp's header free of test plumbing.
// See PluginManagerTestAccess for the same pattern applied to the plugin manager.

class OfsApp;

namespace ofs {
class PluginManager;
class CommandRegistry;
class BindingSystem;
class VideoPlayer;
class ProcessingPanel;
struct AppSettings;
struct EffectRegistryState;
struct ScriptRegistryState;
namespace ui {
struct NotificationState;
}
} // namespace ofs

struct OfsAppTestAccess {
    static ofs::PluginManager &pluginManager(OfsApp &app);
    static ofs::CommandRegistry &commandRegistry(OfsApp &app);
    static ofs::BindingSystem &bindingSystem(OfsApp &app);
    static ofs::EffectRegistryState &effectRegistry(OfsApp &app);
    static ofs::ScriptRegistryState &scriptRegistry(OfsApp &app);
    static ofs::ui::NotificationState &notifications(OfsApp &app);
    // The dummy player: the active player once a project loads, and stable for the app's lifetime
    // (unlike `player`, which is reassigned between mpv/dummy).
    static ofs::VideoPlayer *videoPlayer(OfsApp &app);
    static const ofs::AppSettings &appSettings(OfsApp &app);
    static ofs::ProcessingPanel &processingPanel(OfsApp &app);

    // Invoke the protected runtime DPI-change hook. The real trigger (SDL_GetWindowDisplayScale changing
    // mid-session) can't be produced in the headless test window, so the layout suite calls this after
    // setting style.FontScaleDpi to the simulated scale.
    static void dispatchDisplayScaleChanged(OfsApp &app, float newScale);
};
