#include "OfsApp.h"
#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/IntentEvents.h"
#include "Core/StandardAxis.h"
#include "Core/TaskEvents.h"
#include "Format/AppSettings.h"
#include "Localization/AxisNames.h"
#include "Localization/Translator.h"
#include "Services/BindingSystem.h"
#include "Services/EditIntentRouter.h"
#include "Services/EffectRegistry.h"
#include "Services/ManagedHttp.h"
#include "Services/NavigatorRouter.h"
#include "Services/ProcessingSystem.h"
#include "Services/ProjectManager.h"
#include "Services/ScriptSystem.h"
#include "Services/SelectIntentRouter.h"
#include "Services/UiSoundService.h"
#include "Services/UndoSystem.h"
#include "Services/UpdateChecker.h"
#include "Services/VideoTranscoder.h"
#include "Services/WaveformService.h"
#include "UI/AboutWindow.h"
#include "UI/BackupRestoreWindow.h"
#include "UI/ConfigurationWindow.h"
#include "UI/DockLayout.h"
#include "UI/FooterBar.h"
#include "UI/Icons.h"
#include "UI/ImGuiHelpers.h"
#include "UI/LogWindow.h"
#include "UI/ModalManager.h"
#include "UI/ProcessingPanel.h"
#include "UI/ProjectConfigWindow.h"
#include "UI/ScriptSimulator.h"
#include "UI/ScriptStatistics.h"
#include "UI/ScriptTimeline.h"
#include "UI/ShortcutWindow.h"
#include "UI/Theme.h"
#include "UI/TimelinePreviewPopup.h"
#include "UI/TitleBar.h"
#include "UI/VideoPlayerControls.h"
#include "UI/VideoPlayerWindow.h"
#include "UI/WaveformRenderer.h"
#include "UI/WelcomeScreen.h"
#include "Util/FileFingerprint.h"
#include "Util/FrameAllocator.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"
#include "Util/Subprocess.h"
#include "Video/DummyVideoPlayer.h"
#include "Video/MpvVideoPlayer.h"
#include "Video/VideoPlayer.h"
#include "Video/VideoPreview.h"
#include "imgui.h"
#include "imgui_stdlib.h"

#include <OfsBuildInfo.h> // generated: git tag baked into the binary (welcome footer)
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <system_error>

namespace {
// Formats a Trigger (KeyChord, PadButton, or PadAxis) into a frame-scratch hint for the palette.
const char *formatTriggerHint(const ofs::Trigger &t) {
    if (const auto *kc = std::get_if<ofs::KeyChord>(&t)) {
        const char *ctrl = (kc->modifiers & SDL_KMOD_CTRL) != 0 ? "Ctrl+" : "";
        const char *shift = (kc->modifiers & SDL_KMOD_SHIFT) != 0 ? "Shift+" : "";
        const char *alt = (kc->modifiers & SDL_KMOD_ALT) != 0 ? "Alt+" : "";
        const char *gui = (kc->modifiers & SDL_KMOD_GUI) != 0 ? "GUI+" : "";
        return fmtScratch("{}{}{}{}{}", ctrl, shift, alt, gui, SDL_GetKeyName(kc->key));
    }
    if (const auto *pb = std::get_if<ofs::PadButton>(&t)) {
        const char *name = SDL_GetGamepadStringForButton(pb->button);
        return name ? fmtScratch("[Pad] {}", name) : "[Pad] ?";
    }
    if (const auto *pa = std::get_if<ofs::PadAxis>(&t)) {
        const char *name = SDL_GetGamepadStringForAxis(pa->axis);
        return name ? fmtScratch("[Pad] {}{}", name, pa->positive ? "+" : "-") : "[Pad] ?";
    }
    return "";
}

// The undo memory budget in bytes from the user setting (MB), floored at 1 MB so a stray 0 can't leave
// only the newest step undoable.
size_t undoMemoryBytes(const ofs::AppSettings &s) {
    return static_cast<size_t>(std::max(1, s.undoMemoryLimitMb)) << 20;
}

// Fills a frame-scratch FooterSelectOption[] from one extension-point registry's entries. Works for any
// of the three (edit mode / navigator / selection mode) — they share the {id, displayName, owningPlugin,
// onUi} entry shape. The native entry (matched by id) shows the localized `nativeLabel`; a plugin entry
// shows its own display name verbatim, falling back to its id. Returns the array and writes the count.
template <class Registry>
ofs::ui::FooterSelectOption *buildFooterOptions(const Registry &registry, const char *nativeId, const char *nativeLabel,
                                                int &count) {
    const auto &entries = registry.entries();
    auto *opts = ofs::FrameAllocator::instance().allocArray<ofs::ui::FooterSelectOption>(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        opts[i].id = entries[i].id.c_str();
        opts[i].label = entries[i].id == nativeId         ? nativeLabel
                        : !entries[i].displayName.empty() ? entries[i].displayName.c_str()
                                                          : entries[i].id.c_str();
        opts[i].source = entries[i].owningPlugin.c_str(); // empty for native → no source shown
        opts[i].hasUi = entries[i].onUi != nullptr;
    }
    count = static_cast<int>(entries.size());
    return opts;
}

} // namespace

OfsApp::OfsApp(ofs::ScriptProject &project, ofs::EventQueue &eq)
    : eventQueue(eq), scriptProject(project), commandRegistry(eq) {}

