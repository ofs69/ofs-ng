#include <imgui_te_engine.h>

void RegisterProcessingTests(ImGuiTestEngine *);
void RegisterUndoTests(ImGuiTestEngine *);
void RegisterDialogsTests(ImGuiTestEngine *);
void RegisterModalsTests(ImGuiTestEngine *);
void RegisterProjectDialogsTests(ImGuiTestEngine *);
void RegisterPlaybackTests(ImGuiTestEngine *);
void RegisterTimelineTests(ImGuiTestEngine *);
void RegisterPluginsTests(ImGuiTestEngine *);
void RegisterWindowTests(ImGuiTestEngine *);
void RegisterSimulatorTests(ImGuiTestEngine *);
void RegisterVideoControlsTests(ImGuiTestEngine *);
void RegisterCommandPaletteTests(ImGuiTestEngine *);
void RegisterBandBarTests(ImGuiTestEngine *);
void RegisterLayoutTests(ImGuiTestEngine *);
void RegisterNoProjectTests(ImGuiTestEngine *);
void RegisterWelcomeTests(ImGuiTestEngine *);
void RegisterNotificationsTests(ImGuiTestEngine *);
void RegisterFooterSelectorTests(ImGuiTestEngine *);
void RegisterMetadataTests(ImGuiTestEngine *);
void RegisterThemeTests(ImGuiTestEngine *);
void RegisterConfigTests(ImGuiTestEngine *);
void RegisterProjectSettingsTests(ImGuiTestEngine *);
void RegisterMenusTests(ImGuiTestEngine *);
void RegisterShortcutTests(ImGuiTestEngine *);
void RegisterMultiAxisTests(ImGuiTestEngine *);
void RegisterTranscodeTests(ImGuiTestEngine *);
void RegisterUpdatesTests(ImGuiTestEngine *);
void RegisterPluginUiTests(ImGuiTestEngine *);

void RegisterAllTests(ImGuiTestEngine *engine) {
    RegisterProcessingTests(engine);
    RegisterUndoTests(engine);
    RegisterDialogsTests(engine);
    RegisterModalsTests(engine);
    RegisterProjectDialogsTests(engine);
    RegisterPlaybackTests(engine);
    RegisterTimelineTests(engine);
    RegisterPluginsTests(engine);
    RegisterWindowTests(engine);
    RegisterSimulatorTests(engine);
    RegisterVideoControlsTests(engine);
    RegisterCommandPaletteTests(engine);
    RegisterBandBarTests(engine);
    RegisterLayoutTests(engine);
    RegisterNoProjectTests(engine);
    RegisterWelcomeTests(engine);
    RegisterNotificationsTests(engine);
    RegisterFooterSelectorTests(engine);
    RegisterMetadataTests(engine);
    RegisterThemeTests(engine);
    RegisterConfigTests(engine);
    RegisterProjectSettingsTests(engine);
    RegisterMenusTests(engine);
    RegisterShortcutTests(engine);
    RegisterMultiAxisTests(engine);
    RegisterTranscodeTests(engine);
    RegisterUpdatesTests(engine);
    // Registered last: loads the real C# plugin into the live app; keeping it after the other suites
    // means no other suite runs with the plugin loaded.
    RegisterPluginUiTests(engine);
}
