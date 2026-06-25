#pragma once

#include "Core/AxisClip.h"
#include "Core/Events.h"
#include "Core/GraphPresetEvents.h"
#include "Core/ProjectLifecycleEvents.h"
#include "Core/SceneViewEvents.h"
#include "Core/ScriptNodeEvents.h"
#include "Core/StandardAxis.h"
#include "Format/AppSettings.h"
#include "Format/Project.h"
#include "Services/EffectRegistry.h"
#include "Util/Coro.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace ofs {
class EventQueue;
class JobSystem;
struct ScriptProject;

class ProjectManager {
  public:
    ProjectManager(ScriptProject &project, EventQueue &eq, const AppSettings &appSettings, JobSystem &jobSystem,
                   const EffectRegistryState &effectReg);
    ~ProjectManager();

    void update(float dt);

    [[nodiscard]] std::filesystem::path getCurrentProjectPath() const;
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> getLastSaveTime() const { return lastSaveTime; }
    [[nodiscard]] const char *getProjectTitle() const;
    [[nodiscard]] bool isDirty() const;
    // True once a project exists (saved file, loaded media, or any present axis). Drives which
    // project-scoped menu items are enabled.
    [[nodiscard]] bool hasActiveProject() const;

  private:
    // Project lifecycle — invoked from event handlers only. Each is a coroutine that may co_await an
    // unsaved-changes / confirmation modal or a native file dialog; its result arrives a frame later.
    //
    // guardUnsaved prompts about unsaved changes (and may co_await a save dialog), then runs
    // `proceed` only if the user didn't cancel and any requested save succeeded. EVERY co_await in
    // guardUnsaved / the start* continuations is a direct child of that one co::Fire, so the
    // ModalManager always holds the running flow's own handle — clean teardown, no nested-frame leak.
    co::Fire guardUnsaved(std::function<void()> proceed);
    co::Fire startOpenOrNewProject();           // doClose + one picker: .ofp opens, funscript/media start a new project
    void openPathByExtension(std::string file); // dispatch a chosen/dropped path: .ofp opens, funscript/media start new
    void setupDefaultAxes();                    // restore the default L0–R3 device axes on a fresh project
    co::Fire initNewProject(std::string mediaPath); // default axes + funscript auto-discovery for a fresh project
    // New project around a script. Coroutine: the sibling-media scan runs on the main thread (cheap —
    // no parsing), but the funscript itself is parsed on a JobSystem worker via JobAwait.
    co::Fire initNewProjectFromFunscript(std::filesystem::path funscriptPath);
    // Timeline span for a media-less project: never 0, and long enough to show every existing action
    // (the latest action time, with a small tail, when that exceeds kDefaultDummyDuration).
    [[nodiscard]] double mediaLessDuration() const;
    co::Fire relocateFlow();
    co::Fire saveFlow(bool saveAs); // standalone Save / Save As (menu), independent of guardUnsaved
    void doClose();                 // synchronous close work (after any unsaved-changes prompt)
    co::Fire importFunscript();
    // targetPath engaged → write there and skip the picker (Quick Export re-export). For
    // exportMultipleFunscript10 it is the output folder; for exportMultiAxisFunscript the output file.
    co::Fire exportMultiAxisFunscript(std::vector<StandardAxis> axes, bool useChannels,
                                      std::optional<std::string> targetPath);
    co::Fire exportMultipleFunscript10(std::vector<StandardAxis> axes, std::optional<std::string> targetPath);
    // Persist the just-used export parameters so Quick Export can replay them dialog-free.
    void recordLastExport(int format, std::vector<StandardAxis> axes, std::string outputPath);

    // Request event handlers
    void onOpenOrNewProjectRequest(const OpenOrNewProjectRequestEvent &);
    void onOpenProjectRequest(const OpenProjectRequestEvent &event);
    void onCreateEmptyProject(const CreateEmptyProjectEvent &);
    void onOpenDroppedFile(const OpenDroppedFileEvent &event);
    void onChangeDummyDuration(const ChangeDummyDurationEvent &event);
    void onChangeMediaPath(const ChangeMediaPathEvent &event);
    void onDeclineOptimize();
    void onCloseProjectRequest(const CloseProjectRequestEvent &);
    void onRequestExit(const RequestExitEvent &);
    void onImportFunscriptRequest(const ImportFunscriptRequestEvent &);
    // Applies a parsed import (pushed by importFunscript() after I/O). Places each axis, then notifies:
    // an error if any axis was dropped because the scratch slots (S0–S9) are full, else a success toast.
    void onImportFunscriptData(const ImportFunscriptDataEvent &event);
    void onExportFunscriptRequest(const ExportFunscriptRequestEvent &event);

