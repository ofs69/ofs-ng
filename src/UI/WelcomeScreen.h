#pragma once

#include <memory>

namespace ofs {
class EventQueue;
class FogBackground;
struct AppSettings;

// Full-viewport "no project loaded" screen. Pure renderer: reads recent paths from AppSettings and
// pushes project-open events; owns no project state. OfsApp renders this in place of the editor
// dockspace whenever ProjectManager::hasActiveProject() is false, so the editor windows never have to
// defend against absent project state themselves.
class WelcomeScreen {
  public:
    WelcomeScreen();
    ~WelcomeScreen();

    void render(EventQueue &eq, const AppSettings &settings);

  private:
    // Animated fog backdrop. Owns a GLSL shader, so it is constructed with the rest of OfsApp's UI once
    // the GL context exists; harmless on the headless backend (the shader simply compiles nothing).
    std::unique_ptr<FogBackground> fog_;
};
} // namespace ofs