bool OfsApp::init() {
    if (!Application::init())
        return false;

    appSettings = ofs::AppSettings::load();
    layoutStore = ofs::LayoutStore::load();

    // imgui.ini is disabled (see Application::initImGui), so restore the active layout ourselves here
    // — before the first NewFrame, so ImGui builds the dock nodes from it. "Default" needs no ini: the
    // dockspace builds the hardcoded arrangement on first frame when no node exists yet.
    if (layoutStore.activeLayoutName != "Default") {
        for (const auto &preset : layoutStore.layouts) {
            if (preset.name == layoutStore.activeLayoutName) {
                ImGui::LoadIniSettingsFromMemory(preset.ini.data(), preset.ini.size());
                break;
            }
        }
    }

    // Apply the saved language before any UI renders. English (the baked-in defaults) is always
    // available even if this is skipped or fails; we never auto-detect from the OS locale.
    if (!appSettings.language.empty() && appSettings.language != "en")
        ofs::loc::Translator::instance().load(appSettings.language);

    // The CJK glyph font is deferred (loadFonts loads only Latin + icons). If the UI language is itself
    // CJK, the menus/labels need those glyphs on the very first frame — load it now, on the critical
    // path, rather than letting the whole UI render as boxes during the deferred window. For any other
    // language it stays deferred to onStartupComplete(), so the common case pays nothing for it.
    {
        // Match the primary subtag so script/region variants (e.g. "zh-Hant", "zh-Hans") still trigger
        // the CJK glyph load — an exact "zh" compare would miss them and render the menus as boxes.
        const std::string &culture = ofs::loc::Translator::instance().activeCulture();
        const std::string_view primary(culture.c_str(), std::min(culture.find('-'), culture.size()));
        if (primary == "ja" || primary == "zh" || primary == "ko")
            loadCjkFont();
    }

    {
        ofs::theme::Theme t;
        if (!ofs::theme::load(appSettings.activeTheme, &t)) {
            ofs::theme::makeDarkTheme(&t); // missing/unknown active theme → shipped dark
            appSettings.activeTheme = t.name;
        }
        ofs::theme::apply(t); // post-apply hook re-captures the DPI base (Application::onThemeApplied)
    }

    mpvPlayer = std::make_shared<ofs::MpvVideoPlayer>(appSettings.hwdecEnabled, eventQueue);
    dummyPlayer = std::make_shared<ofs::DummyVideoPlayer>(eventQueue);
    player = mpvPlayer;

    for (size_t i = 0; i < ofs::kStandardAxisCount; ++i)
        scriptProject.axes[i].role = static_cast<ofs::StandardAxis>(i);

    scriptProject.simulator = appSettings.simulator;

    ofs::registerNativeEffects(effectRegistry);

    eventQueue.on<ofs::SeekEvent>([this](const ofs::SeekEvent &e) { onSeekEvent(e); });
    eventQueue.on<ofs::QuickExportEvent>([this](const ofs::QuickExportEvent &) { onQuickExport(); });
    eventQueue.on<ofs::ExitConfirmedEvent>([this](const ofs::ExitConfirmedEvent &) { stop(); });
    eventQueue.on<ofs::OpenCommandPaletteEvent>(
        [this](const ofs::OpenCommandPaletteEvent &) { commandPaletteOpenRequested = true; });
    // Latched (not opened here): the options modal is built in onImGuiRender where an ImGui frame is
    // live, so its width can be font-relative like every other modal.
    eventQueue.on<ofs::OpenTranscodeDialogEvent>(
        [this](const ofs::OpenTranscodeDialogEvent &) { transcodeDialogOpenRequested = true; });
    eventQueue.on<ofs::MediaInfoReadyEvent>([this](const ofs::MediaInfoReadyEvent &e) {
        if (e.path == transcodeSourceInfoPath)
            transcodeSourceInfo = e.info;
    });
    // Arm the optimize prompt whenever a project loads or the media changes; maybeOfferOptimize() waits
    // for the async video load to settle, then its canOffer gate (original source, no existing intra copy)
    // decides whether to actually show it — so arming on a switch *to* the intra copy is harmless. Without
    // the media-change arm, opening a second video after dismissing the prompt once never re-offered.
    eventQueue.on<ofs::LoadProjectEvent>([this](const ofs::LoadProjectEvent &) { optimizePromptPending = true; });
    eventQueue.on<ofs::ChangeMediaPathEvent>(
        [this](const ofs::ChangeMediaPathEvent &) { optimizePromptPending = true; });

    // Footer notification center. NotifyEvent is the generic push channel; script-compile failures
    // are wired here too so Roslyn errors surface in the UI instead of only the log.
    eventQueue.on<ofs::NotifyEvent>([this](const ofs::NotifyEvent &e) { notifications.push(e.level, e.message); });
    // Generic non-blocking background-task indicators (waveform extraction, …): services push these to
    // surface a footer progress entry instead of a blocking modal. The handlers only touch UI state.
    eventQueue.on<ofs::TaskStartedEvent>(
        [this](const ofs::TaskStartedEvent &e) { notifications.startTask(e.id, e.label, e.cancellable); });
    eventQueue.on<ofs::TaskProgressEvent>(
        [this](const ofs::TaskProgressEvent &e) { notifications.updateTask(e.id, e.detail, e.progress); });
    eventQueue.on<ofs::TaskEndedEvent>([this](const ofs::TaskEndedEvent &e) { notifications.endTask(e.id); });
    // Footer affordance / palette command → latch which mode's options to open; onImGuiRender raises the
    // click-away modal inside a live frame (so its width is font-relative, like every showCustomModal).
    eventQueue.on<ofs::OpenToolOptionsEvent>(
        [this](const ofs::OpenToolOptionsEvent &e) { openToolOptionsModalTarget_ = e.target; });
    eventQueue.on<ofs::ScriptCompiledEvent>([this](const ofs::ScriptCompiledEvent &e) {
        if (!e.ok)
            notifications.push(ofs::NotifyLevel::Error, Str::AppScriptCompileFailed.fmt(e.fileName, e.error));
    });

    // Localization: load + persist the selected language; export/refresh tooling (main thread,
    // infrequent file I/O — off the render hot path).
    eventQueue.on<ofs::SetLanguageEvent>([this](const ofs::SetLanguageEvent &e) {
        ofs::loc::Translator::instance().load(e.languageId);
        appSettings.language = e.languageId;
        appSettings.save();
    });
    // The single write path for AppSettings: the UI is handed a const AppSettings& and pushes a
    // mutator here. We apply it and mark dirty; onUpdate() flushes the save once per frame so no
    // settings edit writes the file synchronously from render.
    eventQueue.on<ofs::ModifyEvent<ofs::AppSettings>>([this](const ofs::ModifyEvent<ofs::AppSettings> &e) {
        e.apply(appSettings);
        appSettingsDirty_ = true;
        // Lazily create/tear down the preview engine when the toggle flips. Pushed (not called
        // directly) so the mpv create/destroy runs in the engine's drained handler, between frames.
        if (videoPreview && videoPreview->isEnabled() != appSettings.showTimelinePreview)
            eventQueue.push(ofs::SetPreviewEnabledEvent{appSettings.showTimelinePreview});
        // Apply the pause-on-seek policy live; the player caches the flag (cheap to re-push).
        eventQueue.push(ofs::SetPauseOnSeekEvent{appSettings.pauseOnSeek});
        // Apply analog input tunables live (e.g. dragging the deadzone/smoothing sliders).
        if (bindingSystem)
            bindingSystem->setAnalogConfig(
                {.deadzone = appSettings.input.deadzone, .smoothing = appSettings.input.smoothing});
        // Apply the undo memory budget live (shrinking it trims the history immediately).
        if (undoSystem)
            undoSystem->setMaxBytes(undoMemoryBytes(appSettings));
    });
    eventQueue.on<ofs::ExportCatalogEvent>([this](const ofs::ExportCatalogEvent &e) {
        const bool ok = ofs::loc::Translator::instance().exportCatalog(e.path);
        notifications.push(ok ? ofs::NotifyLevel::Success : ofs::NotifyLevel::Error,
                           ok ? Str::AppCatalogExported.c_str() : Str::AppCatalogExportFailed.c_str());
    });
    eventQueue.on<ofs::RefreshTranslationEvent>([this](const ofs::RefreshTranslationEvent &e) {
        const bool ok = ofs::loc::Translator::instance().refreshTranslation(e.path);
        notifications.push(ok ? ofs::NotifyLevel::Success : ofs::NotifyLevel::Error,
                           ok ? Str::AppTranslationRefreshed.c_str() : Str::AppTranslationRefreshFailed.c_str());
    });
    // OfsApp owns AppSettings, so the recent-projects list (shown on the welcome screen) is cleared here.
    // Persisted on the next settings save; the in-memory clear is what the welcome screen reflects now.
    eventQueue.on<ofs::ClearRecentProjectsEvent>(
        [this](const ofs::ClearRecentProjectsEvent &) { appSettings.lastProjectPaths.clear(); });
    eventQueue.on<ofs::RemoveRecentProjectEvent>(
        [this](const ofs::RemoveRecentProjectEvent &e) { std::erase(appSettings.lastProjectPaths, e.path); });
    // Keep the recent-projects list current as projects are opened/saved during the session (not just
    // for the one project open at exit). Most-recent-first, de-duplicated, capped at five.
    eventQueue.on<ofs::RememberRecentProjectEvent>([this](const ofs::RememberRecentProjectEvent &e) {
        if (e.path.empty())
            return;
        auto &paths = appSettings.lastProjectPaths;
        std::erase(paths, e.path);
        paths.insert(paths.begin(), e.path);
        if (paths.size() > 5)
            paths.resize(5);
        // A project is now the live session — re-arm the auto-reopen a prior explicit close may have cleared.
        appSettings.reopenLastProject = true;
    });
    // Explicit user close: don't reopen this project on the next launch. Persist promptly (not just at
    // exit) so the suppression survives a crash before shutdown.
    eventQueue.on<ofs::ProjectClosedEvent>([this](const ofs::ProjectClosedEvent &) {
        appSettings.reopenLastProject = false;
        appSettingsDirty_ = true;
    });

    if (mpvPlayer->init()) {
        dummyPlayer->init();
        eventQueue.on<ofs::ChangeDummyDurationEvent>(
            [this](const ofs::ChangeDummyDurationEvent &) { player = dummyPlayer; });
        eventQueue.on<ofs::LoadVideoEvent>([this](const ofs::LoadVideoEvent &) { player = mpvPlayer; });
        eventQueue.on<ofs::CloseVideoEvent>([this](const ofs::CloseVideoEvent &) { player = mpvPlayer; });

        eventQueue.push(ofs::VolumeChangedEvent{appSettings.volume});
        eventQueue.push(ofs::SetPauseOnSeekEvent{appSettings.pauseOnSeek});
        videoPlayerWindow = std::make_unique<ofs::VideoPlayerWindow>(eventQueue);
        undoSystem = std::make_unique<ofs::UndoSystem>(scriptProject, eventQueue, undoMemoryBytes(appSettings));
        projectManager =
            std::make_unique<ofs::ProjectManager>(scriptProject, eventQueue, appSettings, jobSystem, effectRegistry);
        processingSystem = std::make_unique<ofs::ProcessingSystem>(scriptProject, effectRegistry, scriptRegistry,
                                                                   eventQueue, jobSystem);
        // Constructed here (before freeze) so its event handlers register; init() (loading the
        // Roslyn host) happens below alongside the plugin host.
        scriptSystem = std::make_unique<ofs::ScriptSystem>(scriptProject, scriptRegistry, eventQueue, jobSystem);
        // Intra-frame transcoder — registers its handlers before freeze(); runs ffmpeg on JobSystem workers.
        videoTranscoder = std::make_unique<ofs::VideoTranscoder>(scriptProject, eventQueue, jobSystem);
        // Audio-waveform extraction — registers handlers before freeze(); runs ffmpeg on JobSystem workers.
        waveformService = std::make_unique<ofs::WaveformService>(scriptProject, eventQueue, jobSystem);
        // UI feedback sounds — subscribes to NotifyEvent before freeze(); reads enable/volume from
        // appSettings live, so it must outlive nothing but the event queue.
        uiSoundService = std::make_unique<ofs::UiSoundService>(eventQueue, appSettings);

        // reopenLastProject is cleared by an explicit user close, so a deliberately-closed project stays
        // closed across a restart (it remains in lastProjectPaths for the welcome screen's recent list).
        if (appSettings.reopenLastProject && !appSettings.lastProjectPaths.empty() &&
            std::filesystem::exists(ofs::util::fromUtf8(appSettings.lastProjectPaths.front()))) {
            eventQueue.push(ofs::OpenProjectRequestEvent{appSettings.lastProjectPaths.front()});
        }
    }

    bindingSystem = std::make_unique<ofs::BindingSystem>(eventQueue, commandRegistry, rebindState);
    // Sole StepRequestEvent subscriber (the navigation seam); registers in its ctor before freeze().
    navigatorRouter = std::make_unique<ofs::NavigatorRouter>(scriptProject, eventQueue, navigatorRegistry);
    // Sole EditRequestEvent subscriber (the editing seam); registers in its ctor before freeze().
    editIntentRouter = std::make_unique<ofs::EditIntentRouter>(scriptProject, eventQueue, editModeRegistry);
    // Sole SelectRequestEvent subscriber (the selection seam); registers in its ctor before freeze().
    selectIntentRouter = std::make_unique<ofs::SelectIntentRouter>(scriptProject, eventQueue, selectionModeRegistry);
    scriptTimelineWindow = std::make_unique<ofs::ScriptTimelineWindow>();
    // Owns the waveform shader; reads the service's GL texture. Created here (after the GL context exists)
    // so the shader compiles, and before the timeline renders it.
    waveformRenderer = std::make_unique<ofs::WaveformRenderer>(*waveformService);
    videoControlsWindow = std::make_unique<ofs::VideoControlsWindow>(eventQueue);
    previewPopup = std::make_unique<ofs::TimelinePreviewPopup>();
    // init() only registers handlers + loads the mpv lib (must run before freeze() below); the second
    // mpv instance is created lazily once the feature is enabled.
    videoPreview = std::make_unique<ofs::VideoPreview>(appSettings.hwdecEnabled, eventQueue);
    videoPreview->init();
    if (appSettings.showTimelinePreview)
        eventQueue.push(ofs::SetPreviewEnabledEvent{true});
    scriptSimulator = std::make_unique<ofs::ScriptSimulator>(eventQueue);
    scriptStatisticsWindow = std::make_unique<ofs::ScriptStatisticsWindow>();
    logWindow = std::make_unique<ofs::LogWindow>();
    aboutWindow = std::make_unique<ofs::AboutWindow>();
    backupRestoreWindow = std::make_unique<ofs::BackupRestoreWindow>();
    welcomeScreen = std::make_unique<ofs::WelcomeScreen>();
    // Register the custom-command kind menu (step / move-position / move-time) before the store and the
    // Shortcut window, both of which hold the registry by const reference. project & appSettings are
    // captured into the templates' build/render closures, so they must outlive the registry (they do).
    ofs::registerBuiltinCommandTemplates(customCommandTemplates, scriptProject, appSettings);
    // Constructed before the Shortcut window, which holds it by const reference to read custom-command
    // definitions for its editor. Definitions are registered later by customCommandStore->load() (after
    // initCommands), and its event handlers register here, before freeze().
    customCommandStore = std::make_unique<ofs::CustomCommandStore>(eventQueue, commandRegistry, customCommandTemplates);
    shortcutWindow = std::make_unique<ofs::ShortcutWindow>(commandRegistry, *bindingSystem, *customCommandStore,
                                                           customCommandTemplates);
    configWindow = std::make_unique<ofs::ConfigurationWindow>(appSettings);
    projectConfigWindow = std::make_unique<ofs::ProjectConfigWindow>(appSettings);
    processingPanel = std::make_unique<ofs::ProcessingPanel>();
    // Registers its CheckForUpdatesEvent / result handlers here, before freeze().
    updateChecker = std::make_unique<ofs::UpdateChecker>(eventQueue, jobSystem);
    // Surface the update-check outcome as a footer notification: always for an available update (the
    // discovery path for the silent startup check), and — only for a user-initiated check — a confirming
    // "up to date" / failure toast so a command-palette run isn't silent. The UpdateChecker itself owns
    // the About-window status; these handlers add the transient toast on top.
    eventQueue.on<ofs::UpdateAvailableEvent>([this](const ofs::UpdateAvailableEvent &e) {
        eventQueue.push(
            ofs::NotifyEvent{.level = ofs::NotifyLevel::Info, .message = Str::AboutUpdateAvailable.fmt(e.version)});
    });
    eventQueue.on<ofs::UpdateUpToDateEvent>([this](const ofs::UpdateUpToDateEvent &e) {
        if (e.userInitiated)
            eventQueue.push(
                ofs::NotifyEvent{.level = ofs::NotifyLevel::Success, .message = Str::AboutUpToDate.c_str()});
    });
    eventQueue.on<ofs::UpdateCheckFailedEvent>([this](const ofs::UpdateCheckFailedEvent &e) {
        if (e.userInitiated)
            eventQueue.push(
                ofs::NotifyEvent{.level = ofs::NotifyLevel::Warning, .message = Str::AboutUpdateFailed.c_str()});
    });
    // Registers on<ShowModalEvent> in its ctor — must happen before freeze() below.
    modalManager = std::make_unique<ofs::ModalManager>(eventQueue);

    if (videoPlayerWindow && scriptSimulator) {
        videoPlayerWindow->setOverlayCallback([this](ImDrawList *dl, const ofs::OverlayViewport &vp, bool vpHovered) {
            if (!appSettings.showSimulator)
                return false;
            return scriptSimulator->renderOverlay(dl, scriptProject, eventQueue, vp, vpHovered);
        });
    }

    initCommands();

    // Register the user's custom commands into the registry now — after the native commands (which
    // include the "Custom" group), before loadBindings() below — so a saved binding to custom.<n>
    // resolves on first boot. (The store itself was constructed earlier, alongside the windows that
    // read it; its event handlers registered then, before freeze().)
    customCommandStore->load();

    // Construct the plugin manager now (its ctor registers event handlers, which must happen before
    // freeze() below), but defer the expensive init()/loadPlugins() — .NET CoreCLR bring-up plus per-DLL
    // load — to onStartupComplete(), after the first frame is on screen. Plugin-contributed commands and
    // edit modes therefore appear a frame late; onStartupComplete() re-runs refreshProviderCommands() and
    // loadBindings() so bindings to those commands still resolve.
    pluginManager = std::make_unique<ofs::PluginManager>(scriptProject, eventQueue, player, dummyPlayer,
                                                         commandRegistry, *bindingSystem, effectRegistry);

    // scriptSystem->init() (Roslyn host load) is deferred to onStartupComplete() alongside the plugin
    // host: its handlers registered in the ctor (before freeze), and `ready` gates all use, so script
    // compilation simply waits until the host is up — first-frame rendering needs none of it.

    // Register the registry-resident provider commands (mode switches, axis select/toggle, tool options)
    // before loadBindings, so a saved binding to e.g. mode.edit.<id> or nav.axis.<x> resolves on first boot.
    refreshProviderCommands();

    // Load bindings after all commands (native + provider) are registered so saved bindings apply. Plugin
    // commands aren't loaded yet (deferred); onStartupComplete() reloads bindings once they are.
    bindingSystem->loadBindings();
    bindingSystem->setAnalogConfig({.deadzone = appSettings.input.deadzone, .smoothing = appSettings.input.smoothing});

    // All handlers registered — lock the map against further writes, then start worker threads.
    eventQueue.freeze();
    jobSystem.start();

    window->enableCustomTitleBar();

    // Restore the persisted window geometry onto the still-hidden window (Application::run shows it after
    // the first frame). Validated against the live displays inside restoreGeometry; an unset geometry
    // leaves the start-size heuristic from Window::init in place.
    const auto &wg = appSettings.windowGeometry;
    window->restoreGeometry({.x = wg.x, .y = wg.y, .width = wg.width, .height = wg.height, .maximized = wg.maximized});

    return true;
}

