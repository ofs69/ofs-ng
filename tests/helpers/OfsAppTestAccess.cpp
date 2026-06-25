#include "OfsAppTestAccess.h"

#include "App/OfsApp.h"
#include "Services/CommandRegistry.h"
#include "helpers/TestState.h"
// Complete types needed to bind references through the unique_ptrs / upcast the dummy player.
#include "UI/ProcessingPanel.h"
#include "Video/DummyVideoPlayer.h"

ofs::PluginManager &OfsAppTestAccess::pluginManager(OfsApp &app) {
    return *app.pluginManager;
}
ofs::CommandRegistry &OfsAppTestAccess::commandRegistry(OfsApp &app) {
    return app.commandRegistry;
}
ofs::BindingSystem &OfsAppTestAccess::bindingSystem(OfsApp &app) {
    return *app.bindingSystem;
}
ofs::EffectRegistryState &OfsAppTestAccess::effectRegistry(OfsApp &app) {
    return app.effectRegistry;
}
ofs::ScriptRegistryState &OfsAppTestAccess::scriptRegistry(OfsApp &app) {
    return app.scriptRegistry;
}
ofs::ui::NotificationState &OfsAppTestAccess::notifications(OfsApp &app) {
    return app.notifications;
}
ofs::VideoPlayer *OfsAppTestAccess::videoPlayer(OfsApp &app) {
    return app.dummyPlayer.get();
}
const ofs::AppSettings &OfsAppTestAccess::appSettings(OfsApp &app) {
    return app.appSettings;
}
ofs::ProcessingPanel &OfsAppTestAccess::processingPanel(OfsApp &app) {
    return *app.processingPanel;
}

std::string localizedCommandTitle(const char *commandId) {
    const ofs::CommandRegistry &reg = *getTestState().commandRegistry;
    const ofs::Command *cmd = reg.find(commandId);
    return cmd != nullptr ? std::string(cmd->title.c_str()) : std::string();
}

std::string localizedGroupName(const char *groupKey) {
    return std::string(getTestState().commandRegistry->groupDisplayName(groupKey));
}