    // Mutation event handlers
    void onSaveProjectEvent(const SaveProjectEvent &event);
    void onAxisSelected(const AxisSelectedEvent &event);
    void onSetAxisGrouping(const SetAxisGroupingEvent &event);
    void onRemoveSelectedActions(const RemoveSelectedActionsEvent &event);
    void onAddActionAtTime(const AddActionAtTimeEvent &event);
    void onOverlaySettingsChanged(const OverlaySettingsChangedEvent &event);
    void onMoveSelectionPosition(const MoveSelectionPositionEvent &event);
    void onMoveSelectionTime(const MoveSelectionTimeEvent &event);
    void onMoveActionToCurrentTime(const MoveActionToCurrentTimeEvent &event);
    void onPasteActions(const PasteActionsEvent &event);
    void onRemoveActionAtTime(const RemoveActionAtTimeEvent &event);
    void onMoveAction(const MoveActionEvent &event);
    void onSimulatorPositionChanged(const SimulatorPositionChangedEvent &event);
    void onAddScratchAxis(const AddScratchAxisEvent &event);
    void onRemoveAxis(const RemoveAxisEvent &event);
    void onToggleAxisVisibility(const ToggleAxisVisibilityEvent &event);
    void onToggleAxisPanelVisibility(const ToggleAxisPanelVisibilityEvent &event);
    void onShowMultiAxis(const ShowMultiAxisEvent &event);
    void onShowL0Only(const ShowL0OnlyEvent &event);
    void onToggleAxisLock(const ToggleAxisLockEvent &event);
    void onCreateRegion(const CreateRegionEvent &event);
    void onDeleteRegion(const DeleteRegionEvent &event);
    void onModifyRegion(const ModifyRegionEvent &event);
    void onMoveRegionNodes(const MoveRegionNodesEvent &event);
    void onBakeRegion(const BakeRegionEvent &event);
    void onAssignAxis(const AssignAxisToRegionEvent &event);
    co::Fire onSaveGraph(SaveGraphEvent event);
    co::Fire onLoadGraph(LoadGraphEvent event);
    void onApplyGraphRemap(const ApplyGraphRemapEvent &event);
    void onCancelGraphLoad(const CancelGraphLoadEvent &event);
    void onRemapCurrentGraph(const RemapCurrentGraphEvent &event);
    void onConfirmGraphTrust(const ConfirmGraphTrustEvent &event);
    void onReviewGraphScripts(const ReviewGraphScriptsEvent &event);
    void onSaveEmbeddedScript(const SaveEmbeddedScriptEvent &event);
    void onVideoModeChanged(const VideoModeChangedEvent &event);
    void onVideoResolutionChanged(const VideoResolutionChangedEvent &event);
    void onSelectRegion(const SelectRegionEvent &event);
    void onCaptureOverlayAnchor(const CaptureOverlayAnchorEvent &event);
    void onCaptureVideoFraming(const CaptureVideoFramingEvent &event);
    void onCaptureSimInverted(const CaptureSimInvertedEvent &event);
    // The SceneView to write a capture into: the chapter containing the cursor (created from the
    // current activeSceneView if it has none yet), or defaultSceneView when outside any chapter.
    // Updates lastSceneViewIdx so resolveActiveSceneView() won't re-restore over the edit.
    SceneView &sceneViewAtCursor();
    // Resolve the scene view for the current cursor: if the active chapter changed since last
    // frame, set project.activeSceneView (chapter override or defaultSceneView) and push a
    // RestoreSceneViewEvent so the video window snaps its camera. Called once per update().
    void resolveActiveSceneView();
    void onClearRegionSelection(const ClearRegionSelectionEvent &event);
    void onUpdateTimelineView(const UpdateTimelineViewEvent &event);
    void onSetTimelineShowPoints(const SetTimelineShowPointsEvent &event);
    // Apply a project's saved resume position once the freshly opened media reports its real length.
    // The seek is deferred to here because the load fires LoadVideoEvent asynchronously: at load time
    // the player's duration is still 0, so a SeekEvent would clamp the target to 0.
    void onDurationChanged(const DurationChangedEvent &event);

    // Metadata, bookmarks/chapters, simulator settings and video-player settings are handled inline
    // via on<ModifyEvent<T>>() lambdas registered in the constructor (see ModifyEvent<T>).
    void onCommitAxisActions(const CommitAxisActionsEvent &event);
    void onSetAxisSelection(const SetAxisSelectionEvent &event);
    void onCopySelection(const CopySelectionEvent &event);
    void onSetPluginProjectData(const SetPluginProjectDataEvent &event);