void OfsApp::onStartupComplete() {
    // Everything here is deferred from init() so the window paints first; it runs while the window is on
    // screen (briefly frozen) rather than in front of it.

    // Merge the CJK glyph font (no-op if init() already loaded it eagerly for a CJK UI language). For a
    // Latin-script UI this keeps its inflate off the critical path; CJK content (e.g. a filename or
    // metadata) renders as boxes for this brief deferred window, then resolves.
    loadCjkFont();

    // Load the plugin host (.NET CoreCLR + per-DLL load) and the Roslyn script host. Deferring here is
    // also safer than init(): a plugin trust modal can now drive re-entrant frames because the app is
    // fully initialized and the worker pool is running (see Application::run).
    if (pluginManager && pluginManager->init())
        pluginManager->loadPlugins();
    if (scriptSystem)
        scriptSystem->init();

    // Plugins may have registered commands and edit/navigator/selection modes. Rebuild the provider
    // commands and re-resolve bindings so a saved binding to a plugin command takes effect now.
    refreshProviderCommands();
    bindingSystem->loadBindings();

    // Bring up the .NET-backed HTTP backend (its own CoreCLR load of Ofs.HostServices) so the update
    // checker has a network path. Non-fatal if it fails — the checker then just reports no network.
    ofs::initManagedHttp();

    // Silent update check (opt-out): fired here, not in init(), so it overlaps the rest of startup and
    // never delays the first frame. The result surfaces only if a newer release exists (userInitiated=false).
    if (appSettings.checkForUpdatesOnStartup)
        eventQueue.push(ofs::CheckForUpdatesEvent{.userInitiated = false});
}

OfsApp::~OfsApp() {
    // Stop all background work before any service is torn down. Worker threads evaluate plugin and script
    // nodes through trampolines that deref ScriptSystem::hostApi / a plugin's HostApi by raw pointer; since
    // members destruct after this body (and jobSystem, declared first, destructs last), a job still running
    // here would otherwise outlive those services and a faulting node would report through a freed HostApi —
    // an intermittent AccessViolation seen at headless-test shutdown. Quiescing first closes that window.
    // First signal the fire-and-forget workers (ffmpeg-backed extraction/encode) to abort: shutdown()
    // blocks on pool->wait(), and these tasks only stop when their cancel flag is set — without this a
    // long decode/encode stalls close until it finishes on its own.
    if (waveformService)
        waveformService->cancelInFlight();
    if (videoTranscoder)
        videoTranscoder->cancelInFlight();
    jobSystem.shutdown();

    for (auto &[id, gpad] : gamepads_)
        SDL_CloseGamepad(gpad);
    gamepads_.clear();

    // The recent-projects list is maintained live via RememberRecentProjectEvent as projects are
    // opened/saved, so the open-at-exit project is already at the front — nothing to do here.

    // project.simulator is the live copy edited during the session (ranges/display via the config
    // window, transform via the simulator window); mirror the whole thing back for persistence.
    appSettings.simulator = scriptProject.simulator;

    if (mpvPlayer)
        appSettings.volume = mpvPlayer->getVolume();

    if (window) {
        const auto g = window->currentRestoreGeometry();
        appSettings.windowGeometry = {
            .x = g.x, .y = g.y, .width = g.width, .height = g.height, .maximized = g.maximized};
    }

    appSettings.save();
    if (pluginManager) {
        pluginManager->savePluginStates();
        // Tear plugins down while PluginManager (and the hostApi/callCtx_ the managed host holds raw
        // pointers into) is still alive. After savePluginStates so the persisted enabled flags are
        // untouched by teardown. shutdown() is idempotent — the destructor calls it again as a backstop.
        pluginManager->shutdown();
    }
}

void OfsApp::onUpdate(float dt) {
    eventQueue.drain();

    // Commit (or drop) the undo snapshot of any discrete edit applied during this drain: a real edit
    // becomes undoable this frame, a no-op edit (e.g. a region create that found no room) records none.
    if (undoSystem)
        undoSystem->endFrame();

    // Resume any flow whose modal was answered last frame, before service updates run. Runs in
    // the update phase so resumed business logic / ScriptProject mutations stay out of render.
    if (modalManager)
        modalManager->pump();

    // Keep the registry-resident provider commands in step with the mode set / scratch-axis existence / UI
    // language (all can change via events just drained — plugin load/unload, scratch create/delete, language
    // switch). Cheap signature poll; only rebuilds the registry on an actual change, off the per-frame path.
    refreshProviderCommands();

    // Apply a pending docking-layout change now — after NewFrame but before windows are submitted
    // in onImGuiRender(), so it doesn't fight the DockSpaceOverViewport already submitted this frame.
    if (pendingDefaultReset_) {
        ofs::ui::applyDefaultLayout();
        pendingDefaultReset_ = false;
    } else if (pendingLayoutApply_) {
        ImGui::LoadIniSettingsFromMemory(pendingLayoutIni_.data(), pendingLayoutIni_.size());
        pendingLayoutApply_ = false;
        pendingLayoutIni_.clear();
    }

    if (player) {
        player->update(dt);
        scriptProject.playback.cursorPos = player->getLogicalPosition();
    }
    if (videoPreview)
        videoPreview->update(dt);
    sessionTime += dt;
    if (!scriptProject.state.filePath.empty())
        scriptProject.state.totalEditingSeconds += dt;
    if (projectManager)
        projectManager->update(dt);
    if (bindingSystem) {
        // Clear *keyboard* holds when ImGui keyboard capture rises (a text field focuses): that context
        // won't deliver the held key's key-up, so the hold would otherwise stick. Gamepad/analog
        // holds are left alone — their release is always delivered, and gamepad nav raises this flag on
        // L1/R1, which must not kill a held frame-step. tickHolds then advances the survivors.
        const bool wantCapture = ImGui::GetIO().WantCaptureKeyboard;
        if (wantCapture && !prevWantCaptureKeyboard_)
            bindingSystem->clearKeyHolds();
        prevWantCaptureKeyboard_ = wantCapture;

        // Poll analog gamepad axes for PadAxis hold sources (sticks/triggers). Single active pad — the
        // last opened wins. SDL axes are ±32767; normalize sticks to ±1 and triggers to 0..1.
        std::array<float, SDL_GAMEPAD_AXIS_COUNT> axisRaw{};
        for (auto &[id, pad] : gamepads_)
            for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a)
                axisRaw[a] =
                    std::clamp(static_cast<float>(SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(a))) / 32767.0f,
                               -1.0f, 1.0f);
        const bool padCaptured =
            (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) != 0 && ImGui::GetIO().NavActive;

        // Rebind capture from a stick/trigger push: a PadAxis can't arrive as a button event, so the
        // first axis past a generous threshold becomes the captured trigger (mirrors the key/button
        // capture paths in BindingSystem). Its sign selects the bound half.
        ofs::RebindState &rs = bindingSystem->rebindState();
        if (rs.capturing) {
            constexpr float kAxisCaptureThreshold = 0.6f;
            for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
                if (std::abs(axisRaw[a]) >= kAxisCaptureThreshold) {
                    rs.captured = ofs::PadAxis{.axis = static_cast<SDL_GamepadAxis>(a), .positive = axisRaw[a] > 0.f};
                    rs.hasResult = true;
                    rs.capturing = false;
                    break;
                }
            }
        }

        bindingSystem->tickAnalog(axisRaw, padCaptured, dt);
        bindingSystem->tickHolds(dt);
    }
    if (pluginManager)
        pluginManager->update(dt);
    if (scriptSystem)
        scriptSystem->update(dt);

    // Opt-in translator aid: re-load the active language file when it changes on disk. Off the
    // normal path — only runs when the setting is enabled.
    if (appSettings.liveReloadTranslations)
        ofs::loc::Translator::instance().pollReload();

    // Deferred app-settings persistence (distinct from project save): at most one appSettings.save()
    // per frame, regardless of how many ModifyEvent<AppSettings> were drained (e.g. a slider dragged
    // across many frames).
    if (appSettingsDirty_) {
        appSettings.save();
        appSettingsDirty_ = false;
    }
}

void OfsApp::onSeekEvent(const ofs::SeekEvent &event) {
    const double duration = player ? player->getDuration() : 0.0;
    scriptProject.playback.cursorPos = std::clamp(event.time, 0.0, duration);
}

void OfsApp::refreshProviderCommands() {
    const uint64_t sig =
        ofs::registryProviderSignature(scriptProject, editModeRegistry, navigatorRegistry, selectionModeRegistry);
    if (sig == providerCommandSignature_)
        return;
    providerCommandSignature_ = sig;
    // Wholesale refresh: drop the previous Dynamic set and rebuild (mode switches + axis select/toggle +
    // tool options). The set is small and this runs only on a mode-set / scratch-axis-existence / language
    // change, never per frame. Custom (source=Custom) and native commands are untouched. Pointers into the
    // registry are not held across this — it runs in onUpdate, before the frame's palette is built.
    commandRegistry.removeBySource(ofs::CommandSource::Dynamic);
    std::vector<ofs::Command> cmds;
    ofs::buildRegistryProviderCommands(scriptProject, editModeRegistry, navigatorRegistry, selectionModeRegistry, cmds);
    for (auto &c : cmds)
        commandRegistry.add(std::move(c));
}

void OfsApp::renderTitleBar() {
    // Rebuild the dynamic navigation commands only when the project's nav state changes, so the
    // per-frame palette build below never constructs strings/vectors (hot-path rule). The
    // cached list is stable for the rest of the frame, so pointers into it survive until dispatch.
    const uint64_t sig =
        ofs::dynamicCommandsSignature(scriptProject, editModeRegistry, navigatorRegistry, selectionModeRegistry);
    if (sig != navSignature) {
        navSignature = sig;
        dynamicPaletteCommands.clear();
        ofs::buildDynamicCommands(scriptProject, editModeRegistry, navigatorRegistry, selectionModeRegistry,
                                  dynamicPaletteCommands);
    }

    // Build the palette view into frame-scratch from two stable sources: the static registry and the
    // cached dynamic commands. Only inPalette + enabled() commands are shown; per-command key hints
    // come from BindingSystem. Parallel ptrs[] holds each shown command so invokedCommand (an index
    // into the filtered array) maps straight back to its run(). No per-frame heap.
    const auto &staticCmds = commandRegistry.all();
    const int total = static_cast<int>(staticCmds.size() + dynamicPaletteCommands.size());
    auto *cmds = ofs::FrameAllocator::instance().allocArray<ofs::ui::TitleBarCommand>(total > 0 ? total : 1);
    auto *ptrs = ofs::FrameAllocator::instance().allocArray<const ofs::Command *>(total > 0 ? total : 1);
    int count = 0;
    const bool hasProject = projectManager && projectManager->hasActiveProject();
    auto appendCommand = [&](const ofs::Command &c) {
        if (!c.inPalette || !c.enabled())
            return;
        // On the welcome screen only global commands are offered — every project/plugin/dynamic command
        // is hidden (the one filter that covers all three; requiresProject defaults true).
        if (!hasProject && c.requiresProject)
            return;
        ptrs[count] = &c;
        cmds[count].title = c.title.c_str();
        // Localized group text — drives both the palette's "Group: Title" label and its fuzzy-match
        // haystack, so matching is against the active language with no English fallback (mirrors the
        // Shortcut window). c.group stays the canonical English key used for icon/displayName lookup.
        cmds[count].group = commandRegistry.groupDisplayName(c.group);
        cmds[count].icon = commandRegistry.groupIcon(c.group);
        // Plugin commands live in their own (unregistered) groups, so they have no group icon.
        // Give them all a shared plugin glyph so they read as plugin-provided in the palette.
        if (cmds[count].icon[0] == '\0' && c.source == ofs::CommandSource::Plugin)
            cmds[count].icon = ICON_PLUGIN;
        cmds[count].shortcut = "";
        if (bindingSystem) {
            if (const auto *hint = bindingSystem->findHint(c.id))
                cmds[count].shortcut = formatTriggerHint(*hint);
        }
        cmds[count].keywords = c.keywords.c_str();             // search-only aliases; folded into the fuzzy haystack
        cmds[count].frecency = commandRegistry.frecency(c.id); // O(1) hash lookup; orders the palette
        ++count;
    };
    for (const auto &c : staticCmds)
        appendCommand(c);
    for (const auto &c : dynamicPaletteCommands)
        appendCommand(c);

    const bool requestOpen = commandPaletteOpenRequested;
    commandPaletteOpenRequested = false;

    const auto result = ofs::ui::renderTitleBar(
        window->getTitle().c_str(), projectManager ? projectManager->getProjectTitle() : Str::AppNoProject.c_str(),
        window->isMaximized(), std::span<const ofs::ui::TitleBarCommand>(cmds, static_cast<size_t>(count)), requestOpen,
        paletteState);

    // Hand the draggable geometry to the window's hit test; it owns all window mechanics. The search
    // box range is carved back out so its clicks aren't swallowed as a window drag.
    window->setTitleBarLayout(result.height, result.buttonsLeftX, result.searchLeftX, result.searchRightX);

    // Dispatch straight through the chosen command's run (the single execution path for both static
    // and dynamic commands; equivalent to CommandRegistry::run for registry entries). Dynamic nav
    // commands aren't in the registry, so run(id) couldn't reach them.
    if (result.invokedCommand >= 0 && result.invokedCommand < count) {
        const ofs::Command *c = ptrs[result.invokedCommand];
        if (c && c->run) {
            // Count the use for frecency. This path calls c->run directly (dynamic nav commands aren't
            // in the registry, so run(id) couldn't reach them), so record explicitly here.
            commandRegistry.recordUse(c->id);
            c->run(eventQueue);
        }
    }

    switch (result.action) {
    case ofs::ui::TitleBarAction::Minimize:
        window->minimize();
        break;
    case ofs::ui::TitleBarAction::ToggleMaximize:
        window->toggleMaximize();
        break;
    case ofs::ui::TitleBarAction::Close:
        eventQueue.push(ofs::RequestExitEvent{});
        break;
    case ofs::ui::TitleBarAction::None:
        break;
    }
}

