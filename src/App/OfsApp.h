#pragma once

#include "App/Application.h"
#include "Core/AppState.h"
#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/IntentEvents.h"
#include "Core/ScriptProject.h"
#include "Core/StandardAxis.h"
#include "Core/TranscodeEvents.h"
#include "Core/VectorSet.h"
#include "Core/WaveformEvents.h"
#include "Format/AppSettings.h"
#include "Format/LayoutStore.h"
#include "Services/BindingSystem.h"
#include "Services/CommandProviders.h"
#include "Services/CommandRegistry.h"
#include "Services/CustomCommandStore.h"
#include "Services/CustomCommandTemplate.h"
#include "Services/EditModeRegistry.h"
#include "Services/EffectRegistry.h"
#include "Services/JobSystem.h"
#include "Services/NavigatorRegistry.h"
#include "Services/PluginManager.h"
#include "Services/ScriptRegistry.h"
#include "Services/SelectionModeRegistry.h"
#include "UI/Notifications.h"
#include "UI/TitleBar.h"
#include "Video/VideoPlayer.h"
#include <SDL3/SDL_events.h>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace ofs {
class VideoPlayer;
class MpvVideoPlayer;
class DummyVideoPlayer;
class VideoPreview;
class TimelinePreviewPopup;
class VideoPlayerWindow;
class ProjectManager;
class NavigatorRouter;
class EditIntentRouter;
class SelectIntentRouter;
class UndoSystem;
class ConfigurationWindow;
class ProjectConfigWindow;
class ScriptSimulator;
class ScriptStatisticsWindow;
class LogWindow;
class AboutWindow;
class ScriptTimelineWindow;
class VideoControlsWindow;
class ShortcutWindow;
class ProcessingSystem;
class ScriptSystem;
class VideoTranscoder;
class ProcessingPanel;
class ModalManager;
class WelcomeScreen;
class WaveformService;
class WaveformRenderer;
} // namespace ofs

class OfsApp : public ofs::Application {
  public:
    OfsApp(ofs::ScriptProject &project, ofs::EventQueue &eq);
    ~OfsApp() override;
    bool init() override;

    // Test-only access to private service handles, exposed through a friend struct whose accessor
    // bodies live in the test tree (tests/helpers/OfsAppTestAccess.cpp). Keeps test plumbing out of
    // this header; a friend declaration emits no code, so shipping builds are unaffected.
    friend struct OfsAppTestAccess;

  protected:
    void onImGuiRender() override;
    void onUpdate(float dt) override;
    void onPostRender() override;
    void onStartupComplete() override;
    void onEvent(SDL_Event *event) override;
    bool canAppIdle() const override;
    bool shouldDropAmbientEvent(const SDL_Event &event) const override;
    float fontSizeBase() const override;
    int frameCapFps() const override;
    bool isVideoPlaybackActive() const override;

    // Frame-loop hook (see Application): supplies the title and routes the caption-button actions to
    // the window. All window mechanics live in Window; this only translates UI intent.
    void renderTitleBar() override;

  private:
    void renderMainMenuBar(); // the menu bar shell: File/Edit/View inline, the larger menus below
    // The three substantial menus + the right-aligned title block, split out of renderMainMenuBar. Each
    // owns its own BeginMenu/EndMenu; `hasProject` is computed once by the caller and threaded through.
    void renderLayoutMenu();
    void renderAxesMenu(bool hasProject);
    void renderPluginsMenu(bool hasProject);
    void renderMenuBarTitle(); // right-aligned project title + dirty marker + last-saved age
    void renderEditor();       // the dockspace + all editor windows; rendered only with an active project
#if !defined(NDEBUG) && !defined(OFS_TEST_PREF_SUBDIR)
    // Debug overlay: while ImGui holds the keyboard, draw the window path to the focus owner.
    void renderFocusPathDebugOverlay();
#endif
    void renderFooterBar();          // bottom status strip; gathers telemetry and calls ui::renderFooterBar
    void renderToolOptions();        // docked "Tool Options" panel: the active modes' onUi
    bool renderActiveToolSections(); // draws all active edit/nav/select onUi as collapsing sections (panel only)
    // Draws one extension point's active-mode onUi (no header) for the per-mode click-away modal; false if
    // that mode has no options. `toolOptionTitle` is the modal title — the active mode's display name.
    bool renderToolOptionForTarget(ofs::ToolOptionTarget target);
    [[nodiscard]] std::string toolOptionTitle(ofs::ToolOptionTarget target) const;
    // Maps a ToolOptionTarget to its (registry, active-mode id) pair and invokes `fn(registry, activeId)`,
    // returning its result. Centralizes the target→extension-point dispatch the tool-options paths share.
    template <class F> decltype(auto) withToolTarget(ofs::ToolOptionTarget target, F &&fn) const {
        switch (target) {
        case ofs::ToolOptionTarget::Edit:
            return fn(editModeRegistry, scriptProject.activeEditMode);
        case ofs::ToolOptionTarget::Navigator:
            return fn(navigatorRegistry, scriptProject.activeNavigator);
        case ofs::ToolOptionTarget::Selection:
            return fn(selectionModeRegistry, scriptProject.activeSelectionMode);
        }
        return fn(editModeRegistry, scriptProject.activeEditMode); // unreachable; keeps a defined return path
    }
    bool renderExportFunscriptBody(); // interior of the Export modal; returns true to close
    void openExportFunscriptModal();  // populate axis options + show the Export modal
    void onQuickExport();             // replay the project's last export, or open the modal if none yet
    void openTranscodeOptionsModal(); // build + show the intra-frame optimize options modal
    void promptForMissingIntraDir();  // alert + force re-pick when the configured output folder is gone
    void pickIntraOutputDir();        // open the folder picker, persist the choice, re-open the options modal
    void maybeOfferOptimize();        // consume optimizePromptPending once media is ready; offer the prompt
    bool renderNewLayoutBody();       // interior of the New Layout modal; returns true to close
    void saveActiveLayout();          // snapshot current dock arrangement into the active preset
    void revertToActiveLayout();      // reload the active layout's saved state, discarding live tweaks
    void initCommands();              // registers all Commands + default KeyChord bindings
    void onSeekEvent(const ofs::SeekEvent &event);