    void setDirty(bool dirty = true);

    // Normalize `file` to an .ofp path and write it; returns saveProjectInternal's success.
    bool saveProjectToPath(const std::string &file);
    // Load a project file. Coroutine: the .ofp is read + CBOR-decoded on a JobSystem worker via
    // JobAwait, then applied (clearProject + loadFromProject + events) on the main thread when it
    // resumes. Takes the path by value — the frame outlives the caller's argument across the await.
    co::Fire loadProjectInternal(std::filesystem::path path);
    bool saveProjectInternal(const std::filesystem::path &path);
    void scheduleWrite(Project p, const std::filesystem::path &path, bool isUserSave, long long captureMs);
    void finalizePendingWrite(bool ok);
    // Resolve a missing media path. Returns true if resolved synchronously (caller drives the
    // video load); false if a relocateFlow was started, which loads the video when it finishes.
    bool resolveMediaPath();
    void openProjectVideo();
    // Pick which remembered source feeds the player: prefer the intra-optimized copy whenever it is set
    // and exists on disk, else the original. Sets project.activeSource and the resolved state.mediaPath.
    // Shared by load, post-transcode, and the source toggle.
    void resolveActiveMedia();
    // When no intra copy is recorded yet, look for one the shared output dir already holds for this
    // exact source content (matched by the content fingerprint, the same key intraOutputPath() uses).
    // Lets a freshly-opened original automatically adopt an optimized copy produced earlier — here or
    // in another project, since the copies are shared and content-addressed. A ~12 KiB fingerprint
    // read, so callers invoke it once per media establishment, not per frame.
    void discoverExistingIntra();
    void moveAction(StandardAxis role, double fromAt, double toAt, int toPos);
    // Replace a region's graph with `graph` (roles already final) and set its axis
    // set to `newRoles`, re-evaluating the union of old and new roles. If `name` is
    // non-empty it replaces the region's name. Returns false if the region is gone.
    // Validates nothing — callers check axis overlap first.
    bool applyLoadedGraph(int regionId, ProcessingNodeGraph graph, AxisRoles newRoles, const std::string &name, int hz);
    void clearProject();
    void loadFromProject(const Project &project);
    void saveToProject(Project &project) const;

    // Persistent record of graphs the user has accepted to run embedded scripts from. Keyed by a
    // hash of the embedded code only, so trust survives across sessions and is asked for again only
    // when that code changes (not when nodes are merely moved or rewired). See graphTrustHash.
    void loadTrustedGraphs();
    void saveTrustedGraphs() const;
    // Content hashes of every shipped library script (data.pak data/scripts/lib/*). A shipped script dropped
    // from the add menu is embedded into the node verbatim, so its embedded source hashes to one of
    // these — graphTrustHash treats those as already trusted (first-party code, like shipped plugins),
    // and a graph carrying only shipped scripts never raises the trust prompt. Loaded once at startup.
    void loadShippedScriptHashes();

    struct PendingWrite {
        std::future<bool> future;
        std::filesystem::path path;
        bool isUserSave;
    };

    ScriptProject &project;
    EventQueue &eq;
    const AppSettings &appSettings;
    JobSystem &jobSystem;
    const EffectRegistryState &effectReg;
    std::optional<PendingWrite> pendingWrite;
    std::optional<std::chrono::steady_clock::time_point> lastSaveTime;
    std::array<double, 5> backupElapsed{};
    std::unordered_set<std::string> trustedGraphHashes;
    std::unordered_set<std::string> shippedScriptHashes; // content hashes of data.pak data/scripts/lib/* (auto-trusted)
    // Multi-axis action clipboard (internal service state, not project document state). One AxisClip per
    // copied axis. A single entry is broadcast across the active group on paste; multiple entries paste
    // each back onto its own role. Populated by CopySelectionEvent, consumed by PasteActionsEvent.
    std::vector<AxisClip> clipboard;
    // Active-chapter index from the last resolveActiveSceneView() (-1 = outside any chapter).
    // Sentinel -2 forces a restore on the first update after a load. See resolveActiveSceneView.
    int lastSceneViewIdx = -2;
    // A just-loaded project's resume position, awaiting the opened media's real duration before it can
    // seek (see onDurationChanged). Set on load, consumed by the first valid DurationChangedEvent.
    std::optional<double> pendingResumeSeek;
};

} // namespace ofs