bool OfsApp::canOptimizeIntra() const {
    // hasMedia() (not isVideoLoaded(), which is true for the media-less dummy timeline) gates on a real
    // decodable file loaded as the original source.
    return player && player->hasMedia() && scriptProject.activeSource == ofs::MediaSource::Original &&
           ofs::util::toolAvailable("ffmpeg") && ofs::util::toolAvailable("ffprobe") && !scriptProject.transcode.active;
}

void OfsApp::renderFooterBar() {
    ofs::ui::FooterBarInfo info;

    // App render-loop status — always shown, including over the welcome screen.
    info.appFps = ImGui::GetIO().Framerate;
    info.idle = fpsIdling.isIdling;

    // No project: the footer drops all axis/transport telemetry for a single welcome status line (the
    // bell still renders inside ui::renderFooterBar).
    if (!projectManager || !projectManager->hasActiveProject()) {
        info.idleMessage = ofs::generated::kGitTag.empty()
                               ? Str::AppNoProjectOpen.c_str()
                               : fmtScratch("ofs-ng {} · {}", ofs::generated::kGitTag, Str::AppNoProjectOpen.c_str());
        ofs::ui::renderFooterBar(info, notifications, eventQueue);
        return;
    }

    // Transport stats off the player (fps/speed only — the cursor/duration readout was dropped).
    if (player) {
        info.fps = player->getFps();
        info.playbackSpeed = player->getPlaybackSpeed();
        // Source badge: only meaningful with a real file loaded (the dummy timeline has no source).
        if (player->hasMedia())
            info.mediaSource = scriptProject.activeSource == ofs::MediaSource::Intra ? 1 : 0;
    }

    // Whether the original-source badge offers a click-to-optimize affordance. Mirrors the optimize
    // command's gate minus the output dir (now auto-picked on demand): a real original is loaded, the
    // tools resolve, and nothing is already transcoding.
    info.canOptimize = canOptimizeIntra();

    // Active axis + selection.
    const ofs::StandardAxis active = scriptProject.state.activeAxis;
    const ofs::AxisState &axis = scriptProject.axes[static_cast<size_t>(active)];
    info.activeAxis = active;
    info.activeActionCount = static_cast<int>(axis.actions.size());
    info.selectionCount = static_cast<int>(axis.selection.size());
    if (axis.selection.size() >= 2)
        info.selectionSpan = axis.selection.back().at - axis.selection.front().at;

    info.visibleTime = scriptProject.timelineView.visibleTime;

    // Undo history: stack depth (steps) plus compressed bytes held vs. the configured budget.
    if (undoSystem) {
        info.undoSteps = undoSystem->undoStepCount();
        info.redoSteps = undoSystem->redoStepCount();
        info.undoUsedBytes = undoSystem->memoryUsedBytes();
        info.undoMaxBytes = undoSystem->memoryMaxBytes();
    }

    // Managed (.NET) heap — 0 when no CLR is loaded, which hides the zone.
    if (pluginManager)
        info.managedHeapBytes = pluginManager->managedHeapBytes();

    // Background eval workers — count + age of the longest-running one (drives the stuck-thread warning).
    const auto workers = jobSystem.evalWorkerStats();
    info.runningWorkers = static_cast<int>(workers.running);
    info.oldestWorkerSeconds = workers.oldestAgeSeconds;

    // Background evaluation — surface the async-job contract once for the whole project.
    for (const auto &a : scriptProject.axes) {
        if (a.pendingEval) {
            if (info.evaluatingCount == 0)
                info.evaluatingAxisName = ofs::standardAxisShortName(a.role).data();
            ++info.evaluatingCount;
        }
    }

    // Interaction extension-point selectors — frame-scratch option lists from the registries. The native
    // ids get a localized label; a plugin entry shows its own (verbatim, plugin-localized) display name,
    // falling back to its id if it supplied none. (A mode's per-mode options UI lives in the docked Tool
    // Options panel — renderToolOptions — not in the footer.)
    info.editModes =
        buildFooterOptions(editModeRegistry, ofs::kNativeEditModeId, Str::FtEditModeNative.c_str(), info.editModeCount);
    info.activeEditModeId = scriptProject.activeEditMode.c_str();
    info.navigators = buildFooterOptions(navigatorRegistry, ofs::kFollowOverlayNavigatorId, Str::FtStepNative.c_str(),
                                         info.navigatorCount);
    info.activeNavigatorId = scriptProject.activeNavigator.c_str();
    info.selectionModes = buildFooterOptions(selectionModeRegistry, ofs::kNativeSelectionModeId,
                                             Str::FtSelectNative.c_str(), info.selectionModeCount);
    info.activeSelectionModeId = scriptProject.activeSelectionMode.c_str();

    // A click on a selector's tool-options affordance pushes OpenToolOptionsEvent; the handler latches the
    // target and onImGuiRender raises the modal (inside a live frame). The docked panel (View menu) stays
    // for users who want the options visible while editing.
    ofs::ui::renderFooterBar(info, notifications, eventQueue);
}

bool OfsApp::renderActiveToolSections() {
    bool any = false;
    // `seed` namespaces each section's widget ids: PluginManager::renderIntentUi resets its per-call
    // id counter to 0, so two sections in the same window (e.g. the peaks navigator and peaks selection
    // mode, which draw the same widgets) would otherwise collide. Each mode is a collapsing header,
    // expanded by default; collapsing it skips the onUi call so the options actually hide.
    auto section = [&](const char *seed, const std::string &activeId, const auto &registry) {
        const auto *e = registry.find(activeId);
        if (!e || !e->onUi || !pluginManager)
            return;
        ImGui::PushID(seed);
        // displayName is plugin-provided (rendered verbatim); empty falls back to the namespaced id.
        const char *hdr = fmtScratch("{}###hdr", e->displayName.empty() ? e->id.c_str() : e->displayName.c_str());
        if (ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen))
            pluginManager->renderIntentUi(e->owningPlugin, e->onUi, e->userData);
        ImGui::PopID();
        any = true;
    };
    section("toolopt_edit", scriptProject.activeEditMode, editModeRegistry);
    section("toolopt_nav", scriptProject.activeNavigator, navigatorRegistry);
    section("toolopt_sel", scriptProject.activeSelectionMode, selectionModeRegistry);
    return any;
}

bool OfsApp::renderToolOptionForTarget(ofs::ToolOptionTarget target) {
    if (!pluginManager)
        return false;
    // Draw exactly one extension point's active-mode onUi, with no header — the modal is per-mode (the
    // collapsing header is the docked panel's affair). Returns false when the active mode has no options.
    return withToolTarget(target, [this](const auto &registry, const std::string &activeId) {
        const auto *e = registry.find(activeId);
        if (!e || !e->onUi)
            return false;
        pluginManager->renderIntentUi(e->owningPlugin, e->onUi, e->userData);
        return true;
    });
}

std::string OfsApp::toolOptionTitle(ofs::ToolOptionTarget target) const {
    // The modal title is the active mode's display name (it stands in for the panel's section header);
    // a plugin mode always carries one, so the Str fallback only covers an unexpectedly-empty entry.
    return withToolTarget(target, [](const auto &registry, const std::string &activeId) -> std::string {
        const auto *e = registry.find(activeId);
        if (e && !e->displayName.empty())
            return e->displayName;
        if (e)
            return e->id;
        return {Str::ToolOptionsTitle.c_str()};
    });
}

void OfsApp::renderToolOptions() {
    if (!appSettings.showToolOptions)
        return;

    // A docked panel that renders the active edit-mode / navigator / selection-mode options (each mode's
    // onUi), one section per active mode that supplied one. A docked window resizes and scrolls naturally
    // and stays open while editing — the always-visible counterpart to the footer's click-away tool-options
    // modal, which shares the same section renderer. NoNavInputs: plugin widgets
    // want raw arrow/Enter.
    bool open = appSettings.showToolOptions;
    if (ImGui::Begin(Str::ToolOptionsTitle.id("tool_options"), &open, ImGuiWindowFlags_NoNavInputs)) {
        if (!renderActiveToolSections())
            ImGui::TextDisabled("%s", Str::ToolOptionsEmpty.c_str());
    }
    ImGui::End();
    if (appSettings.showToolOptions != open) { // closed via the window's [x] — persist like the menu toggle
        appSettings.showToolOptions = open;
        appSettingsDirty_ = true;
    }
}

void OfsApp::onImGuiRender() {
    // Footer + menu are always-on chrome. The footer reserves its work area before any body claims the
    // viewport, mirroring how the title bar (rendered by Application's renderTitleBar hook) is submitted
    // ahead of onImGuiRender.
    renderFooterBar();
    renderMainMenuBar();

    // Consume the optimize-dialog latch here (not in the event handler) so the modal is built inside a
    // live ImGui frame — its width is font-relative like every other showCustomModal call.
    if (transcodeDialogOpenRequested) {
        transcodeDialogOpenRequested = false;
        openTranscodeOptionsModal();
    }
    // Tool-options affordance / palette command: raise one mode's options as a click-away-dismiss modal.
    // Built here (a live frame) so its width is font-relative like every other showCustomModal call; the
    // body re-resolves the target's active mode each frame, so switching that mode while it is open swaps
    // its content. Per-mode, so the modal needs no section header — the mode name is its title.
    if (openToolOptionsModalTarget_) {
        const ofs::ToolOptionTarget target = *openToolOptionsModalTarget_;
        openToolOptionsModalTarget_.reset();
        ofs::showCustomModal(eventQueue, {.title = toolOptionTitle(target),
                                          .width = ImGui::GetFontSize() * 20.0f,
                                          .body =
                                              [this, target]() {
                                                  if (!renderToolOptionForTarget(target))
                                                      ImGui::TextDisabled("%s", Str::ToolOptionsEmpty.c_str());
                                                  return false; // closed by click-away / Escape, never self-closes
                                              },
                                          .dismissOnClickAway = true});
    }
    maybeOfferOptimize();

    // Global app windows render on both screens — Preferences and Keyboard Shortcuts are app/global
    // state (not project state), and their menu items / palette commands are available with no project
    // open. Each early-returns when its visibility flag is off, so they cost nothing until opened.
    if (shortcutWindow)
        shortcutWindow->render(appState.showShortcutWindow, eventQueue, appSettings);
    if (configWindow)
        configWindow->render(scriptProject, eventQueue, appState.showConfigWindow);
    if (aboutWindow)
        aboutWindow->render(appState.showAboutWindow, updateChecker->status(), eventQueue);
    if (backupRestoreWindow)
        backupRestoreWindow->render(appState.showBackupRestoreWindow, scriptProject, eventQueue);

    // One top-level branch: the editor (dockspace + windows) only renders with an active project;
    // otherwise the welcome screen takes the body. Every editor window may therefore assume a project
    // exists instead of defending against absent state. hasActiveProject() is the single source of
    // truth (no stored flag), so the screen is derived fresh each frame.
    if (projectManager && projectManager->hasActiveProject())
        renderEditor();
    else
        welcomeScreen->render(eventQueue, appSettings);

    // Cross-cutting overlays render after the body on both screens: the background-task stack and toasts
    // above all docked windows (tasks first so renderToasts can float above them), then modals last so a
    // blocking dialog (e.g. the New/Open picker reached from the welcome screen) stacks above everything.
    ofs::ui::renderTasks(notifications, eventQueue);
    ofs::ui::renderToasts(notifications);
    if (modalManager)
        modalManager->render();
}