    // Live global hold-repeat cadence, read fresh each tick so Shortcut-window edits apply instantly.
    [[nodiscard]] ofs::HoldRepeatParams holdRepeatParams() const {
        return {.initialDelay = appSettings.holdRepeat.initialDelay,
                .interval = appSettings.holdRepeat.interval,
                .accel = appSettings.holdRepeat.accel,
                .maxRateHz = appSettings.holdRepeat.maxRateHz};
    }

    struct ExportFunscriptDialog {
        bool isOpen = false;
        int format = 2;
        struct AxisOption {
            ofs::StandardAxis role = ofs::StandardAxis::L0;
            bool selected = false;
        };
        std::vector<AxisOption> options;
    };
    ExportFunscriptDialog exportFunscriptDialog;

    ofs::JobSystem jobSystem;
    ofs::EventQueue &eventQueue;
    ofs::ScriptProject &scriptProject;
    ofs::EffectRegistryState effectRegistry;
    ofs::ScriptRegistryState scriptRegistry;

    ofs::AppSettings appSettings;
    // App-settings dirty flag: tracks unsaved AppSettings (global preferences). Deliberately named
    // to NOT be confused with the project dirty flag `project.state.settingsDirty` (set via
    // ProjectManager::setDirty), which tracks unsaved changes to the open .ofs document. The two are
    // independent: this one flushes appSettings.save(); that one drives the project save prompt.
    // Set by the ModifyEvent<AppSettings> handler; onUpdate() flushes one appSettings.save() per
    // frame when set, so preference edits never write the file synchronously from render.
    bool appSettingsDirty_ = false;
    // Previous frame's ImGui::GetIO().WantCaptureKeyboard, for detecting the false→true rising edge
    // that must clear active key-holds (a text field stealing focus mid-hold).
    bool prevWantCaptureKeyboard_ = false;
    ofs::LayoutStore layoutStore;
    ofs::AppState appState;

#ifndef NDEBUG
    // Toggled from the View menu (debug builds only); drives ImGui::ShowMetricsWindow. Transient,
    // not persisted — defaults off each launch.
    bool showImGuiMetrics_ = false;
#endif

    // Docking-layout chrome. Menu clicks set a pending flag; the actual ImGui ini apply / default
    // rebuild runs at the top of the next onUpdate() — before windows are submitted that frame.
    std::string newLayoutName_;
    bool focusNewLayoutName_ = false;
    std::string pendingLayoutIni_;
    bool pendingLayoutApply_ = false;
    bool pendingDefaultReset_ = false;
    ofs::RebindState rebindState;
    ofs::CommandRegistry commandRegistry;
    ofs::ui::CommandPaletteState paletteState;
    ofs::ui::NotificationState notifications;  // footer bell log; fed by on<NotifyEvent>
    bool commandPaletteOpenRequested = false;  // latched by OpenCommandPaletteEvent, consumed by renderTitleBar
    bool transcodeDialogOpenRequested = false; // latched by OpenTranscodeDialogEvent, consumed in onImGuiRender
    // Source resolution/fps for the optimize modal's preview, filled asynchronously by VideoTranscoder's
    // ffprobe (RequestMediaInfoEvent → MediaInfoReadyEvent). The path guards against a result arriving for
    // a source other than the one the modal is currently showing.
    std::optional<ofs::MediaInfo> transcodeSourceInfo;
    std::string transcodeSourceInfoPath;
    // Latched on LoadProjectEvent; consumed once the opened project's video is actually loaded, to offer
    // optimizing an unoptimized source. Deferred (not handled at load) because the video loads async.
    bool optimizePromptPending = false;

    // Which mode's options to open, latched by OpenToolOptionsEvent (footer affordance or palette command)
    // and consumed in onImGuiRender to raise the click-away modal (showCustomModal). Latched (not raised
    // inline) so the modal is built inside a live ImGui frame.
    std::optional<ofs::ToolOptionTarget> openToolOptionsModalTarget_;

