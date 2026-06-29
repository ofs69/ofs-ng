#pragma once

#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/ProjectLifecycleEvents.h"
#include "Core/ScriptProject.h"
#include "Services/EffectRegistry.h"
#include <filesystem>
#include <imgui_te_context.h>
#include <string>

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

struct TestSharedState {
    ofs::ScriptProject *project = nullptr;
    ofs::EventQueue *eventQueue = nullptr;
    // Live app subsystems, populated by OfsTestApp::init(). Let plugin-UI tests load a real plugin
    // into the running app and read back its commands / nodes / bindings.
    ofs::PluginManager *pluginManager = nullptr;
    ofs::CommandRegistry *commandRegistry = nullptr;
    ofs::BindingSystem *bindingSystem = nullptr;
    ofs::EffectRegistryState *effectRegistry = nullptr;
    // Compiled-script registry, so the script-node crash test can await a Roslyn compile before
    // wiring the script into a region graph.
    ofs::ScriptRegistryState *scriptRegistry = nullptr;
    ofs::ui::NotificationState *notifications = nullptr;
    // The active (dummy) video player, so transport tests can assert play/pause + speed directly.
    ofs::VideoPlayer *videoPlayer = nullptr;
    // The live AppSettings, so config-window tests can read back a toggled preference.
    const ofs::AppSettings *appSettings = nullptr;
    // The Processing panel, so node-editor tests can drive deleteSelected() over a real imnodes selection.
    ofs::ProcessingPanel *processingPanel = nullptr;
    // The running app, so the layout suite can drive the protected DPI-change hook directly (the real
    // SDL display-scale trigger can't be simulated in the headless test window).
    OfsApp *app = nullptr;
};

// Populated by OfsTestApp::init() before any test runs.
TestSharedState &getTestState();

// Active-language display title of a command (what the palette search box shows and fuzzy-matches),
// or "" if the id is unknown. Lets palette tests type the localized query instead of a hardcoded
// English string, so they pass under ui-smoke-loc where native titles are translated.
std::string localizedCommandTitle(const char *commandId);

// Active-language display name of a command group (the palette's "Group:" prefix, also folded into its
// fuzzy-match haystack with no English fallback), or the raw key if the group is unregistered. Dynamic
// commands (chapters/bookmarks/axes) aren't in the registry, so pair this with the language-independent
// axis code to query one, e.g. localizedGroupName("Select Axis") + " S0".
std::string localizedGroupName(const char *groupKey);

// Load the standard fixture project.
// Clears dirty flags first so the "unsaved changes" dialog is never shown when
// a previous test left the project in a dirty state.
static inline void loadFixture(ImGuiTestContext *ctx) {
    getTestState().project->clearDirtyFlags();
    // The .ofp is now decoded on a JobSystem worker and applied when the flow resumes. Clear filePath
    // up front so the wait below blocks until THIS load lands (it is set on success) — a prior test may
    // have left it pointing at the same fixture, which would otherwise make the wait return instantly
    // while the new load is still in flight (its doClose() would then race the caller's next event).
    getTestState().project->state.filePath.clear();
    const std::filesystem::path dir(OFS_TESTS_DIR);
    getTestState().eventQueue->push(ofs::OpenProjectRequestEvent{(dir / "fixtures" / "basic.ofp").string()});
    for (int i = 0; i < 600 && getTestState().project->state.filePath.empty(); ++i)
        ctx->Yield();
}