void OfsApp::renderEditor() {
    ofs::ui::beginDockspace(layoutStore.locked);

// Suppressed under OFS_TEST_PREF_SUBDIR (the test-build sentinel): the metrics window floats over the
// central node and would intercept clicks the UI tests aim at docked windows.
#if !defined(NDEBUG) && !defined(OFS_TEST_PREF_SUBDIR)
    if (showImGuiMetrics_)
        ImGui::ShowMetricsWindow(&showImGuiMetrics_);
#endif

    if (scriptSimulator) {
        const bool vpHovered = videoPlayerWindow ? videoPlayerWindow->isWindowHovered() : false;
        scriptSimulator->render(scriptProject, eventQueue, appSettings.showSimulator, vpHovered);
    }

    // Exactly one of these renders per frame, and both Begin the same dock window (the Processing
    // panel via the "###video_player" shared id slug). Keeping the center node to a single window is what
    // preserves a user-hidden tab bar — ImGui force-clears HiddenTabBar on any node holding >1 window.
    if (processingPanel && scriptProject.procSelRegionId != -1) {
        processingPanel->render(scriptProject, eventQueue, effectRegistry, scriptRegistry);
    } else if (videoPlayerWindow) {
        videoPlayerWindow->onImGuiRender(scriptProject, eventQueue, *player);
    }

    if (scriptTimelineWindow)
        scriptTimelineWindow->render(scriptProject, eventQueue, *player, *waveformRenderer);
    if (videoControlsWindow)
        videoControlsWindow->render(scriptProject, eventQueue, *player, *videoPreview, *previewPopup);
    if (scriptStatisticsWindow)
        scriptStatisticsWindow->render(scriptProject, appSettings.showStatistics);
    if (logWindow)
        logWindow->render(appState.showLogWindow);
    if (projectConfigWindow)
        projectConfigWindow->render(scriptProject, eventQueue, appState.showProjectConfigWindow, sessionTime,
                                    player ? player->getDuration() : 0.0, dummyPlayer && dummyPlayer->isVideoLoaded());

    if (pluginManager)
        pluginManager->renderUI();

    renderToolOptions();

    if (processingPanel && scriptTimelineWindow && !scriptProject.procPanelLocked &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const bool anyPopupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        if (scriptProject.procSelRegionId != -1 && !processingPanel->cursorInsideThisFrame() &&
            !scriptTimelineWindow->wasRegionClickedThisFrame() && !anyPopupOpen) {
            eventQueue.push(ofs::ClearRegionSelectionEvent{});
        }
    }
}

void OfsApp::onPostRender() {
    if (player)
        player->notifySwap();
    if (videoPreview)
        videoPreview->reportSwap();
}

float OfsApp::fontSizeBase() const {
    return appSettings.fontSizeBase;
}

int OfsApp::frameCapFps() const {
    return appSettings.maxFps;
}

bool OfsApp::canAppIdle() const {
#ifdef OFS_TEST_ENGINE
    // Under the imgui_test_engine harness (ui-tests) frames are driven programmatically with
    // no real input, so the idle path would throttle the loop to fpsIdle (~9 FPS) and crawl. Never idle.
    return false;
#else
    // A queued/answered modal must keep the loop awake, or the click->resume frame stalls until
    // the next input event arrives. An active Hold (held key/button/stick) likewise keeps it awake:
    // a held input emits no further SDL events, so idling would block in waitEvents and freeze the
    // per-frame tick that drives the hold (frame-step repeat, simulator nudging, etc.).
    return (!player || player->isPaused()) && !(modalManager && modalManager->busy()) &&
           !(bindingSystem && bindingSystem->hasActiveHolds());
#endif
}

bool OfsApp::shouldDropAmbientEvent(const SDL_Event &event) const {
    // A connected controller emits a steady stream of axis-motion events from stick/trigger jitter.
    // Motion within the deadzone is "at rest": drop it so it never wakes the idle wait. A real push past
    // the deadzone is kept — it wakes the loop, which then reads the axis by polling (onUpdate). The
    // floor keeps a deadzone of 0 from letting raw jitter peg the loop.
    if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION || event.type == SDL_EVENT_JOYSTICK_AXIS_MOTION) {
        const auto raw =
            static_cast<float>(event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION ? event.gaxis.value : event.jaxis.value);
        const float dz = std::max(0.05f, appSettings.input.deadzone);
        return std::abs(raw / 32767.0f) <= dz;
    }
    // The gamepad touchpad isn't bound to anything; a resting/grazing finger floods motion (and the
    // down/up around it). Drop it like the stick jitter above.
    if (event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN || event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION ||
        event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_UP)
        return true;
    // SDL emits an UPDATE_COMPLETE marker at the end of every poll cycle in which a pad's state changed,
    // and a SENSOR_UPDATE stream for pads with gyro/accelerometer. Neither is deliberate input: the
    // stick jitter behind UPDATE_COMPLETE is already dropped above, and real input still arrives as its
    // own button/axis event. Left in the queue these markers wake the wait every cycle and peg the loop
    // at full FPS whenever a controller is plugged in. Drop them.
    if (event.type == SDL_EVENT_GAMEPAD_UPDATE_COMPLETE || event.type == SDL_EVENT_JOYSTICK_UPDATE_COMPLETE ||
        event.type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE)
        return true;
    return false;
}

void OfsApp::renderMainMenuBar() {
    const bool hasProject = projectManager && projectManager->hasActiveProject();
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(Str::AppMenuFile.id("menu_file"))) {
            if (ImGui::MenuItem(Str::AppMenuOpenNew.iconId(ICON_FOLDER_OPEN, "menu_open_new")))
                eventQueue.push(ofs::OpenOrNewProjectRequestEvent{});
            ImGui::BeginDisabled(!hasProject);
            if (ImGui::MenuItem(Str::AppMenuRestoreBackup.iconId(ICON_ARCHIVE_RESTORE, "menu_restore_backup")))
                appState.showBackupRestoreWindow = true;
            if (ImGui::MenuItem(Str::AppMenuCloseProject.iconId(ICON_X, "menu_close_project")))
                eventQueue.push(ofs::CloseProjectRequestEvent{});
            ImGui::EndDisabled();
            // The recent-projects list lives on the welcome screen (shown whenever no project is open).
            ImGui::Separator();
            ImGui::BeginDisabled(!hasProject);
            if (ImGui::MenuItem(Str::AppMenuSaveProject.iconId(ICON_SAVE, "menu_save_project")))
                eventQueue.push(ofs::SaveProjectEvent{false});
            if (ImGui::MenuItem(Str::AppMenuSaveProjectAs.iconId(ICON_SAVE_ALL, "menu_save_project_as")))
                eventQueue.push(ofs::SaveProjectEvent{true});
            ImGui::EndDisabled();
            ImGui::Separator();
            if (ImGui::BeginMenu(Str::AppMenuImport.iconId(ICON_IMPORT, "menu_import"), hasProject)) {
                if (ImGui::MenuItem(Str::AppMenuImportFunscript.iconId(ICON_FILE, "menu_import_funscript")))
                    eventQueue.push(ofs::ImportFunscriptRequestEvent{});
                ImGui::EndMenu();
            }
            ImGui::BeginDisabled(!hasProject);
            if (ImGui::MenuItem(Str::AppMenuExportFunscript.iconId(ICON_FILE_OUTPUT, "menu_export")))
                openExportFunscriptModal();
            ImGui::EndDisabled();
            ImGui::Separator();
            if (ImGui::MenuItem(Str::AppMenuExit.iconId(ICON_LOG_OUT, "menu_exit")))
                eventQueue.push(ofs::RequestExitEvent{});
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(Str::AppMenuEdit.id("menu_edit"))) {
            if (ImGui::MenuItem(Str::AppMenuPreferences.iconId(ICON_SETTINGS, "menu_preferences")))
                appState.showConfigWindow = true;
            // Gate on hasProject, not filePath: a fresh untitled project (no file yet) still needs
            // Project Configuration to load its video — the New Project dialog points users here.
            ImGui::BeginDisabled(!hasProject);
            if (ImGui::MenuItem(Str::AppMenuProject.iconId(ICON_SLIDERS_HORIZONTAL, "menu_project_config")))
                appState.showProjectConfigWindow = true;
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(Str::AppMenuView.id("menu_view"))) {
            ImGui::MenuItem(Str::AppMenuShortcuts.iconId(ICON_KEYBOARD, "menu_shortcuts"), nullptr,
                            &appState.showShortcutWindow);
            if (ImGui::BeginMenu(Str::AppMenuSimulator.iconId(ICON_AXIS_3D, "menu_view_simulator"))) {
                if (ImGui::MenuItem(Str::AppMenuSimulatorShow.id("menu_view_simulator_show"), nullptr,
                                    &appSettings.showSimulator))
                    appSettingsDirty_ = true;
                // Lock lives on the project's SimulatorState (mutated via event, like the overlay's
                // own right-click menu), so this is just another entry point to the same toggle.
                const bool simLocked = scriptProject.simulator.lockedPosition;
                if (ImGui::MenuItem(
                        Str::SimLocked.iconId(simLocked ? ICON_LOCK : ICON_LOCK_OPEN, "menu_view_simulator_lock"),
                        nullptr, simLocked))
                    eventQueue.push(ofs::ModifyEvent<ofs::SimulatorState>{
                        [](ofs::SimulatorState &s) { s.lockedPosition = !s.lockedPosition; }});
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(Str::AppMenuStatistics.iconId(ICON_CHART_LINE, "menu_view_statistics"), nullptr,
                                &appSettings.showStatistics))
                appSettingsDirty_ = true;
            if (ImGui::MenuItem(Str::AppMenuToolOptions.iconId(ICON_SETTINGS_2, "menu_view_tooloptions"), nullptr,
                                &appSettings.showToolOptions))
                appSettingsDirty_ = true;
            ImGui::MenuItem(Str::AppMenuLog.iconId(ICON_SCROLL_TEXT, "menu_view_log"), nullptr,
                            &appState.showLogWindow);
            ImGui::Separator();
            // Live checkmark reflects the actual window flag; dispatches directly (main-thread UI),
            // exactly as the title-bar minimize/maximize do. F11 hint is omitted because the binding
            // is user-rebindable — the real shortcut is shown in the command palette and bindings UI.
            if (ImGui::MenuItem(Str::AppMenuFullscreen.iconId(ICON_MAXIMIZE, "menu_view_fullscreen"), nullptr,
                                window->isFullscreen()))
                window->toggleFullscreen();
#ifndef NDEBUG
            // Developer diagnostic — debug builds only, so it stays an English literal like the ImGui
            // metrics window it toggles (not routed through the localization catalog).
            ImGui::Separator();
            ImGui::MenuItem("Dear ImGui Metrics###imgui_metrics", nullptr, &showImGuiMetrics_);
#endif
            ImGui::EndMenu();
        }

        renderLayoutMenu();
        renderAxesMenu(hasProject);
        renderPluginsMenu(hasProject);

        if (ImGui::BeginMenu(Str::AppMenuHelp.id("menu_help"))) {
            if (ImGui::MenuItem(Str::AppMenuAbout.iconId(ICON_INFO, "menu_help_about")))
                appState.showAboutWindow = true;
            ImGui::EndMenu();
        }

        renderMenuBarTitle();

        ImGui::EndMainMenuBar();
    }
}