    // Palette-only dynamic commands (chapters / bookmarks / scratch-axis delete), merged into the palette
    // alongside the static registry. Rebuilt only when navSignature changes so renderTitleBar stays heap-free.
    std::vector<ofs::Command> dynamicPaletteCommands;
    uint64_t navSignature = 0;

    // Registry-resident provider commands (source=Dynamic, opt-in bindable): mode switches, axis select/toggle,
    // and tool options. Rebuilt into the CommandRegistry whenever the mode set, scratch-axis existence, or
    // UI language changes — off the per-frame path. Seeded to a value the real signature can't take (it is
    // non-zero) so the first call always builds.
    uint64_t providerCommandSignature_ = 0;
    void refreshProviderCommands();

    std::shared_ptr<ofs::VideoPlayer> player;
    std::shared_ptr<ofs::MpvVideoPlayer> mpvPlayer;
    std::shared_ptr<ofs::DummyVideoPlayer> dummyPlayer;

    std::unique_ptr<ofs::PluginManager> pluginManager;

    std::unique_ptr<ofs::VideoPlayerWindow> videoPlayerWindow;
    std::unique_ptr<ofs::ProjectManager> projectManager;
    std::unique_ptr<ofs::UndoSystem> undoSystem;
    std::unique_ptr<ofs::BindingSystem> bindingSystem;
    // The kind menu for custom commands: each template owns its editor UI, factory, and (de)serialization
    // (see registerBuiltinCommandTemplates). Declared before customCommandStore / shortcutWindow, which
    // hold a const ref to it, so it outlives them. Populated once in init().
    ofs::CustomCommandTemplateRegistry customCommandTemplates;
    // Owns user-defined custom commands + custom_commands.json; registers them into commandRegistry as
    // source=Custom. Constructed after initCommands(), loaded before bindingSystem->loadBindings().
    std::unique_ptr<ofs::CustomCommandStore> customCommandStore;
    // Declared after the registry it references so the router destructs first. Sole StepRequestEvent
    // subscriber — resolves a step into a SeekEvent through the active navigator.
    ofs::NavigatorRegistry navigatorRegistry;
    std::unique_ptr<ofs::NavigatorRouter> navigatorRouter;
    // Sole EditRequestEvent subscriber — resolves an edit intent into the existing mutation events
    // through the active edit mode. Registry declared first so the router destructs before it.
    ofs::EditModeRegistry editModeRegistry;
    std::unique_ptr<ofs::EditIntentRouter> editIntentRouter;
    // Sole SelectRequestEvent subscriber — resolves a selection gesture per-axis through the active
    // selection mode. Registry declared first so the router destructs before it.
    ofs::SelectionModeRegistry selectionModeRegistry;
    std::unique_ptr<ofs::SelectIntentRouter> selectIntentRouter;

    double sessionTime = 0.0;
    std::map<SDL_JoystickID, SDL_Gamepad *> gamepads_; // opened on GAMEPAD_ADDED, closed on GAMEPAD_REMOVED

    std::unique_ptr<ofs::ConfigurationWindow> configWindow;
    std::unique_ptr<ofs::ProjectConfigWindow> projectConfigWindow;
    std::unique_ptr<ofs::ScriptTimelineWindow> scriptTimelineWindow;
    std::unique_ptr<ofs::VideoControlsWindow> videoControlsWindow;
    std::unique_ptr<ofs::ScriptSimulator> scriptSimulator;
    std::unique_ptr<ofs::ScriptStatisticsWindow> scriptStatisticsWindow;
    std::unique_ptr<ofs::LogWindow> logWindow;
    std::unique_ptr<ofs::AboutWindow> aboutWindow;
    std::unique_ptr<ofs::ShortcutWindow> shortcutWindow;
    std::unique_ptr<ofs::ProcessingSystem> processingSystem;
    std::unique_ptr<ofs::ScriptSystem> scriptSystem;
    std::unique_ptr<ofs::VideoTranscoder> videoTranscoder;
    std::unique_ptr<ofs::ProcessingPanel> processingPanel;
    std::unique_ptr<ofs::WelcomeScreen> welcomeScreen;

    // Audio waveform behind the timeline: the service extracts/caches peaks and owns the GL texture; the
    // renderer (declared after, so it destructs first — it holds a reference to the service) owns the
    // shader and draws. Both use GL, which is safe in their dtors since the base Application (and its GL
    // context) outlives all OfsApp members.
    std::unique_ptr<ofs::WaveformService> waveformService;
    std::unique_ptr<ofs::WaveformRenderer> waveformRenderer;

    // Second mpv instance + its popup renderer for the seek-bar hover frame preview. Declared after
    // mpvPlayer so they destruct first, while the GL context is still alive (the engine deletes its
    // FBO/texture in its destructor).
    std::unique_ptr<ofs::VideoPreview> videoPreview;
    std::unique_ptr<ofs::TimelinePreviewPopup> previewPopup;

    // Declared LAST so it destructs FIRST: ~ModalManager calls handle.destroy() on suspended
    // flow coroutines, which run frame-local RAII that may reference the services above. Those
    // services must still be alive when this is torn down.
    std::unique_ptr<ofs::ModalManager> modalManager;
};
