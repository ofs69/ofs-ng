#pragma once

namespace ofs {
class EventQueue;
struct AppSettings;

// Full-viewport "no project loaded" screen. Pure renderer: reads recent paths from AppSettings and
// pushes project-open events; owns no project state. OfsApp renders this in place of the editor
// dockspace whenever ProjectManager::hasActiveProject() is false, so the editor windows never have to
// defend against absent project state themselves.
class WelcomeScreen {
  public:
    void render(EventQueue &eq, const AppSettings &settings);
};
} // namespace ofs