void OfsApp::renderLayoutMenu() {
    if (!ImGui::BeginMenu(Str::AppMenuLayout.id("menu_layout")))
        return;
    const bool isDefault = (layoutStore.activeLayoutName == "Default");
    ImGui::BeginDisabled(isDefault); // "Default" is read-only; only user layouts are saveable
    // ###-suffixed stable IDs so the leading icon glyph (and, for Lock, its toggle between
    // two icons) can't change the item's ImGui ID — keeps state stable and tests icon-agnostic.
    if (ImGui::MenuItem(Str::AppLayoutSave.iconId(ICON_SAVE, "SaveLayout")))
        saveActiveLayout();
    ImGui::EndDisabled();
    if (isDefault && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", Str::AppLayoutSaveTip.c_str());
    if (!layoutStore.layouts.empty()) {
        std::string deleteRequest;
        if (ImGui::BeginMenu(Str::AppLayoutDelete.iconId(ICON_TRASH, "layout_delete"))) {
            for (const auto &preset : layoutStore.layouts)
                if (ImGui::MenuItem(preset.name.c_str()))
                    deleteRequest = preset.name;
            ImGui::EndMenu();
        }
        if (!deleteRequest.empty()) {
            std::erase_if(layoutStore.layouts, [&](const auto &p) { return p.name == deleteRequest; });
            if (layoutStore.activeLayoutName == deleteRequest) {
                layoutStore.activeLayoutName = "Default";
                pendingDefaultReset_ = true;
            }
            layoutStore.save();
            eventQueue.push(ofs::NotifyEvent{.level = ofs::NotifyLevel::Success,
                                             .message = Str::AppLayoutDeleted.fmt(deleteRequest)});
        }
    }
    if (ImGui::MenuItem(Str::AppLayoutReset.iconId(ICON_REFRESH, "ResetLayout")))
        revertToActiveLayout();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", isDefault ? Str::AppLayoutResetTipDefault.c_str() : Str::AppLayoutResetTipUser.c_str());
    if (ImGui::MenuItem(Str::AppLayoutLock.iconId(layoutStore.locked ? ICON_LOCK : ICON_LOCK_OPEN, "LockLayout"),
                        nullptr, layoutStore.locked)) {
        layoutStore.locked = !layoutStore.locked;
        layoutStore.save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", Str::AppLayoutLockTip.c_str());
    ImGui::Separator();
    if (ImGui::MenuItem(Str::AppLayoutNew.iconId(ICON_PLUS, "NewLayout"))) {
        newLayoutName_.clear();
        focusNewLayoutName_ = true;
        ofs::showCustomModal(eventQueue, {.title = Str::AppNewLayoutTitle.c_str(),
                                          .width = ImGui::GetFontSize() * 22.5f,
                                          .body = [this]() -> bool { return renderNewLayoutBody(); }});
    }
    ImGui::Separator();
    if (ImGui::MenuItem(Str::AppLayoutDefault.id("layout_default"), nullptr, isDefault) && !isDefault) {
        layoutStore.activeLayoutName = "Default";
        pendingDefaultReset_ = true;
        layoutStore.save();
    }
    // Switching to a user layout applies its saved arrangement on the next frame.
    for (const auto &preset : layoutStore.layouts) {
        const bool active = (preset.name == layoutStore.activeLayoutName);
        if (ImGui::MenuItem(preset.name.c_str(), nullptr, active) && !active) {
            layoutStore.activeLayoutName = preset.name;
            pendingLayoutIni_ = preset.ini;
            pendingLayoutApply_ = true;
            layoutStore.save();
        }
    }
    ImGui::EndMenu();
}

void OfsApp::renderAxesMenu(bool hasProject) {
    if (!ImGui::BeginMenu(Str::AppMenuAxes.id("menu_axes"), hasProject))
        return;
    if (ImGui::MenuItem(Str::AppAxesMultiAxis.iconId(ICON_EYE, "menu_multi_axis")) && projectManager)
        eventQueue.push(ofs::ShowMultiAxisEvent{});
    ImGui::SetItemTooltip("%s", Str::AppAxesMultiAxisTip.c_str());
    if (ImGui::MenuItem(Str::AppAxesL0Only.iconId(ICON_EYE_OFF, "menu_l0_only")) && projectManager)
        eventQueue.push(ofs::ShowL0OnlyEvent{});
    ImGui::SetItemTooltip("%s", Str::AppAxesL0OnlyTip.c_str());
    ImGui::Separator();
    bool seenScratch = false;
    for (size_t i = 0; i < ofs::kStandardAxisCount; ++i) {
        const auto role = static_cast<ofs::StandardAxis>(i);
        const auto &axis = scriptProject.axes[i];
        const bool isScratch = ofs::isScratchAxis(role);
        // Standard axes are always listed; a scratch axis is listed only once it exists (in the
        // strip or holding data) so the menu isn't flooded with ten empty S-slots.
        if (isScratch && !axis.exists())
            continue;
        if (isScratch && !seenScratch) {
            ImGui::Separator();
            seenScratch = true;
        }
        // Stable per-role ###id (display shows the localized axis name) so tests target the
        // submenu by role index, not the translated display label.
        if (ImGui::BeginMenu(fmtScratch("{}###menu_axis_{}", ofs::loc::localizedAxisName(role), i))) {
            bool vis = axis.isVisible;
            if (ImGui::MenuItem(Str::AppAxesShowTimeline.id("menu_axis_show_timeline"), nullptr, vis))
                eventQueue.push(ofs::ToggleAxisVisibilityEvent{.axisRole = role, .visible = !vis});

            bool inPanel = axis.showInStrip;
            bool isL0 = (role == ofs::StandardAxis::L0);
            ImGui::BeginDisabled(isL0);
            if (ImGui::MenuItem(Str::AppAxesShowPanel.id("menu_axis_show_panel"), nullptr, inPanel) && !isL0)
                eventQueue.push(ofs::ToggleAxisPanelVisibilityEvent{.axisRole = role, .inPanel = !inPanel});
            ImGui::EndDisabled();

            // Only an empty scratch axis can be removed; once it holds actions it behaves like
            // a standard axis and is hidden via "Show in Panel" instead.
            if (isScratch && axis.actions.empty()) {
                ImGui::Separator();
                if (ImGui::MenuItem(Str::AppAxesRemove.iconId(ICON_TRASH, "menu_axis_remove")))
                    eventQueue.push(ofs::RemoveAxisEvent{role});
            }
            ImGui::EndMenu();
        }
    }
    ImGui::Separator();
    int scratchCount = 0;
    for (auto i = static_cast<size_t>(ofs::StandardAxis::S0); i < ofs::kStandardAxisCount; ++i)
        if (scriptProject.axes[i].exists()) // a data-bearing scratch axis occupies a slot even if hidden
            ++scratchCount;
    ImGui::BeginDisabled(scratchCount >= ofs::kMaxScratchAxes);
    if (ImGui::MenuItem(Str::AppAxesAddScratch.iconId(ICON_PLUS, "menu_add_scratch_axis")) && projectManager)
        eventQueue.push(ofs::AddScratchAxisEvent{});
    ImGui::EndDisabled();
    if (scratchCount >= ofs::kMaxScratchAxes && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", Str::AppAxesMaxTip.c_str());
    ImGui::EndMenu();
}

void OfsApp::renderPluginsMenu(bool hasProject) {
    if (!ImGui::BeginMenu(Str::AppMenuPlugins.id("menu_plugins"), hasProject))
        return;
    if (ImGui::MenuItem(Str::AppPluginsInstall.iconId(ICON_IMPORT, "menu_install_plugin")))
        eventQueue.push(ofs::RequestInstallPluginEvent{});
    ImGui::Separator();
    if (!pluginManager || pluginManager->getPlugins().empty()) {
        ImGui::TextDisabled("%s", Str::AppPluginsNoneLoaded.c_str());
        ImGui::EndMenu();
        return;
    }
    for (auto &plugin : pluginManager->getPlugins()) {
        // A check in the top-level list marks an enabled plugin at a glance, without opening
        // the submenu; disabled plugins show no glyph.
        const char *check = plugin.enabled ? ICON_CHECK " " : "";
        if (ImGui::BeginMenu(fmtScratch("{}{}##{}", check, plugin.displayName, plugin.name))) {
            if (!plugin.version.empty())
                ImGui::SeparatorText(fmtScratch("v{}", plugin.version));
            bool enabled = plugin.enabled;
            if (ImGui::MenuItem(Str::AppPluginEnabled.id("plugin_enabled"), nullptr, &enabled))
                eventQueue.push(ofs::SetPluginEnabledEvent{.name = plugin.name, .enabled = enabled});
            // onBuildUI is non-null only when the plugin overrides OnRenderUi (see
            // PluginBridge.OverridesRenderUi); it is also zeroed when a plugin is disabled.
            // A plugin with no window has nothing to show, so the toggle is inert — render it
            // both unchecked and disabled (the value overload + manual flip), so it never
            // shows the contradictory "checked but greyed" state.
            const bool hasWindow = plugin.api.onBuildUI != nullptr;
            if (ImGui::MenuItem(Str::AppPluginShowWindow.id("plugin_show_window"), nullptr,
                                hasWindow && plugin.windowOpen, hasWindow)) {
                plugin.windowOpen = !plugin.windowOpen;
                eventQueue.push(ofs::SavePluginStatesEvent{});
            }
            // First-party plugins ship with the app and live in the read-only base root;
            // only user-installed plugins can be uninstalled or hot-reloaded.
            if (!plugin.firstParty) {
                ImGui::Separator();
                bool hotReload = plugin.hotReload;
                if (ImGui::MenuItem(Str::AppPluginHotReload.id("plugin_hot_reload"), nullptr, &hotReload,
                                    plugin.enabled))
                    eventQueue.push(ofs::SetPluginHotReloadEvent{.name = plugin.name, .enabled = hotReload});
                ImGui::SetItemTooltip("%s", Str::AppPluginHotReloadHint.c_str());
                if (ImGui::MenuItem(Str::AppPluginUninstall.iconId(ICON_TRASH, "plugin_uninstall")))
                    eventQueue.push(ofs::RequestUninstallPluginEvent{.name = plugin.name});
            }
            ImGui::EndMenu();
        }
    }
    ImGui::EndMenu();
}

void OfsApp::renderMenuBarTitle() {
    // The project name now lives in the title bar; the menu bar keeps only a compact saved/unsaved
    // indicator (nothing until a project is open). A dot marks unsaved edits, a check marks a clean
    // project; the relative save time stays in the hover tooltip rather than cluttering the bar.
    if (!projectManager || !projectManager->hasActiveProject())
        return;

    const bool dirty = projectManager->isDirty();
    const char *glyph = dirty ? ICON_CIRCLE_DOT : ICON_CHECK;
    const ImU32 color =
        dirty ? ofs::theme::GetColorU32(AppCol_UnsavedIndicator) : ofs::theme::GetColorU32(AppCol_Success);

    // Right-align the glyph; its width is measured and the trailing gutter is font-relative (DPI-safe).
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(glyph).x - ImGui::GetFontSize() * 1.25f);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(glyph);
    ImGui::PopStyleColor();

    if (!ImGui::IsItemHovered())
        return;
    if (!dirty) {
        ImGui::SetTooltip("%s", Str::AppSavedTip.c_str());
        return;
    }
    const char *ago = nullptr;
    if (auto t = projectManager->getLastSaveTime()) {
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - *t).count();
        // Named lvalues for the granularity values: fmt::make_format_args binds by reference.
        if (secs < 60) {
            ago = Str::AppSavedSecsAgo.fmt(secs);
        } else if (secs < 3600) {
            auto mins = secs / 60;
            ago = Str::AppSavedMinsAgo.fmt(mins);
        } else {
            auto hours = secs / 3600;
            ago = Str::AppSavedHoursAgo.fmt(hours);
        }
    }
    if (ago)
        ImGui::SetTooltip("%s\n%s", Str::AppUnsavedTip.c_str(), ago);
    else
        ImGui::SetTooltip("%s", Str::AppUnsavedTip.c_str());
}

void OfsApp::openExportFunscriptModal() {
    exportFunscriptDialog.isOpen = true;
    exportFunscriptDialog.options.clear();
    for (size_t i = 0; i < ofs::kStandardAxisCount; ++i) {
        const auto &axis = scriptProject.axes[i];
        if (ofs::isScratchAxis(static_cast<ofs::StandardAxis>(i)))
            continue;
        exportFunscriptDialog.options.push_back(
            {.role = static_cast<ofs::StandardAxis>(i), .selected = !axis.actions.empty()});
    }
    // Options are populated once here (the push-once point); the body (captures `this`) reads the
    // dialog state + axes and pushes the export request when confirmed.
    ofs::showCustomModal(eventQueue, {.title = Str::AppExportTitle.c_str(),
                                      .width = ImGui::GetFontSize() * 21.25f,
                                      .body = [this]() -> bool { return renderExportFunscriptBody(); }});
}

void OfsApp::onQuickExport() {
    if (!projectManager || !projectManager->hasActiveProject())
        return;
    // First export of a project goes through the full picker; every export records its config, so
    // afterwards Quick Export replays it (same format, axes, path) with no dialog.
    const auto &lastExport = scriptProject.state.lastExport;
    if (!lastExport) {
        openExportFunscriptModal();
        return;
    }
    eventQueue.push(ofs::ExportFunscriptRequestEvent{
        .axes = lastExport->axes, .format = lastExport->format, .targetPath = lastExport->outputPath});
}

bool OfsApp::renderExportFunscriptBody() {
    auto &dlg = exportFunscriptDialog;

    ImGui::TextUnformatted(Str::AppExportFormat);
    ImGui::RadioButton(Str::AppExportFormat10.id("export_fmt_10"), &dlg.format, 0);
    ImGui::RadioButton(Str::AppExportFormat11.id("export_fmt_11"), &dlg.format, 1);
    // .fmt() (no args) so the {{}} escape in the catalog renders the literal "channels{}"; the ###id
    // is wrapped on afterward (the formatted string is an argument, not a format spec, so its braces
    // are never re-interpreted).
    ImGui::RadioButton(fmtScratch("{}###export_fmt_20", Str::AppExportFormat20.fmt()), &dlg.format, 2);

    ImGui::Separator();
    ImGui::TextUnformatted(Str::AppExportAxesInclude);

    for (auto &opt : dlg.options) {
        const auto roleIdx = static_cast<size_t>(opt.role);
        const auto &axis = scriptProject.axes[roleIdx];
        ImGui::Checkbox(fmtScratch("{}###export_axis_{}", ofs::loc::localizedAxisName(opt.role), roleIdx),
                        &opt.selected);
        if (axis.actions.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", Str::AppExportEmpty.c_str());
        }
    }

    ImGui::Separator();
    if (ImGui::Button(Str::AppExportButton.id("export_confirm"))) {
        std::vector<ofs::StandardAxis> selected;
        for (const auto &opt : dlg.options)
            if (opt.selected)
                selected.push_back(opt.role);
        if (!selected.empty())
            eventQueue.push(ofs::ExportFunscriptRequestEvent{.axes = std::move(selected), .format = dlg.format});
        dlg.isOpen = false;
        return true;
    }
    ImGui::SameLine();
    if (ImGui::Button(Str::AppCancel.id("export_cancel"))) {
        dlg.isOpen = false;
        return true;
    }
    return false;
}

void OfsApp::openTranscodeOptionsModal() {
    // No output dir chosen yet: go straight to the folder picker (no scolding modal — there is nothing
    // wrong, the user simply hasn't picked where intra copies land). On a valid choice we persist it and
    // re-open this dialog so the flow continues.
    if (appSettings.intraOutputDir.empty()) {
        pickIntraOutputDir();
        return;
    }

    // A dir was chosen but the folder was deleted or unmounted since: ffmpeg would otherwise "succeed"
    // writing nothing into a missing directory. Alert the user and make them pick a new folder rather
    // than silently creating one — the choice of where large intra copies land is theirs.
    std::error_code dirEc;
    if (!std::filesystem::is_directory(ofs::util::fromUtf8(appSettings.intraOutputDir), dirEc)) {
        promptForMissingIntraDir();
        return;
    }

    // Seed the config once, here, from the live project/player. The gate guarantees we are on the
    // Original source, so originalMediaPath is the file to transcode (fall back to the resolved
    // mediaPath defensively). The body below only edits this shared config and pushes events.
    auto cfg = std::make_shared<ofs::TranscodeConfig>();
    cfg->sourcePath = !scriptProject.state.originalMediaPath.empty() ? scriptProject.state.originalMediaPath
                                                                     : scriptProject.state.mediaPath;
    cfg->cfrFps = 60.0; // default until ffprobe reports the source rate (see the body's preview block)

    // Probe the source off the main thread (ffprobe blocks): the result seeds the resolution preview, the
    // CFR rate, and the progress duration — keeping the whole optimize flow on ffmpeg/ffprobe, not the
    // mpv player. Clear any stale result first; the body shows a placeholder until it lands.
    transcodeSourceInfo.reset();
    transcodeSourceInfoPath = cfg->sourcePath;
    eventQueue.push(ofs::RequestMediaInfoEvent{cfg->sourcePath});

    // Resolve the deterministic output path ONCE here — fingerprinting reads ~12 KiB off disk, which
    // must not happen every frame the modal is open. The result is stable for the dialog's lifetime.
    const auto outPath = ofs::util::intraOutputPath(ofs::util::fromUtf8(appSettings.intraOutputDir),
                                                    ofs::util::fromUtf8(cfg->sourcePath));
    cfg->outputPath = ofs::util::toUtf8(outPath);
    std::error_code ec;
    const bool resolvable = !outPath.empty() && !cfg->sourcePath.empty();
    const bool exists = resolvable && std::filesystem::exists(outPath, ec);

    // Deferred-start latch for the high-resolution warning: a stacked confirm can't close this body
    // itself (the body owns its close), so on "optimize anyway" the confirm sets this (0/1 = reuse flag)
    // and the body starts + closes on its next frame. -1 means no start pending.
    auto pendingReuse = std::make_shared<int>(-1);

    ofs::showCustomModal(eventQueue, {.title = Str::TranscodeTitle.c_str(),
                                      .width = ImGui::GetFontSize() * 26.0f,
                                      .body = [this, cfg, resolvable, exists, pendingReuse]() -> bool {
                                          return renderTranscodeOptionsBody(cfg, resolvable, exists, pendingReuse);
                                      }});
}

void OfsApp::startTranscode(const std::shared_ptr<ofs::TranscodeConfig> &cfg, bool reuse) {
    cfg->reuseIfExists = reuse;
    eventQueue.push(ofs::TranscodeRequestEvent{*cfg});
}

bool OfsApp::renderTranscodeOptionsBody(const std::shared_ptr<ofs::TranscodeConfig> &cfg, bool resolvable, bool exists,
                                        const std::shared_ptr<int> &pendingReuse) {
    // A deferred high-resolution "optimize anyway" landed: start the run and close.
    if (*pendingReuse >= 0) {
        startTranscode(cfg, *pendingReuse == 1);
        return true;
    }

    ImGui::TextWrapped("%s", Str::TranscodeIntro.c_str());

    // ── Scale (downscale factor expressed as a % of each source dimension; not a target
    // resolution). Percentages are numeric/symbolic, so the labels stay literal like the
    // resolution combo in ProjectConfigWindow — only a stable ###id is attached. ──
    ImGui::SeparatorText(Str::TranscodeScale);
    int scale = static_cast<int>(cfg->scale);
    ImGui::RadioButton("100%###intra_scale_full", &scale, 0);
    ImGui::SameLine();
    ImGui::RadioButton("75%###intra_scale_threequarter", &scale, 1);
    ImGui::SameLine();
    ImGui::RadioButton("50%###intra_scale_half", &scale, 2);
    ImGui::SameLine();
    ImGui::RadioButton("25%###intra_scale_quarter", &scale, 3);
    cfg->scale = static_cast<ofs::ScaleFactor>(scale);

    // ── Resolution preview: the source size and what the chosen factor produces. Filled by the
    // async ffprobe (placeholder until then). The pixel sizes are literal; only labels localize. ──
    if (transcodeSourceInfo && transcodeSourceInfo->valid()) {
        const auto &mi = *transcodeSourceInfo;
        // Seed timing from the probe so the flow needs no player: the duration feeds the progress
        // %, and the source rate pre-fills the CFR field (harmless to update while it stays hidden
        // under Keep-original — once the user picks Constant FPS they own the value).
        if (cfg->sourceDuration <= 0.0)
            cfg->sourceDuration = mi.durationSec;
        if (cfg->timing == ofs::TimingMode::KeepOriginal && mi.fps > 0.0)
            cfg->cfrFps = mi.fps;
        ImGui::TextDisabled("%s", Str::TranscodeResSource.fmt(fmtScratch("{}×{}", mi.width, mi.height)));
        if (cfg->scale != ofs::ScaleFactor::Full) {
            const auto [ow, oh] = ofs::transcode::scaledDimensions(mi.width, mi.height, cfg->scale);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", Str::TranscodeResOutput.fmt(fmtScratch("{}×{}", ow, oh)));
        }
    } else {
        ImGui::TextDisabled("%s", Str::TranscodeResProbing.c_str());
    }

    ImGui::SeparatorText(Str::TranscodeTiming);
    int timing = static_cast<int>(cfg->timing);
    ImGui::RadioButton(Str::TranscodeTimingKeep.id("intra_timing_keep"), &timing, 0);
    ImGui::SameLine();
    ImGui::RadioButton(Str::TranscodeTimingCfr.id("intra_timing_cfr"), &timing, 1);
    cfg->timing = static_cast<ofs::TimingMode>(timing);
    if (cfg->timing == ofs::TimingMode::ConstantFps) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        ImGui::InputDouble("###intra_cfr_fps", &cfg->cfrFps, 0.0, 0.0, "%.3f");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Str::TranscodeFps.c_str());
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ofs::theme::GetStyleColorVec4(AppCol_Warning), "%s", Str::TranscodeCfrNote.c_str());
        ImGui::PopTextWrapPos();
    }

    // ── Codec. H.264 (all-intra) is the safe default for almost every use case; MJPEG is the
    // experimental max-seek-speed path. Selecting MJPEG raises a stacked confirm (huge files) so
    // an expert opts in deliberately — declining reverts to H.264. ──
    ImGui::SeparatorText(Str::TranscodeCodec);
    const auto prevCodec = cfg->codec;
    int codec = static_cast<int>(cfg->codec);
    ImGui::RadioButton(Str::TranscodeCodecH264.id("intra_codec_h264"), &codec, 0);
    ImGui::SameLine();
    ImGui::RadioButton(Str::TranscodeCodecMjpeg.id("intra_codec_mjpeg"), &codec, 1);
    cfg->codec = static_cast<ofs::VideoCodec>(codec);
    if (cfg->codec == ofs::VideoCodec::Mjpeg && prevCodec != ofs::VideoCodec::Mjpeg)
        ofs::confirmAsync(
            eventQueue,
            {.title = Str::TranscodeMjpegWarnTitle.c_str(),
             .message = Str::TranscodeMjpegNote.c_str(),
             // Decline is last so Escape (mapped to the last button) lands on the safe codec.
             .buttons = {Str::TranscodeMjpegWarnAccept.c_str(), Str::TranscodeMjpegWarnDecline.c_str()},
             .severity = ofs::ModalSeverity::Warning},
            [cfg](int idx) {
                if (idx != 0) // declined / dismissed → fall back to the safe codec
                    cfg->codec = ofs::VideoCodec::H264;
            },
            /*stack=*/true);

    // ── Quality. CRF (libx264, 18–28) and MJPEG -q:v (2–31) share the "lower = better/larger"
    // direction, so one hint covers both; the slider id and range switch with the codec. ──
    ImGui::SeparatorText(Str::TranscodeQuality);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(ICON_CIRCLE_HELP).x -
                            ImGui::GetStyle().ItemSpacing.x);
    if (cfg->codec == ofs::VideoCodec::Mjpeg)
        ImGui::SliderInt("###intra_mjpeg_q", &cfg->mjpegQuality, 2, 31);
    else
        ImGui::SliderInt("###intra_crf", &cfg->crf, 18, 28);
    ImGui::SameLine();
    ofs::ui::helpMarker(Str::TranscodeQualityHint.c_str());

    // ── Decode-speed options (each carries a cost disclaimer in its tooltip). fastdecode is an
    // x264 bitstream tweak, so it's hidden under MJPEG (which is already maximally decode-cheap);
    // the 8-bit 4:2:0 pin applies to either codec. ──
    if (cfg->codec == ofs::VideoCodec::H264) {
        ImGui::Checkbox(Str::TranscodeFastDecode.id("intra_fastdecode"), &cfg->fastDecode);
        ImGui::SetItemTooltip("%s", Str::TranscodeFastDecodeTip.c_str());
    }
    ImGui::Checkbox(Str::TranscodeForceYuv420p.id("intra_force_yuv420p"), &cfg->forceYuv420p);
    ImGui::SetItemTooltip("%s", Str::TranscodeForceYuv420pTip.c_str());

    ImGui::Separator();
    ImGui::Checkbox(Str::TranscodeSwitchAfter.id("intra_switch_after"), &cfg->switchAfter);

    ImGui::Separator();
    if (exists)
        ImGui::TextColored(ofs::theme::GetStyleColorVec4(AppCol_Warning), "%s", Str::TranscodeExists.c_str());

    // Kick off a run with the current config (outputPath was resolved at open). Progress shows in
    // the footer task indicator — no modal.
    auto start = [&](bool reuse) { startTranscode(cfg, reuse); };

    // Gate a re-encode whose output is still beyond 4K after the chosen scale: an all-intra copy
    // at that size (worse, an 8K source left at Full) explodes on disk. The stacked confirm nudges
    // a 50%/25% downscale; "optimize anyway" defers the real start to the body's next frame via
    // the latch. Returns whether to close the options modal now. "Use Existing" never routes here
    // — it adopts the already-encoded copy and produces nothing new.
    auto guardedStart = [&](bool reuse) -> bool {
        bool oversized = false;
        if (transcodeSourceInfo && transcodeSourceInfo->valid()) {
            const auto [ow, oh] =
                ofs::transcode::scaledDimensions(transcodeSourceInfo->width, transcodeSourceInfo->height, cfg->scale);
            oversized = static_cast<long long>(ow) * oh > 3840LL * 2160; // beyond UHD 4K
        }
        if (!oversized) {
            start(reuse);
            return true;
        }
        ofs::confirmAsync(
            eventQueue,
            {.title = Str::TranscodeResWarnTitle.c_str(),
             .message = Str::TranscodeResWarnBody.fmt(
                 fmtScratch("{}×{}", transcodeSourceInfo->width, transcodeSourceInfo->height)),
             .buttons = {Str::TranscodeResWarnReduce.c_str(), Str::TranscodeResWarnProceed.c_str()},
             .severity = ofs::ModalSeverity::Warning},
            [pendingReuse, reuse](int idx) {
                if (idx == 1) // "Optimize anyway"
                    *pendingReuse = reuse ? 1 : 0;
            },
            /*stack=*/true);
        return false; // keep the options modal open beneath the warning
    };

    bool close = false;
    ImGui::BeginDisabled(!resolvable);
    if (exists) {
        if (ImGui::Button(Str::TranscodeUseExisting.id("intra_use_existing"))) {
            start(true);
            close = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(Str::TranscodeReoptimize.id("intra_reoptimize")))
            close = guardedStart(false);
    } else if (ImGui::Button(Str::TranscodeStart.id("intra_start"))) {
        close = guardedStart(false);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(Str::TranscodeCancel.id("intra_options_cancel")))
        close = true;
    return close;
}

