#pragma once

#include "UI/Theme.h"
#include "imgui.h"
#include <string>
#include <vector>

namespace ofs {

struct AppSettings;
struct ScriptProject;
class EventQueue;

class ConfigurationWindow {
  public:
    explicit ConfigurationWindow(const AppSettings &appSettings);

    void render(const ScriptProject &project, EventQueue &eq, bool &open);

  private:
    void renderApplicationTab(EventQueue &eq);
    void renderSimulatorTab(const ScriptProject &project, EventQueue &eq);
    void renderThemeTab(const ScriptProject &project, EventQueue &eq);

    const AppSettings &appSettings;
    std::vector<std::string> availableLanguages;        // refreshed on window open + when the combo opens
    bool langComboOpen_ = false;                        // tracks the language combo's open edge for a one-shot rescan
    std::vector<ofs::theme::ThemeInfo> availableThemes; // refreshed on open + after save/delete
    std::string themeName_;                             // Save-As / Duplicate name input (grows; UTF-8 safe)
    std::string themeFilter_;                           // Theme-tab color/var fuzzy search box (UTF-8 safe)
    bool prevOpen = false;
    bool openHwdec = false;
    int fontSizeEdit_ = 0; // live buffer for the font-size InputInt; seeded on open, committed on edit-end
};

} // namespace ofs