void OfsApp::pickIntraOutputDir() {
    // Open the folder picker; on a valid choice persist it (same ModifyEvent<AppSettings> path as
    // Preferences) and re-open the options dialog so the user lands back where they started, now with a
    // working output folder.
    ofs::pickFile(eventQueue,
                  {.kind = ofs::FileDialogKind::SelectFolder,
                   .key = "intra_output",
                   .title = Str::PrefIntraChooseFolderTitle.c_str()},
                  [this](std::string path) {
                      if (path.empty())
                          return;
                      eventQueue.push(ofs::ModifyEvent<ofs::AppSettings>{
                          [path = std::move(path)](ofs::AppSettings &s) { s.intraOutputDir = path; }});
                      eventQueue.push(ofs::OpenTranscodeDialogEvent{});
                  });
}

void OfsApp::promptForMissingIntraDir() {
    // Alert + force a re-pick. The only actionable button opens the folder picker; on a valid choice the
    // picker persists it and re-opens the options dialog.
    ofs::confirmAsync(eventQueue,
                      ofs::ModalSpec{.title = Str::TranscodeNoDirTitle.c_str(),
                                     .message = Str::TranscodeNoDirBody.fmt(appSettings.intraOutputDir.c_str()),
                                     .buttons = {Str::PrefIntraChooseFolder.c_str(), Str::AppCancel.c_str()},
                                     .severity = ofs::ModalSeverity::Warning},
                      [this](int idx) {
                          if (idx == 0) // 0 = Choose Folder; 1 = Cancel, -1 = dismissed
                              pickIntraOutputDir();
                      });
}

void OfsApp::maybeOfferOptimize() {
    if (!optimizePromptPending)
        return;
    // The video loads asynchronously after LoadProjectEvent, so defer the decision until we know whether
    // a real source is present: either media is ready, or the project simply has no source path.
    const bool noSource = scriptProject.state.originalMediaPath.empty();
    const bool mediaReady = player && player->hasMedia();
    if (!noSource && !mediaReady)
        return; // still loading — re-check next frame
    optimizePromptPending = false;
    if (noSource || !mediaReady)
        return; // nothing to optimize (no media, or the load failed / was declined)

    // Offer only when optimizing is possible and worthwhile: not already declined for this project's
    // original, on the original source with no existing copy, an output dir + tools available, and nothing
    // already running. (Same gate as the command, minus hasMedia which we just confirmed.)
    const bool canOffer = canOptimizeIntra() && !scriptProject.state.intraOptimizeDeclined &&
                          scriptProject.state.intraMediaPath.empty() && !appSettings.intraOutputDir.empty();
    if (!canOffer)
        return;

    ofs::showCustomModal(
        eventQueue,
        {.title = Str::OptimizePromptTitle.c_str(), .width = ImGui::GetFontSize() * 24.0f, .body = [this]() -> bool {
             ImGui::PushTextWrapPos(0.0f);
             ImGui::TextUnformatted(Str::OptimizePromptBody.c_str());
             ImGui::PopTextWrapPos();
             ImGui::Separator();

             bool close = false;
             if (ImGui::Button(Str::OptimizePromptOptimize.id("optimize_prompt_yes"))) {
                 eventQueue.push(ofs::OpenTranscodeDialogEvent{});
                 close = true;
             }
             ImGui::SameLine();
             if (ImGui::Button(Str::OptimizePromptNotNow.id("optimize_prompt_no"))) {
                 // Decline is per-project, scoped to the current original: a different video is
                 // offered afresh, the same project on reopen is not. There is no global opt-out.
                 eventQueue.push(ofs::DeclineOptimizeEvent{});
                 close = true;
             }
             return close;
         }});
}

void OfsApp::saveActiveLayout() {
    size_t n = 0;
    const char *ini = ImGui::SaveIniSettingsToMemory(&n);
    for (auto &preset : layoutStore.layouts) {
        if (preset.name == layoutStore.activeLayoutName) {
            preset.ini.assign(ini, n);
            layoutStore.save();
            eventQueue.push(
                ofs::NotifyEvent{.level = ofs::NotifyLevel::Success, .message = Str::AppLayoutSaved.fmt(preset.name)});
            return;
        }
    }
}

void OfsApp::revertToActiveLayout() {
    // The actual rebuild/load is deferred to the next onUpdate (before windows are submitted).
    if (layoutStore.activeLayoutName == "Default") {
        pendingDefaultReset_ = true;
        return;
    }
    for (const auto &preset : layoutStore.layouts) {
        if (preset.name == layoutStore.activeLayoutName) {
            pendingLayoutIni_ = preset.ini;
            pendingLayoutApply_ = true;
            return;
        }
    }
    pendingDefaultReset_ = true; // active name no longer exists → fall back to the built-in layout
}

bool OfsApp::renderNewLayoutBody() {
    if (focusNewLayoutName_) {
        ImGui::SetKeyboardFocusHere();
        focusNewLayoutName_ = false;
    }

    ImGui::TextUnformatted(Str::AppNewLayoutName);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool entered = ImGui::InputText("##layoutname", &newLayoutName_, ImGuiInputTextFlags_EnterReturnsTrue);

    const bool empty = newLayoutName_.empty();
    bool duplicate = newLayoutName_ == "Default";
    for (const auto &preset : layoutStore.layouts)
        if (preset.name == newLayoutName_)
            duplicate = true;
    const bool canCreate = !empty && !duplicate;

    ImGui::Separator();
    if (empty)
        ImGui::TextDisabled("%s", Str::AppNewLayoutEnterName.c_str());
    else if (duplicate)
        ImGui::TextColored(ofs::theme::GetStyleColorVec4(AppCol_Error), "%s", Str::AppNewLayoutDuplicate.c_str());

    ImGui::Separator();
    bool close = false;
    ImGui::BeginDisabled(!canCreate);
    if (ImGui::Button(Str::AppNewLayoutCreate.id("newlayout_create")) || (entered && canCreate)) {
        size_t n = 0;
        const char *ini = ImGui::SaveIniSettingsToMemory(&n);
        layoutStore.layouts.push_back({.name = newLayoutName_, .ini = std::string(ini, n)});
        layoutStore.activeLayoutName = newLayoutName_;
        layoutStore.save();
        eventQueue.push(
            ofs::NotifyEvent{.level = ofs::NotifyLevel::Success, .message = Str::AppLayoutCreated.fmt(newLayoutName_)});
        close = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(Str::AppCancel.id("newlayout_cancel")))
        close = true;
    return close;
}

void OfsApp::onEvent(SDL_Event *event) {
    if (event->type == SDL_EVENT_KEY_DOWN) {
        eventQueue.push(ofs::KeyDownEvent{
            .key = event->key.key,
            .modifiers = static_cast<SDL_Keymod>(event->key.mod),
            .repeat = event->key.repeat,
            .keyboardCaptured = ImGui::GetIO().WantCaptureKeyboard,
        });
    } else if (event->type == SDL_EVENT_KEY_UP) {
        // Always delivered (no keyboardCaptured gate) so an in-flight Hold binding ends even if ImGui
        // grabbed the keyboard mid-press — otherwise the hold would stick.
        eventQueue.push(ofs::KeyUpEvent{
            .key = event->key.key,
            .modifiers = static_cast<SDL_Keymod>(event->key.mod),
        });
    } else if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        // A held key whose window loses focus will never deliver its key-up; drop all holds so none
        // survives into a context that can't end it.
        if (bindingSystem)
            bindingSystem->clearHolds();
    } else if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        // OS-level close (Alt+F4, taskbar "Close Window") on our sole SDL window. Route through the
        // same guarded path as the title-bar close button so unsaved changes raise the save prompt
        // instead of being lost. (Native file dialogs are separate OS windows and never reach here.)
        eventQueue.push(ofs::RequestExitEvent{});
    } else if (event->type == SDL_EVENT_DROP_FILE) {
        // Drag-and-drop is a welcome-screen affordance only: with a project open the editor has its own
        // explicit load paths and a stray drop must not blow away the current work. event.drop.data is a
        // SDL-owned UTF-8 path valid for this call, so copy it into the event.
        if (event->drop.data && projectManager && !projectManager->hasActiveProject())
            eventQueue.push(ofs::OpenDroppedFileEvent{event->drop.data});
    } else if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        // A click that gives the window focus is not preceded by any key events — those only reach a
        // focused window — so ImGui's modifier state is still stale on that first click. A shift-click
        // on the timeline would then seek instead of adding a point. Re-sync ImGui's modifier keys from
        // the live OS state; the backend has already queued this click, so both apply the same frame.
        // (Redundant same-value events are deduplicated by ImGui, so the normal focused path is a no-op.)
        const SDL_Keymod mods = SDL_GetModState();
        ImGuiIO &io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiKey_LeftShift, (mods & SDL_KMOD_LSHIFT) != 0);
        io.AddKeyEvent(ImGuiKey_RightShift, (mods & SDL_KMOD_RSHIFT) != 0);
        io.AddKeyEvent(ImGuiKey_LeftCtrl, (mods & SDL_KMOD_LCTRL) != 0);
        io.AddKeyEvent(ImGuiKey_RightCtrl, (mods & SDL_KMOD_RCTRL) != 0);
        io.AddKeyEvent(ImGuiKey_LeftAlt, (mods & SDL_KMOD_LALT) != 0);
        io.AddKeyEvent(ImGuiKey_RightAlt, (mods & SDL_KMOD_RALT) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (mods & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Ctrl, (mods & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (mods & SDL_KMOD_ALT) != 0);
    } else if (event->type == SDL_EVENT_GAMEPAD_ADDED) {
        SDL_Gamepad *gpad = SDL_OpenGamepad(event->gdevice.which);
        if (gpad)
            gamepads_[event->gdevice.which] = gpad;
    } else if (event->type == SDL_EVENT_GAMEPAD_REMOVED) {
        auto it = gamepads_.find(event->gdevice.which);
        if (it != gamepads_.end()) {
            SDL_CloseGamepad(it->second);
            gamepads_.erase(it);
        }
    } else if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        eventQueue.push(ofs::GamepadButtonEvent{
            .which = event->gbutton.which,
            .button = static_cast<SDL_GamepadButton>(event->gbutton.button),
            // ImGui 1.92.x has no WantCaptureGamepad; use NavEnableGamepad+NavActive as proxy.
            .gamepadCaptured =
                (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) != 0 && ImGui::GetIO().NavActive,
        });
    } else if (event->type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        // Always delivered (no capture gate) so an in-flight gamepad-button Hold ends even if ImGui
        // grabbed the pad mid-press — mirror of the SDL_EVENT_KEY_UP arm.
        eventQueue.push(ofs::GamepadButtonUpEvent{
            .which = event->gbutton.which,
            .button = static_cast<SDL_GamepadButton>(event->gbutton.button),
        });
    }
}
