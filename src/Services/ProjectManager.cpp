#include "ProjectManager.h"
#include "Core/BookmarkChapterState.h"
#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/FunscriptMetadata.h"
#include "Core/OverlaySettings.h"
#include "Core/ProcessingRegion.h"
#include "Core/ScriptProject.h"
#include "Core/SimulatorSettings.h"
#include "Core/TimeSlot.h"
#include "Core/TranscodeEvents.h"
#include "Format/BackupArchive.h"
#include "Format/Funscript.h"
#include "Format/MediaTypes.h"
#include "Localization/Translator.h"
#include "Services/JobAwait.h"
#include "Services/JobSystem.h"
#include "Services/ScriptRegistry.h"
#include "UI/Modals.h"
#include "Util/ColorGen.h"
#include "Util/Coro.h"
#include "Util/FileFingerprint.h"
#include "Util/FileUtil.h"
#include "Util/FrameAllocator.h"
#include "Util/JsonUtil.h"
#include "Util/Log.h"
#include "Util/Mnemonic.h"
#include "Util/PathUtil.h"
#include "Util/Resources.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <limits>
#include <nlohmann/json.hpp>
#include <picosha2.h>
#include <random>
#include <ranges>
#include <set>
#include <string>
#include <string_view>

namespace ofs {

static constexpr double kBackupIntervalSeconds = 60.0;

// SHA-256 over a graph's embedded script sources — the only trust-relevant, code-executing content.
// Sources are length-prefixed and ordered by node id so the digest is stable regardless of node
// order and unambiguous across concatenation. Returns "" when the graph embeds no scripts that need
// trusting. Links, positions, and non-script fields are excluded on purpose: re-trust is required
// only when the executable code actually changes, not when a node is repositioned or rewired.
//
// Shipped library scripts are first-party code (like shipped plugins): a node carrying source that
// hashes to a known shipped script is auto-trusted and excluded from the digest, so a graph whose
// embedded scripts are all shipped never raises the trust prompt, while a mix still prompts for the
// user-authored ones.
static std::string graphTrustHash(const ProcessingNodeGraph &graph,
                                  const std::unordered_set<std::string> &shippedScriptHashes) {
    std::vector<const ProcessingGraphNode *> scripts;
    for (const auto &n : graph.nodes)
        if (n.scriptEmbedded() && !shippedScriptHashes.contains(scriptContentHash(n.scriptEmbeddedSource)))
            scripts.push_back(&n);
    if (scripts.empty())
        return {};
    std::ranges::sort(scripts, {}, [](const ProcessingGraphNode *n) { return n->id; });
    std::string blob;
    for (const auto *n : scripts)
        blob += fmt::format("{}:{}\n", n->scriptEmbeddedSource.size(), n->scriptEmbeddedSource);
    return picosha2::hash256_hex_string(blob);
}

static std::filesystem::path trustedGraphsPath() {
    return ofs::util::getPrefPath() / "trusted_graphs.json";
}

ProjectManager::ProjectManager(ScriptProject &project, EventQueue &eq, const AppSettings &appSettings,
                               JobSystem &jobSystem, const EffectRegistryState &effectReg)
    : project(project), eq(eq), appSettings(appSettings), jobSystem(jobSystem), effectReg(effectReg) {
    // Generic resume channel for JobAwait — a flow that moved blocking I/O to a worker resumes here
    // when the worker finishes. The handler is state-free; ProjectManager is currently its only user,
    // so it is registered alongside ProjectManager's own handlers.
    eq.on<ResumeFlowEvent>([](const ResumeFlowEvent &e) {
        if (e.handle)
            e.handle.resume();
    });
    eq.on<OpenOrNewProjectRequestEvent>(
        [this](const OpenOrNewProjectRequestEvent &e) { onOpenOrNewProjectRequest(e); });
    eq.on<OpenProjectRequestEvent>([this](const OpenProjectRequestEvent &e) { onOpenProjectRequest(e); });
    eq.on<CreateEmptyProjectEvent>([this](const CreateEmptyProjectEvent &e) { onCreateEmptyProject(e); });
    eq.on<OpenDroppedFileEvent>([this](const OpenDroppedFileEvent &e) { onOpenDroppedFile(e); });
    eq.on<ChangeDummyDurationEvent>([this](const ChangeDummyDurationEvent &e) { onChangeDummyDuration(e); });
    eq.on<ChangeMediaPathEvent>([this](const ChangeMediaPathEvent &e) { onChangeMediaPath(e); });
    eq.on<DeclineOptimizeEvent>([this](const DeclineOptimizeEvent &) { onDeclineOptimize(); });
    eq.on<CloseProjectRequestEvent>([this](const CloseProjectRequestEvent &e) { onCloseProjectRequest(e); });
    eq.on<RestoreBackupRequestEvent>([this](const RestoreBackupRequestEvent &e) { onRestoreBackupRequest(e); });
    eq.on<RequestExitEvent>([this](const RequestExitEvent &e) { onRequestExit(e); });
    eq.on<ImportFunscriptRequestEvent>([this](const ImportFunscriptRequestEvent &e) { onImportFunscriptRequest(e); });
    eq.on<ImportFunscriptDataEvent>([this](const ImportFunscriptDataEvent &e) { onImportFunscriptData(e); });
    eq.on<ExportFunscriptRequestEvent>([this](const ExportFunscriptRequestEvent &e) { onExportFunscriptRequest(e); });
    eq.on<SaveProjectEvent>([this](const SaveProjectEvent &e) { onSaveProjectEvent(e); });
    eq.on<AxisSelectedEvent>([this](const AxisSelectedEvent &e) { onAxisSelected(e); });
    eq.on<SetAxisGroupingEvent>([this](const SetAxisGroupingEvent &e) { onSetAxisGrouping(e); });
    eq.on<RemoveSelectedActionsEvent>([this](const RemoveSelectedActionsEvent &e) { onRemoveSelectedActions(e); });
    eq.on<AddActionAtTimeEvent>([this](const AddActionAtTimeEvent &e) { onAddActionAtTime(e); });
    eq.on<OverlaySettingsChangedEvent>([this](const OverlaySettingsChangedEvent &e) { onOverlaySettingsChanged(e); });
    eq.on<MoveSelectionPositionEvent>([this](const MoveSelectionPositionEvent &e) { onMoveSelectionPosition(e); });
    eq.on<MoveSelectionTimeEvent>([this](const MoveSelectionTimeEvent &e) { onMoveSelectionTime(e); });
    eq.on<MoveActionToCurrentTimeEvent>(
        [this](const MoveActionToCurrentTimeEvent &e) { onMoveActionToCurrentTime(e); });
    eq.on<CopySelectionEvent>([this](const CopySelectionEvent &e) { onCopySelection(e); });
    eq.on<PasteActionsEvent>([this](const PasteActionsEvent &e) { onPasteActions(e); });
    eq.on<RemoveActionAtTimeEvent>([this](const RemoveActionAtTimeEvent &e) { onRemoveActionAtTime(e); });
    eq.on<MoveActionEvent>([this](const MoveActionEvent &e) { onMoveAction(e); });
    eq.on<SimulatorPositionChangedEvent>(
        [this](const SimulatorPositionChangedEvent &e) { onSimulatorPositionChanged(e); });
    eq.on<AddScratchAxisEvent>([this](const AddScratchAxisEvent &e) { onAddScratchAxis(e); });
    eq.on<RemoveAxisEvent>([this](const RemoveAxisEvent &e) { onRemoveAxis(e); });
    eq.on<ToggleAxisVisibilityEvent>([this](const ToggleAxisVisibilityEvent &e) { onToggleAxisVisibility(e); });
    eq.on<ToggleAxisPanelVisibilityEvent>(
        [this](const ToggleAxisPanelVisibilityEvent &e) { onToggleAxisPanelVisibility(e); });
    eq.on<ShowMultiAxisEvent>([this](const ShowMultiAxisEvent &e) { onShowMultiAxis(e); });
    eq.on<ShowL0OnlyEvent>([this](const ShowL0OnlyEvent &e) { onShowL0Only(e); });
    eq.on<ToggleAxisLockEvent>([this](const ToggleAxisLockEvent &e) { onToggleAxisLock(e); });
    eq.on<CreateRegionEvent>([this](const CreateRegionEvent &e) { onCreateRegion(e); });
    eq.on<DeleteRegionEvent>([this](const DeleteRegionEvent &e) { onDeleteRegion(e); });
    eq.on<ModifyRegionEvent>([this](const ModifyRegionEvent &e) { onModifyRegion(e); });
    eq.on<MoveRegionNodesEvent>([this](const MoveRegionNodesEvent &e) { onMoveRegionNodes(e); });
    eq.on<BakeRegionEvent>([this](const BakeRegionEvent &e) { onBakeRegion(e); });
    eq.on<AssignAxisToRegionEvent>([this](const AssignAxisToRegionEvent &e) { onAssignAxis(e); });
    eq.on<SaveGraphEvent>([this](const SaveGraphEvent &e) { onSaveGraph(e); });
    eq.on<LoadGraphEvent>([this](const LoadGraphEvent &e) { onLoadGraph(e); });
    eq.on<ApplyGraphRemapEvent>([this](const ApplyGraphRemapEvent &e) { onApplyGraphRemap(e); });
    eq.on<CancelGraphLoadEvent>([this](const CancelGraphLoadEvent &e) { onCancelGraphLoad(e); });
    eq.on<RemapCurrentGraphEvent>([this](const RemapCurrentGraphEvent &e) { onRemapCurrentGraph(e); });
    eq.on<ConfirmGraphTrustEvent>([this](const ConfirmGraphTrustEvent &e) { onConfirmGraphTrust(e); });
    eq.on<ReviewGraphScriptsEvent>([this](const ReviewGraphScriptsEvent &e) { onReviewGraphScripts(e); });
    eq.on<SaveEmbeddedScriptEvent>([this](const SaveEmbeddedScriptEvent &e) { onSaveEmbeddedScript(e); });
    eq.on<VideoModeChangedEvent>([this](const VideoModeChangedEvent &e) { onVideoModeChanged(e); });
    eq.on<VideoResolutionChangedEvent>([this](const auto &e) { onVideoResolutionChanged(e); });
    eq.on<SelectRegionEvent>([this](const auto &e) { onSelectRegion(e); });
    eq.on<CaptureOverlayAnchorEvent>([this](const CaptureOverlayAnchorEvent &e) { onCaptureOverlayAnchor(e); });
    eq.on<CaptureVideoFramingEvent>([this](const CaptureVideoFramingEvent &e) { onCaptureVideoFraming(e); });
    eq.on<CaptureSimInvertedEvent>([this](const CaptureSimInvertedEvent &e) { onCaptureSimInverted(e); });
    eq.on<ClearRegionSelectionEvent>([this](const auto &e) { onClearRegionSelection(e); });
    eq.on<SetProcPanelLockedEvent>([this](const auto &e) { onSetProcPanelLocked(e); });
    eq.on<UpdateTimelineViewEvent>([this](const auto &e) { onUpdateTimelineView(e); });
    eq.on<SetTimelineShowPointsEvent>([this](const auto &e) { onSetTimelineShowPoints(e); });
    eq.on<SetTimelineShowWaveformEvent>([this](const auto &e) { onSetTimelineShowWaveform(e); });
    eq.on<DurationChangedEvent>([this](const DurationChangedEvent &e) { onDurationChanged(e); });

    // this->project below (not the same-named ctor parameter that shadows it here).
    eq.on<ModifyEvent<FunscriptMetadata>>([this](const ModifyEvent<FunscriptMetadata> &e) {
        e.apply(this->project.metadata);
        setDirty();
    });
    eq.on<ModifyBookmarkChapterEvent>([this](const ModifyBookmarkChapterEvent &e) {
        const size_t before = this->project.bookmarks.bookmarks.size() + this->project.bookmarks.chapters.size();
        e.apply(this->project.bookmarks);
        // The opaque mutator may have appended or edited out of order; re-assert the sorted-by-time
        // invariant that the rest of the app relies on.
        this->project.bookmarks.sortBookmarks();
        this->project.bookmarks.sortChapters();
        setDirty();
        // Detect a discrete add/remove here (the one place that sees the mutation) so observers don't have
        // to classify every call site. An in-place edit leaves the count unchanged and emits nothing.
        const size_t after = this->project.bookmarks.bookmarks.size() + this->project.bookmarks.chapters.size();
        if (after != before)
            this->eq.push(BookmarkChapterCountChangedEvent{.added = after > before});
    });
    // Simulator settings intentionally do NOT setDirty(): they are app-level prefs mirrored into
    // project.simulator for rendering and persisted via AppSettings, not part of project dirty state.
    eq.on<ModifyEvent<SimulatorState>>(
        [this](const ModifyEvent<SimulatorState> &e) { e.apply(this->project.simulator); });
    eq.on<ModifyEvent<VideoPlayerState>>([this](const ModifyEvent<VideoPlayerState> &e) {
        e.apply(this->project.videoPlayer);
        setDirty();
    });
    eq.on<CommitAxisActionsEvent>([this](const CommitAxisActionsEvent &e) { onCommitAxisActions(e); });
    eq.on<SetAxisSelectionEvent>([this](const SetAxisSelectionEvent &e) { onSetAxisSelection(e); });
    eq.on<SetPluginProjectDataEvent>([this](const SetPluginProjectDataEvent &e) { onSetPluginProjectData(e); });

    loadTrustedGraphs();
    loadShippedScriptHashes();
}

void ProjectManager::loadShippedScriptHashes() {
    // Hash the shipped library exactly as it was packed (same resource read + same content hash the UI
    // uses when it embeds a shipped script), so an embedded shipped node's source matches byte-for-byte.
    constexpr std::string_view kLibPrefix = "data/scripts/lib/";
    for (const auto &name : ofs::res::list(kLibPrefix))
        if (std::string_view(name).ends_with(".cs"))
            if (auto src = ofs::res::readText(name))
                shippedScriptHashes.insert(scriptContentHash(*src));
}

void ProjectManager::loadTrustedGraphs() {
    // Missing file is the normal first-run state — nothing trusted yet, no error to report.
    if (!std::filesystem::exists(trustedGraphsPath()))
        return;
    if (auto hashes = ofs::util::loadJsonFile<std::unordered_set<std::string>>(trustedGraphsPath(), "trusted graphs"))
        trustedGraphHashes = std::move(*hashes);
}

void ProjectManager::saveTrustedGraphs() const {
    try {
        ofs::util::writeFileAtomic(trustedGraphsPath(), nlohmann::json(trustedGraphHashes).dump(2));
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to save trusted graphs: {}", e.what());
    }
}

ProjectManager::~ProjectManager() {
    if (pendingWrite.has_value())
        finalizePendingWrite(pendingWrite->future.get());
}

namespace {
// Apply fn to each axis in `roles` that can actually be edited (unlocked). Used to fan a
// single activeAxis-targeted edit across the active group; with no group this runs once for activeAxis.
template <class F> void forEachEditable(ScriptProject &p, AxisRoles roles, F &&fn) {
    for (size_t i = 0; i < kStandardAxisCount; ++i) {
        if (!roles.test(i))
            continue;
        auto &ax = p.axes[i];
        if (ax.isLocked)
            continue;
        fn(static_cast<StandardAxis>(i), ax);
    }
}

// The axes an edit applies to. Only the lead-targeted (UI) case fans out across the active group;
// an event that names a different axis (a plugin/test targeting a specific axis) stays single-axis,
// so existing single-axis behavior is preserved when no group is active or the event isn't the lead.
// `fanToGroup == false` forces single-axis even for a lead-targeted event: the EditIntentRouter sets it
// for a ReplacePerAxis resolution, where it has already fanned across the group above the seam and each
// axis's intents must apply verbatim (see Events.h AddActionAtTimeEvent::fanToGroup).
AxisRoles editTargets(const ScriptProject &p, StandardAxis evAxis, bool fanToGroup = true) {
    if (fanToGroup && evAxis == p.state.activeAxis)
        return p.effectiveEditSet();
    AxisRoles r;
    if (evAxis < StandardAxis::Count)
        r.set(static_cast<size_t>(evAxis));
    return r;
}

// "Visible in the panel" = a row exists for it in the timeline strip. Only such axes may be activated
// or grouped — you can't lead/group an axis you can't see or interact with.
bool inPanel(const ScriptProject &p, StandardAxis role) {
    const auto i = static_cast<size_t>(role);
    return role < StandardAxis::Count && p.axes[i].showInStrip;
}
} // namespace

void ProjectManager::onAxisSelected(const AxisSelectedEvent &event) {
    if (!inPanel(project, event.role))
        return; // can't activate an axis that isn't shown in the panel
    project.state.activeAxis = event.role;
    if (!project.procPanelLocked)
        project.procSelRegionId = -1;   // a locked processing panel stays pinned across an axis switch
    project.state.axesGrouping.reset(); // activating an axis dissolves any multi-axis group
}

void ProjectManager::onSetAxisGrouping(const SetAxisGroupingEvent &event) {
    // Restrict the group to panel-visible axes; a hidden/absent axis can be neither member nor lead.
    AxisRoles g;
    for (size_t i = 0; i < kStandardAxisCount; ++i)
        if (event.roles.test(i) && inPanel(project, static_cast<StandardAxis>(i)))
            g.set(i);

    StandardAxis lead = event.lead;
    if (!inPanel(project, lead)) {
        // Requested lead isn't selectable — fall back to the first valid member, or bail if none.
        lead = StandardAxis::Count;
        for (size_t i = 0; i < kStandardAxisCount; ++i)
            if (g.test(i)) {
                lead = static_cast<StandardAxis>(i);
                break;
            }
    }
    if (lead >= StandardAxis::Count)
        return; // nothing valid to activate or group

    const AxisRoles prevGrouping = project.state.axesGrouping;
    project.state.activeAxis = lead;
    project.procSelRegionId = -1;
    if (g.count() > 1)
        g.set(static_cast<size_t>(lead)); // the lead is always part of its group
    else
        g.reset(); // a single-axis "group" is just normal single-axis editing
    project.state.axesGrouping = g;
    // Cue only a *real* multi-axis grouping that differs from before; dissolving back to single-axis
    // editing or re-issuing the same group is silent (the request fires for both).
    if (g.count() > 1 && g != prevGrouping)
        eq.push(AxisGroupingChangedEvent{});
}

void ProjectManager::onSaveProjectEvent(const SaveProjectEvent &event) {
    saveFlow(event.saveAs);
}

void ProjectManager::onRemoveSelectedActions(const RemoveSelectedActionsEvent &event) {
    const AxisRoles targets = editTargets(project, event.axis, event.fanToGroup);
    const bool group = targets.count() > 1;
    forEachEditable(project, targets, [&](StandardAxis role, AxisState &axis) {
        if (!axis.selection.empty()) {
            project.mutate(
                role,
                [](AxisState &a) {
                    for (const auto &action : a.selection)
                        a.actions.erase(action);
                    a.selection.clear();
                },
                eq);
        } else if (!group) {
            // Single-axis convenience: with nothing selected, delete the action nearest the playhead.
            // A group never deletes unselected points out from under members.
            const ScriptAxisAction *closest = closestActionByTime(axis.actions, project.playback.cursorPos);
            if (!closest)
                return;
            ScriptAxisAction toRemove = *closest;
            project.mutate(role, [toRemove](AxisState &a) { a.actions.erase(toRemove); }, eq);
        }
    });
}

void ProjectManager::onAddActionAtTime(const AddActionAtTimeEvent &event) {
    ScriptAxisAction newAction = clampedAction(event.time, event.pos);
    forEachEditable(project, editTargets(project, event.axis, event.fanToGroup), [&](StandardAxis role, AxisState &) {
        project.mutate(
            role,
            [newAction](AxisState &a) {
                a.actions.erase(ScriptAxisAction{newAction.at, 0});
                a.actions.insert(newAction);
            },
            eq);
    });
}

namespace {
ModalSpec unsavedChangesSpec() {
    return {.title = Str::PmUnsavedTitle.c_str(),
            .message = Str::PmUnsavedBody.c_str(),
            .buttons = {Str::PmBtnSave.c_str(), Str::PmBtnDontSave.c_str(), Str::PmBtnCancel.c_str()},
            .severity = ModalSeverity::Warning};
}
} // namespace

void ProjectManager::doClose() {
    clearProject(); // resetDocument() already clears filePath/mediaPath along with the rest of the document
    eq.push(CloseVideoEvent{});
}

co::Fire ProjectManager::startOpenOrNewProject() {
    doClose();

    // Merged New+Open: one picker for every intent — an .ofp opens that project, a .funscript starts
    // a new project around that script, and a media file starts a new project from that media. A
    // single dialog means the user can't land in "the wrong dialog" after committing.
    std::vector<std::string> patterns = {"*.ofp", "*.funscript"};
    auto media = mediaFilterPatterns();
    patterns.insert(patterns.end(), media.begin(), media.end());
    std::string file = co_await FileDialog{eq,
                                           {.kind = FileDialogKind::Open,
                                            .key = "project",
                                            .title = Str::PmDlgOpenProject.c_str(),
                                            .filterPatterns = std::move(patterns),
                                            .filterDesc = Str::PmFilterProjectsMedia.c_str()}};

    if (file.empty()) {
        const int choice = co_await Confirm{eq,
                                            {.title = Str::PmNewProjectTitle.c_str(),
                                             .message = Str::PmNewProjectBody.c_str(),
                                             .buttons = {Str::PmBtnYes.c_str(), Str::PmBtnNo.c_str()},
                                             .severity = ModalSeverity::Info}};
        if (choice != 0) // not "Yes"
            co_return;
        initNewProject("");
        co_return;
    }

    openPathByExtension(std::move(file)); // dialog results are already UTF-8
}

void ProjectManager::openPathByExtension(std::string file) {
    std::filesystem::path path = ofs::util::fromUtf8(file);
    const std::string ext = lowerExtension(path);
    if (ext == ".ofp")
        loadProjectInternal(path);
    else if (ext == ".funscript")
        initNewProjectFromFunscript(path);
    else
        initNewProject(std::move(file));
}

void ProjectManager::onCreateEmptyProject(const CreateEmptyProjectEvent &) {
    // Dedicated welcome-screen button: a fresh media-less project with no picker and no confirm.
    guardUnsaved([this] {
        doClose();
        initNewProject("");
    });
}

void ProjectManager::onOpenDroppedFile(const OpenDroppedFileEvent &event) {
    if (event.path.empty())
        return;
    guardUnsaved([this, file = event.path] {
        doClose();
        openPathByExtension(file);
    });
}

void ProjectManager::setupDefaultAxes() {
    constexpr auto kDeviceAxisCount = static_cast<size_t>(StandardAxis::S0);
    for (size_t i = 0; i < kDeviceAxisCount; ++i) {
        const auto sa = static_cast<StandardAxis>(i);
        project.restoreAxis(sa, sa == StandardAxis::L0, sa == StandardAxis::L0, false, true, {}, {});
    }
}

namespace {
// One imported funscript track awaiting placement during project creation: a display label (the source
// file or tag, shown in the mapping picker), the auto-detected target axis (nullopt when the source tag
// matched no standard axis), and its actions.
struct ImportTrack {
    std::string label;
    std::optional<StandardAxis> role;
    VectorSet<ScriptAxisAction> actions;
};

// Result of scanning a video's sibling funscripts on a worker (directory walk + parse). Each track
// carries its auto-detected standard role, or nullopt when its tag maps to no standard axis; the mapping
// picker (shown on the main thread) defaults a nullopt row to the first free scratch slot.
struct SiblingScan {
    // Metadata of the canonical sibling — the file whose stem equals the video stem — falling back to
    // the first match. Adopted as the project metadata on the main thread.
    std::optional<FunscriptMetadata> primaryMeta;
    std::vector<ImportTrack> tracks; // directory-iteration order, so scratch defaults fill S0, S1, …
};

// Auto-detect the standard axis a funscript tag/suffix names, or nullopt when it matches no standard
// axis — or only a scratch axis, which is left unassigned for the mapping picker to default onto the
// first free scratch slot. Shared by every funscript→ImportTrack path (sibling scan + import append).
std::optional<StandardAxis> autoDetectAxisRole(const std::string &tag) {
    auto matched = standardAxisFromTag(tag);
    return (matched && !isScratchAxis(*matched)) ? matched : std::nullopt;
}

// Worker-thread body for initNewProject's auto-discovery: parse every sibling funscript whose stem
// prefixes the video stem into tracks. Pure file I/O + tag mapping, no ScriptProject access. Uses an
// error_code directory walk so a vanished dir yields an empty scan instead of throwing out of the
// worker (which would strand the suspended flow).
SiblingScan scanSiblingFunscripts(const std::filesystem::path &videoDir, const std::string &videoStem) {
    SiblingScan out;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(videoDir, ec)) {
        if (entry.path().extension() != ".funscript")
            continue;
        std::string fsStem = ofs::util::toUtf8(entry.path().stem());
        if (!fsStem.starts_with(videoStem))
            continue;

        auto fs = Funscript::load(entry.path());
        if (!fs)
            continue;

        if (!out.primaryMeta || fsStem == videoStem)
            out.primaryMeta = fs->metadata;

        if (fs->isMultiAxis()) {
            for (auto &[tag, acts] : fs->toAllAxes()) {
                // Prefix each axis with the source file so the row reads "clip: L0", not a bare "L0".
                out.tracks.push_back({.label = fmt::format("{}: {}", fsStem, tag),
                                      .role = autoDetectAxisRole(tag),
                                      .actions = std::move(acts)});
            }
        } else {
            auto fsActions = fs->toActions();
            std::string suffix = fsStem == videoStem ? "L0" : fsStem.substr(videoStem.size());
            std::string axisName = ofs::util::toUtf8(ofs::util::fromUtf8(suffix).stem());
            if (axisName.starts_with(".") || axisName.starts_with("-") || axisName.starts_with("_"))
                axisName = axisName.substr(1);
            if (axisName.empty())
                axisName = "L0";

            out.tracks.push_back(
                {.label = fsStem, .role = autoDetectAxisRole(axisName), .actions = std::move(fsActions)});
        }
    }
    return out;
}

// The native open-dialog spec for picking a .funscript to import (shared by the import flow and the
// picker's "Add file" button so they remember the same directory).
FileDialogSpec funscriptOpenSpec() {
    return {.kind = FileDialogKind::Open,
            .key = "funscript_import",
            .title = Str::PmDlgImportFunscript.c_str(),
            .filterPatterns = {"*.funscript"},
            .filterDesc = Str::PmFilterFunscript.c_str()};
}

// Fan a loaded funscript out into one importable track per axis. A multi-axis script yields a row per
// tag (each auto-detected onto its standard axis, or left unassigned for the picker to default); a
// single-axis script yields one row whose label is the file name and whose default is `singleAxisDefault`
// (L0 for a lone dropped script, unset → free scratch when importing into an existing project).
void appendTracksFromFunscript(std::vector<ImportTrack> &out, const Funscript &fs, const std::string &fileLabel,
                               std::optional<StandardAxis> singleAxisDefault) {
    if (fs.isMultiAxis()) {
        // Prefix each axis with the source file so a multi-axis import shows e.g. "clip: L0", not a bare
        // "L0" with no hint which file it came from.
        for (auto &[tag, acts] : fs.toAllAxes()) {
            out.push_back({.label = fmt::format("{}: {}", fileLabel, tag),
                           .role = autoDetectAxisRole(tag),
                           .actions = std::move(acts)});
        }
    } else {
        out.push_back({.label = fileLabel, .role = singleAxisDefault, .actions = fs.toActions()});
    }
}

// Restore every confirmed import track onto its mapped axis (dirty + selected, no undo snapshot). Shared
// tail of both new-project import flows; every confirmed track carries a resolved role at this point.
void restoreImportedTracks(ScriptProject &project, std::vector<ImportTrack> &confirmed) {
    for (auto &t : confirmed) {
        if (!t.role)
            continue; // every confirmed track carries a resolved role; guard keeps the deref checked
        project.restoreAxis(*t.role, true, true, false, true, std::move(t.actions), {});
    }
}

// Build the editable import picker. One row per track; each row defaults to its auto-detected axis or —
// when unassigned — the next free hidden scratch slot not already taken by an earlier row, so the picker
// never opens in a duplicate state. `title`/`prompt` differ between the new-project and import-into-an-
// existing-project flows. `rows` carries the full label; ModalManager elides it and shows it on hover.
AxisPickSpec buildImportPickSpec(const std::vector<ImportTrack> &tracks, const std::string &title,
                                 const std::string &prompt, const ScriptProject &project) {
    AxisPickSpec spec;
    spec.title = title;
    spec.prompt = prompt;
    for (size_t i = 0; i < kStandardAxisCount; ++i)
        spec.options.emplace_back(standardAxisName(static_cast<StandardAxis>(i)));

    std::vector<int> used;
    auto taken = [&](int x) { return std::ranges::find(used, x) != used.end(); };
    auto nextFreeScratch = [&]() {
        for (auto i = static_cast<size_t>(StandardAxis::S0); i < kStandardAxisCount; ++i)
            if (!project.axes[i].showInStrip && !taken(static_cast<int>(i)))
                return static_cast<int>(i);
        for (size_t i = 0; i < kStandardAxisCount; ++i)
            if (!taken(static_cast<int>(i)))
                return static_cast<int>(i);
        return static_cast<int>(StandardAxis::S0);
    };
    for (const auto &t : tracks) {
        const int def = t.role ? static_cast<int>(*t.role) : nextFreeScratch();
        used.push_back(def);
        spec.rows.push_back(t.label);
        spec.defaults.push_back(def);
    }
    return spec;
}

// Rebuild the track list from the picker's surviving rows, stamping each with the axis the user chose.
// Run between sessions (each "Add file" re-shows the picker) and at confirm, so prior choices and
// removals carry across. Bounds-checked defensively; a malformed result simply yields fewer tracks.
std::vector<ImportTrack> applyPickSurvivors(std::vector<ImportTrack> &entries, const AxisPickResult &res) {
    std::vector<ImportTrack> survivors;
    survivors.reserve(res.rows.size());
    for (size_t k = 0; k < res.rows.size() && k < res.axis.size(); ++k) {
        const int idx = res.rows[k];
        if (idx < 0 || idx >= static_cast<int>(entries.size()))
            continue;
        ImportTrack t = std::move(entries[idx]);
        t.role = static_cast<StandardAxis>(res.axis[k]);
        survivors.push_back(std::move(t));
    }
    return survivors;
}

// Drive the editable import picker to completion, then hand the confirmed tracks (each with a chosen
// role) to `onConfirm`. The user may remap any row, remove rows, or add more files — each "Add" runs a
// native funscript picker and re-shows the modal with the new tracks appended and prior choices kept.
// Cancelling calls onConfirm with an empty list. Shared by the new-project and import-into-existing
// flows (they pass different title/prompt text). Fire-and-forget: the caller may co_return after launch.
co::Fire runImportPicker(EventQueue &eq, JobSystem &jobSystem, ScriptProject &project, std::vector<ImportTrack> entries,
                         std::string title, std::string prompt,
                         std::function<void(std::vector<ImportTrack>)> onConfirm) {
    while (true) {
        AxisPickResult res = co_await AxisPick{eq, buildImportPickSpec(entries, title, prompt, project)};
        entries = applyPickSurvivors(entries, res);
        if (res.outcome == AxisPickResult::Outcome::Confirm) {
            onConfirm(std::move(entries));
            co_return;
        }
        if (res.outcome == AxisPickResult::Outcome::Cancel) {
            onConfirm({});
            co_return;
        }
        // AddFile: run a native picker, parse off-thread, append the new track(s), then re-show.
        std::string file = co_await FileDialog{eq, funscriptOpenSpec()};
        if (file.empty())
            continue;
        const std::string name = ofs::util::toUtf8(ofs::util::fromUtf8(file).filename());
        const std::string label = ofs::util::toUtf8(ofs::util::fromUtf8(file).stem());
        auto fs = co_await JobAwait<std::optional<Funscript>>{
            jobSystem, eq, [path = ofs::util::fromUtf8(file)] { return Funscript::load(path); }};
        if (!fs) {
            eq.push(NotifyEvent{.level = NotifyLevel::Error, .message = Str::PmImportReadFailed.fmt(name)});
            continue;
        }
        const size_t before = entries.size();
        appendTracksFromFunscript(entries, *fs, label, std::nullopt);
        if (entries.size() == before)
            eq.push(NotifyEvent{.level = NotifyLevel::Error, .message = Str::PmImportNoActions.fmt(name)});
    }
}
} // namespace

co::Fire ProjectManager::initNewProject(std::string mediaPath) {
    project.state.filePath = "";
    project.state.mediaPath = std::move(mediaPath);

    setupDefaultAxes();

    // Auto-discover matching funscripts (only when media was selected)
    if (project.state.mediaPath.empty()) {
        // A media-less project needs a real timeline span (never 0) so there's somewhere to put actions.
        // openProjectVideo() starts the dummy player at this duration.
        project.state.dummyDuration = kDefaultDummyDuration;
        eq.push(AxisSelectedEvent{StandardAxis::L0});
        openProjectVideo();
        co_return; // no co_await reached: this branch completes synchronously on call
    }

    std::filesystem::path videoPath = ofs::util::fromUtf8(project.state.mediaPath);
    std::filesystem::path videoDir = videoPath.parent_path();
    // Match in UTF-8: byte-prefix on UTF-8 is equivalent to code-point prefix.
    std::string videoStem = ofs::util::toUtf8(videoPath.stem());

    // Walk + parse the sibling funscripts off the main thread (a directory scan plus a JSON parse per
    // match). Role mapping is resolved in the worker; only the scratch-slot fallback — which reads
    // project.axes — is applied here on resume.
    auto scan = co_await JobAwait<SiblingScan>{
        jobSystem, eq, [videoDir, videoStem] { return scanSiblingFunscripts(videoDir, videoStem); }};

    if (scan.primaryMeta)
        project.metadata = std::move(*scan.primaryMeta);

    // Finalize the open once placement is settled (confirmed or cancelled): restore the confirmed
    // tracks, seed the no-media fallback length, select L0, adopt any optimized copy, and load the video.
    auto finalize = [this](std::vector<ImportTrack> confirmed) {
        restoreImportedTracks(project, confirmed);
        finalizeNewProjectOpen([this] {
            // The opened file is the original; record it as such and resolve the active source. This adopts
            // an already-existing optimized copy for this content (resolveActiveMedia → discoverExistingIntra)
            // and loads it in place of the original — so a new project on a previously optimized video seeks
            // fast immediately, with no re-encode and no optimize prompt. The sibling scan ran against the
            // original path, so the swap here doesn't affect funscript matching.
            project.state.originalMediaPath = project.state.mediaPath;
            resolveActiveMedia();
        });
    };

    // Always confirm placement: show the editable picker for every discovered funscript so the user can
    // accept, correct, add, or remove placements before the project opens.
    if (scan.tracks.empty())
        finalize({});
    else
        runImportPicker(eq, jobSystem, project, std::move(scan.tracks), Str::PmConfirmImportTitle.c_str(),
                        Str::PmConfirmImportPrompt.c_str(), std::move(finalize));
}

co::Fire ProjectManager::initNewProjectFromFunscript(std::filesystem::path funscriptPath) {
    // A funscript is a script, not media. Prefer the normal flow: if a sibling media file shares the
    // funscript's stem (optionally minus an axis suffix like ".roll"), load that media — its
    // auto-discovery then maps this funscript (and any siblings) onto their axes for free. This scan
    // only inspects names (no parsing), so it stays on the main thread.
    const std::string fsStem = ofs::util::toUtf8(funscriptPath.stem());
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(funscriptPath.parent_path(), ec)) {
        if (!isMediaExtension(entry.path()))
            continue;
        if (fsStem.starts_with(ofs::util::toUtf8(entry.path().stem()))) {
            initNewProject(ofs::util::toUtf8(entry.path())); // fire-and-forget: its own flow finishes the open
            co_return;
        }
    }

    // No matching media: empty project with the funscript's axes confirmed through the editable picker. A
    // single-axis script defaults to L0 (the default visible axis); a multi-axis one fans out by tag.
    project.state.filePath = "";
    project.state.mediaPath = "";
    setupDefaultAxes();

    // Parse the funscript on a worker; apply on resume.
    auto fs = co_await JobAwait<std::optional<Funscript>>{jobSystem, eq,
                                                          [funscriptPath] { return Funscript::load(funscriptPath); }};

    // Finalize once placement is settled: restore the confirmed tracks, then open the media-less project
    // on the dummy player (length never 0, long enough to show the imported actions).
    auto finalize = [this](std::vector<ImportTrack> confirmed) {
        restoreImportedTracks(project, confirmed);
        finalizeNewProjectOpen(nullptr);
    };

    std::vector<ImportTrack> tracks;
    if (fs) {
        project.metadata = std::move(fs->metadata);
        appendTracksFromFunscript(tracks, *fs, fsStem, StandardAxis::L0);
    }
    if (tracks.empty())
        finalize({});
    else
        runImportPicker(eq, jobSystem, project, std::move(tracks), Str::PmConfirmImportTitle.c_str(),
                        Str::PmConfirmImportPrompt.c_str(), std::move(finalize));
}

double ProjectManager::mediaLessDuration() const {
    double lastActionTime = 0.0;
    for (const auto &axis : project.axes)
        if (!axis.actions.empty())
            lastActionTime = std::max(lastActionTime, axis.actions.back().at);
    return std::max(kDefaultDummyDuration, lastActionTime + 1.0);
}

void ProjectManager::finalizeNewProjectOpen(const std::function<void()> &mediaSetup) {
    // dummyDuration is the canvas to use whenever there's no media, so it must always be valid even with a
    // video loaded — unloading later then simply reuses it. Seeded after the tracks are restored so the
    // longest imported axis bounds it.
    project.state.dummyDuration = mediaLessDuration();
    eq.push(AxisSelectedEvent{StandardAxis::L0});
    if (mediaSetup)
        mediaSetup();
    openProjectVideo();
}

namespace {
// Default save name for a never-saved project: the loaded media's stem (e.g. "MyVideo.mp4" → "MyVideo.ofp"),
// so the project lands next to its video with a matching name. Falls back to "Untitled.ofp" for a media-less
// project. A previously-saved project doesn't reach here — the picker reopens on its own filePath.
std::string projectDefaultName(const std::string &mediaPath) {
    if (mediaPath.empty())
        return "Untitled.ofp";
    std::string stem = ofs::util::toUtf8(ofs::util::fromUtf8(mediaPath).stem());
    return stem.empty() ? "Untitled.ofp" : stem + ".ofp";
}

// Save spec for the project's .ofp picker (shared by guardUnsaved and saveFlow).
FileDialogSpec projectSaveSpec(const std::string &mediaPath) {
    return {.kind = FileDialogKind::Save,
            .key = "project",
            .title = Str::PmDlgSaveProject.c_str(),
            .defaultName = projectDefaultName(mediaPath),
            .filterPatterns = {"*.ofp"},
            .filterDesc = Str::PmFilterProject.c_str()};
}
} // namespace

bool ProjectManager::saveProjectToPath(const std::string &file) {
    std::filesystem::path path = ofs::util::fromUtf8(file);
    if (path.extension() != ".ofp")
        path += ofs::util::fromUtf8(".ofp");
    return saveProjectInternal(path);
}

co::Fire ProjectManager::guardUnsaved(std::function<void()> proceed) {
    if (isDirty()) {
        const int choice = co_await Confirm{eq, unsavedChangesSpec()};
        if (choice == 2) // Cancel
            co_return;
        if (choice == 0) { // Save
            bool saved = false;
            if (project.state.filePath.empty()) {
                std::string file = co_await FileDialog{eq, projectSaveSpec(project.state.mediaPath)};
                saved = !file.empty() && saveProjectToPath(file);
            } else {
                saved = saveProjectInternal(ofs::util::fromUtf8(project.state.filePath));
            }
            if (!saved) // save cancelled or failed => abort, do not proceed
                co_return;
        }
        // choice == 1 (Don't Save): fall through and discard
    }
    proceed();
}

co::Fire ProjectManager::saveFlow(bool saveAs) {
    if (!hasActiveProject())
        co_return;
    if (saveAs || project.state.filePath.empty()) {
        std::string file = co_await FileDialog{eq, projectSaveSpec(project.state.mediaPath)};
        if (!file.empty())
            saveProjectToPath(file);
    } else {
        saveProjectInternal(ofs::util::fromUtf8(project.state.filePath));
    }
}

co::Fire ProjectManager::importFunscript() {
    if (!hasActiveProject())
        co_return;
    std::string file = co_await FileDialog{eq, funscriptOpenSpec()};
    if (file.empty())
        co_return;

    const std::string name = ofs::util::toUtf8(ofs::util::fromUtf8(file).filename());
    const std::string label = ofs::util::toUtf8(ofs::util::fromUtf8(file).stem());
    auto fs = co_await JobAwait<std::optional<Funscript>>{
        jobSystem, eq, [path = ofs::util::fromUtf8(file)] { return Funscript::load(path); }};
    if (!fs) {
        eq.push(NotifyEvent{.level = NotifyLevel::Error, .message = Str::PmImportReadFailed.fmt(name)});
        co_return;
    }

    // Fan the file out into one editable row per axis (a multi-axis script becomes one row per tag). No
    // implicit default to a device axis: importing into an existing project, an unmatched track defaults
    // to a free scratch slot so it never clobbers L0 — the user confirms or retargets in the picker.
    std::vector<ImportTrack> entries;
    appendTracksFromFunscript(entries, *fs, label, std::nullopt);
    if (entries.empty()) {
        eq.push(NotifyEvent{.level = NotifyLevel::Error, .message = Str::PmImportNoActions.fmt(name)});
        co_return;
    }

    // The picker lets the user remap / remove / add tracks; confirmed tracks apply as one undo step.
    // Cancelling applies nothing.
    runImportPicker(eq, jobSystem, project, std::move(entries), Str::PmImportPickTitle.c_str(),
                    Str::PmImportPickPrompt.c_str(), [this](std::vector<ImportTrack> confirmed) {
                        if (confirmed.empty())
                            return;
                        ImportFunscriptDataEvent data;
                        // A target axis that already holds actions would be silently replaced. Collect those
                        // so the user must confirm the overwrite (undoable, but never without warning).
                        std::string occupied;
                        for (auto &t : confirmed) {
                            if (t.role && !project.axes[static_cast<size_t>(*t.role)].actions.empty())
                                occupied = occupied.empty()
                                               ? std::string(standardAxisName(*t.role))
                                               : fmt::format("{}, {}", occupied, standardAxisName(*t.role));
                            data.axes.push_back({.role = t.role, .actions = std::move(t.actions)});
                        }
                        if (occupied.empty()) {
                            eq.push(std::move(data));
                            return;
                        }
                        confirmAsync(eq,
                                     ModalSpec{.title = Str::PmImportOverwriteTitle.c_str(),
                                               .message = Str::PmImportOverwriteBody.fmt(occupied),
                                               .buttons = {Str::ModalImport.c_str(), Str::ModalCancel.c_str()},
                                               .severity = ModalSeverity::Warning},
                                     [this, data = std::move(data)](int idx) mutable {
                                         if (idx == 0)
                                             eq.push(std::move(data));
                                     });
                    });
}

void ProjectManager::onImportFunscriptData(const ImportFunscriptDataEvent &event) {
    int placed = 0;
    int dropped = 0;
    for (const auto &axis : event.axes) {
        if (axis.role) {
            project.mutate(
                *axis.role,
                [&](AxisState &a) {
                    a.actions = axis.actions;
                    a.isVisible = true;
                    a.showInStrip = true;
                },
                eq);
            ++placed;
            continue;
        }
        // nullopt → first hidden scratch slot, revealed to hold this track; a multi-axis import fills
        // S0, S1, … in order and falls through (dropped) once all ten are shown.
        bool slotFound = false;
        for (auto i = static_cast<size_t>(StandardAxis::S0); i < kStandardAxisCount; ++i) {
            if (!project.axes[i].showInStrip) {
                const auto sa = static_cast<StandardAxis>(i);
                project.restoreAxis(sa, true, true, false, true, axis.actions, {});
                eq.push(AxisModifiedEvent{sa});
                ++placed;
                slotFound = true;
                break;
            }
        }
        if (!slotFound)
            ++dropped;
    }

    if (dropped > 0)
        eq.push(NotifyEvent{.level = NotifyLevel::Error, .message = Str::PmImportDropped.fmt(dropped)});
    else if (placed > 0)
        eq.push(NotifyEvent{.level = NotifyLevel::Success, .message = Str::PmImported.fmt(placed)});
}

co::Fire ProjectManager::exportMultipleFunscript10(std::vector<StandardAxis> axes,
                                                   std::optional<std::string> targetPath) {
    if (axes.empty())
        co_return;
    std::string dir;
    if (targetPath) {
        dir = *targetPath;
    } else {
        dir = co_await FileDialog{eq,
                                  {.kind = FileDialogKind::SelectFolder,
                                   .key = "funscript_export_folder",
                                   .title = Str::PmDlgExportFolder.c_str()}};
    }
    if (dir.empty())
        co_return;

    std::filesystem::path outDir = ofs::util::fromUtf8(dir);
    std::string stem = "script";
    if (!project.state.mediaPath.empty())
        stem = ofs::util::toUtf8(ofs::util::fromUtf8(project.state.mediaPath).stem());

    // Capture each axis's actions + output path on the main thread (reads project.axes), then
    // serialize + write the files on a worker.
    struct WriteJob {
        std::filesystem::path outPath;
        VectorSet<ScriptAxisAction> actions;
    };
    std::vector<WriteJob> writeJobs;
    for (const auto role : axes) {
        const auto roleIdx = static_cast<size_t>(role);
        const auto &axis = project.axes[roleIdx];
        if (isScratchAxis(role))
            continue;
        const auto &actions = axis.resolved ? axis.resolved->actions : axis.actions;
        if (actions.empty())
            continue;
        std::string tag(standardAxisTag(role));
        writeJobs.push_back(
            {.outPath = outDir / ofs::util::fromUtf8(fmt::format("{}.{}.funscript", stem, tag)), .actions = actions});
    }
    FunscriptMetadata exportMeta = project.metadata; // copy for the worker

    struct Counts {
        int written = 0;
        int failed = 0;
    };
    auto counts = co_await JobAwait<Counts>{
        jobSystem, eq, [jobs = std::move(writeJobs), exportMeta = std::move(exportMeta), outDir]() -> Counts {
            // Re-export may target a folder the user has since removed; recreate it so the replay lands.
            std::error_code ec;
            std::filesystem::create_directories(outDir, ec);
            Counts c;
            for (const auto &j : jobs) {
                Funscript fs = Funscript::fromActions(j.actions);
                fs.metadata = exportMeta;
                if (fs.save(j.outPath))
                    ++c.written;
                else
                    ++c.failed;
            }
            return c;
        }};

    if (counts.failed > 0)
        eq.push(NotifyEvent{.level = NotifyLevel::Error,
                            .message = Str::PmExportPartial.fmt(counts.written, counts.failed)});
    else
        eq.push(NotifyEvent{.level = NotifyLevel::Success, .message = Str::PmExportDone.fmt(counts.written)});
    recordLastExport(0, std::move(axes), dir);
}

co::Fire ProjectManager::exportMultiAxisFunscript(std::vector<StandardAxis> axes, bool useChannels,
                                                  std::optional<std::string> targetPath) {
    if (axes.empty())
        co_return;

    std::vector<std::pair<std::string, VectorSet<ScriptAxisAction>>> axisData;
    for (const auto role : axes) {
        const auto roleIdx = static_cast<size_t>(role);
        const auto &axis = project.axes[roleIdx];
        if (isScratchAxis(role))
            continue;
        const auto &actions = axis.resolved ? axis.resolved->actions : axis.actions;
        if (actions.empty())
            continue;
        axisData.emplace_back(std::string(standardAxisTag(role)), actions);
    }
    if (axisData.empty())
        co_return;

    std::string file;
    if (targetPath) {
        file = *targetPath;
    } else {
        std::string defaultName = "script";
        if (!project.state.mediaPath.empty())
            defaultName = ofs::util::toUtf8(ofs::util::fromUtf8(project.state.mediaPath).stem());
        defaultName += useChannels ? ".max.funscript" : ".funscript";

        file = co_await FileDialog{eq,
                                   {.kind = FileDialogKind::Save,
                                    .key = "funscript_export",
                                    .title = useChannels ? Str::PmDlgExport20.c_str() : Str::PmDlgExport11.c_str(),
                                    .defaultName = std::move(defaultName),
                                    .filterPatterns = {"*.funscript"},
                                    .filterDesc = Str::PmFilterFunscript.c_str()}};
    }
    if (file.empty())
        co_return;

    std::filesystem::path outPath = ofs::util::fromUtf8(file);
    const std::string name = ofs::util::toUtf8(outPath.filename());
    FunscriptMetadata meta = project.metadata; // copy for the worker

    // Build the combined funscript + write it on a worker.
    bool ok = co_await JobAwait<bool>{
        jobSystem, eq, [axisData = std::move(axisData), useChannels, meta = std::move(meta), outPath]() -> bool {
            // Re-export may target a directory the user has since removed; recreate it so the replay lands.
            if (outPath.has_parent_path()) {
                std::error_code ec;
                std::filesystem::create_directories(outPath.parent_path(), ec);
            }
            Funscript fs = useChannels ? Funscript::fromAxes20(axisData) : Funscript::fromAxes11(axisData);
            fs.metadata = meta;
            return fs.save(outPath);
        }};

    if (ok)
        eq.push(NotifyEvent{.level = NotifyLevel::Success, .message = Str::PmExportedOne.fmt(name)});
    else
        eq.push(NotifyEvent{.level = NotifyLevel::Error, .message = Str::PmExportFailedOne.fmt(name)});
    recordLastExport(useChannels ? 2 : 1, std::move(axes), file);
}

bool ProjectManager::isDirty() const {
    if (project.state.settingsDirty)
        return true;
    return std::ranges::any_of(project.axes, [](const auto &axis) { return axis.dirty; });
}

void ProjectManager::setDirty(bool dirty) {
    // No active project => nothing to mark dirty. Guarding the one funnel every dirty-marking handler
    // routes through (overlay/simulator/video-mode/timeline all fire while "No Project" is shown) stops
    // the stale flag at its source, so isDirty() can't raise a spurious save prompt.
    if (dirty && !hasActiveProject())
        return;
    if (dirty) {
        project.state.settingsDirty = true;
        // Non-axis edits funnel through here; axis edits bump editRevision in mutate(). Either way the
        // counter advances past backupRevision, arming the next auto-backup tick.
        ++project.editRevision;
    } else {
        project.clearDirtyFlags();
        // A clean project (just saved) matches a file on disk, so there is nothing an auto-backup would
        // preserve until the next edit advances editRevision again.
        backupRevision = project.editRevision;
    }
}

bool ProjectManager::hasActiveProject() const {
    if (!project.state.mediaPath.empty() || !project.state.filePath.empty())
        return true;
    // An open project always shows at least L0 in the panel (setupDefaultAxes / load); a closed one is
    // reset to all-hidden. No axis-presence flag to test — axes always exist.
    return std::ranges::any_of(project.axes, [](const AxisState &a) { return a.showInStrip; });
}

std::filesystem::path ProjectManager::getCurrentProjectPath() const {
    return project.state.filePath;
}

const char *ProjectManager::getProjectTitle() const {
    if (!hasActiveProject())
        return Str::AppNoProject;
    if (project.state.filePath.empty())
        return Str::AppUntitledProject;
    std::string_view pathView(project.state.filePath);
    auto slashPos = pathView.find_last_of("/\\");
    std::string_view filename = (slashPos != std::string_view::npos) ? pathView.substr(slashPos + 1) : pathView;
    return fmtScratch("{}", filename);
}

void ProjectManager::applyLoadedProject(const Project &loaded, const std::filesystem::path &filePath, bool markDirty) {
    clearProject();
    loadFromProject(loaded);
    // Baseline the backup counter against the just-loaded state so a plain open is never re-backed-up.
    // A restore then calls setDirty(true) below, advancing editRevision past this so its recovered (and
    // as-yet-unsaved) content is captured on the next tick.
    backupRevision = project.editRevision;
    project.state.filePath = filePath.empty() ? std::string{} : ofs::util::toUtf8(filePath);
    // This open is a new editing session. Bump the persisted count and snapshot the current action
    // total as the baseline for the Info tab's "this session" net-edits delta.
    project.state.editSessionCount += 1;
    int baseline = 0;
    for (const auto &ax : project.axes)
        baseline += static_cast<int>(ax.resolved ? ax.resolved->actions.size() : ax.actions.size());
    project.state.sessionBaselineActions = baseline;
    if (!project.state.filePath.empty())
        eq.push(RememberRecentProjectEvent{project.state.filePath});

    // Select the saved active axis, or fall back to L0 if it's out of range.
    StandardAxis toSelect = loaded.activeAxisRole;
    if (toSelect >= StandardAxis::Count)
        toSelect = StandardAxis::L0;
    eq.push(AxisSelectedEvent{toSelect});

    // Arm the saved resume position before the (async) media load. onDurationChanged consumes it once
    // the opened media — real video or the media-less dummy — reports its true length. Only a positive
    // position needs restoring; 0 is the natural default a fresh open already lands on.
    if (loaded.playbackPosition > 0.0)
        pendingResumeSeek = loaded.playbackPosition;

    // A restore loads content that differs from the file on disk, so it must come up dirty (and armed
    // for the next auto-backup). A plain open is clean. Set this before the media load; resolveMediaPath
    // may start an async relocate that should not be mistaken for a clean state.
    if (markDirty)
        setDirty(true);

    // If media is missing, resolveMediaPath starts an async relocate prompt that loads the video
    // itself; only drive the load here when it resolved synchronously.
    if (resolveMediaPath())
        openProjectVideo();
}

co::Fire ProjectManager::loadProjectInternal(std::filesystem::path path) {
    // Read + CBOR-decode the .ofp on a worker; the caller already ran doClose(), so the project sits
    // empty until this resumes. Project::load catches its own errors and returns nullopt.
    auto loaded = co_await JobAwait<std::optional<Project>>{jobSystem, eq, [path] { return Project::load(path); }};
    if (!loaded) {
        showError(eq, Str::PmErrorTitle.c_str(), Str::PmErrLoadFailed.c_str());
        co_return;
    }
    applyLoadedProject(*loaded, path, /*markDirty=*/false);
}

co::Fire ProjectManager::restoreBackupInternal(std::filesystem::path backupPath, std::filesystem::path target) {
    auto loaded =
        co_await JobAwait<std::optional<Project>>{jobSystem, eq, [backupPath] { return Project::load(backupPath); }};
    if (!loaded) {
        showError(eq, Str::PmErrorTitle.c_str(), Str::PmErrLoadFailed.c_str());
        co_return;
    }
    // Keep the file association on the real project, not the dated backup, and leave it dirty so a Save
    // commits the recovered state over the original file.
    applyLoadedProject(*loaded, target, /*markDirty=*/true);
    eq.push(NotifyEvent{.level = NotifyLevel::Success, .message = Str::BackupRestoreDone.c_str()});
}

bool ProjectManager::saveProjectInternal(const std::filesystem::path &path) {
    if (!hasActiveProject())
        return false;
    if (pendingWrite.has_value()) {
        finalizePendingWrite(pendingWrite->future.get());
        pendingWrite.reset();
    }

    // Stamp provenance before the snapshot: createdAt once (first ever save), modifiedAt every save.
    // A backup write (update()) does not pass through here, so it never bumps the user-visible dates.
    const auto nowUnix =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (project.state.createdAtUnix == 0)
        project.state.createdAtUnix = nowUnix;
    project.state.modifiedAtUnix = nowUnix;

    auto captureStart = std::chrono::steady_clock::now();
    Project p;
    saveToProject(p);
    auto captureMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - captureStart).count();

    scheduleWrite(std::move(p), path, true, captureMs);
    return true;
}

void ProjectManager::scheduleWrite(Project p, const std::filesystem::path &path, bool isUserSave, long long captureMs) {
    pendingWrite =
        PendingWrite{.future = jobSystem.submitTask([p = std::move(p), path, captureMs, isUserSave]() mutable -> bool {
                         auto writeStart = std::chrono::steady_clock::now();
                         bool ok = p.save(path);
                         auto writeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - writeStart)
                                            .count();
                         OFS_CORE_INFO("[{}] {} — capture {}ms, write {}ms", isUserSave ? "Save" : "Backup",
                                       ofs::util::toUtf8(path.filename()), captureMs, writeMs);
                         return ok;
                     }),
                     .path = path,
                     .isUserSave = isUserSave};
}

void ProjectManager::finalizePendingWrite(bool ok) {
    if (!pendingWrite.has_value())
        return;
    if (pendingWrite->isUserSave) {
        if (ok) {
            project.state.filePath = ofs::util::toUtf8(pendingWrite->path);
            eq.push(RememberRecentProjectEvent{project.state.filePath});
            setDirty(false);
            lastSaveTime = std::chrono::steady_clock::now();
            eq.push(NotifyEvent{.level = NotifyLevel::Success, .message = Str::PmProjectSaved.c_str()});
        } else {
            showError(eq, Str::PmErrorTitle.c_str(), Str::PmErrSaveFailed.c_str());
        }
    }
    pendingWrite.reset();
}

void ProjectManager::update(float dt) {
    if (pendingWrite.has_value() &&
        pendingWrite->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        finalizePendingWrite(pendingWrite->future.get());
    }

    resolveActiveSceneView();

    if (!appSettings.autoBackupEnabled || !hasActiveProject())
        return;

    // Hold the backup timers while a save/backup write is still in flight. Advancing them here
    // would reset any tick that fires during the write and silently drop that backup, delaying it
    // a full interval. Pausing instead retries the moment the single write slot frees up, and the
    // skipped dt (a write is tens of ms) is negligible. This also keeps saves and backups serialized
    // through the one `pendingWrite` slot, so neither ever clobbers the other.
    if (pendingWrite.has_value())
        return;

    backupElapsed += dt;
    if (backupElapsed < kBackupIntervalSeconds)
        return;
    backupElapsed = 0.0;

    // Dirty-gate: nothing changed since the last backup, so a new snapshot would only be a duplicate that
    // evicts an older, genuinely distinct one. Skip it — the rolling window keeps real edit history, not
    // copies of an idle project.
    if (project.editRevision == backupRevision)
        return;

    auto backupDir = ofs::backup::dirForProject(ofs::util::fromUtf8(project.state.filePath));
    std::filesystem::create_directories(backupDir);
    auto backupPath = backupDir / ofs::util::fromUtf8(ofs::backup::fileName(std::time(nullptr)));

    auto captureStart = std::chrono::steady_clock::now();
    Project p;
    saveToProject(p);
    auto captureMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - captureStart).count();
    backupRevision = project.editRevision;
    int keepCount = appSettings.backupKeepCount;

    pendingWrite = PendingWrite{.future = jobSystem.submitTask(
                                    [p = std::move(p), backupDir, backupPath, keepCount, captureMs]() mutable -> bool {
                                        auto writeStart = std::chrono::steady_clock::now();
                                        bool ok = p.save(backupPath);
                                        auto writeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                           std::chrono::steady_clock::now() - writeStart)
                                                           .count();
                                        OFS_CORE_INFO("[Backup] {} — capture {}ms, write {}ms",
                                                      ofs::util::toUtf8(backupPath.filename()), captureMs, writeMs);
                                        if (ok)
                                            ofs::backup::prune(backupDir, keepCount);
                                        return ok;
                                    }),
                                .path = backupPath,
                                .isUserSave = false};
}

bool ProjectManager::resolveMediaPath() {
    if (project.state.mediaPath.empty())
        return true;
    std::filesystem::path mediaPath = ofs::util::fromUtf8(project.state.mediaPath);
    if (std::filesystem::exists(mediaPath))
        return true;

    // Missing media: prompt asynchronously. relocateFlow loads the video when it finishes, so the
    // caller must NOT call openProjectVideo() itself.
    relocateFlow();
    return false;
}

co::Fire ProjectManager::relocateFlow() {
    // project.state.mediaPath is already UTF-8.
    const std::string msg = Str::PmMediaNotFoundBody.fmt(project.state.mediaPath);
    if (co_await Confirm{eq,
                         {.title = Str::PmMediaNotFoundTitle.c_str(),
                          .message = msg,
                          .buttons = {Str::PmBtnRelocate.c_str(), Str::PmBtnSkip.c_str()},
                          .severity = ModalSeverity::Warning}} == 0) {
        std::string file = co_await FileDialog{eq,
                                               {.kind = FileDialogKind::Open,
                                                .key = "video",
                                                .title = Str::PmDlgRelocateMedia.c_str(),
                                                .filterPatterns = mediaFilterPatterns(),
                                                .filterDesc = Str::PmFilterMedia.c_str()}};
        if (!file.empty()) {
            // Relocating points the active source at the moved file. The intra copy (if any) was made
            // from this same content, so it stays associated — only the original's location changed.
            project.state.mediaPath = file;
            if (project.activeSource == MediaSource::Intra)
                project.state.intraMediaPath = std::move(file);
            else
                project.state.originalMediaPath = std::move(file);
        }
    }
    openProjectVideo();
}

void ProjectManager::openProjectVideo() {
    if (!project.state.mediaPath.empty())
        eq.push(LoadVideoEvent{project.state.mediaPath});
    else if (project.state.dummyDuration > 0.0)
        eq.push(ChangeDummyDurationEvent{project.state.dummyDuration});
}

void ProjectManager::resolveActiveMedia() {
    discoverExistingIntra(); // adopt a pre-existing optimized copy before choosing the source
    const std::string &intra = project.state.intraMediaPath;
    const bool useIntra = !intra.empty() && std::filesystem::exists(ofs::util::fromUtf8(intra));
    project.activeSource = useIntra ? MediaSource::Intra : MediaSource::Original;
    project.state.mediaPath = useIntra ? intra : project.state.originalMediaPath;
}

void ProjectManager::discoverExistingIntra() {
    if (!project.state.intraMediaPath.empty())
        return; // a copy is already recorded (loaded from the project or just transcoded)
    if (project.state.originalMediaPath.empty() || appSettings.intraOutputDir.empty())
        return;
    std::error_code ec;
    if (!std::filesystem::is_directory(ofs::util::fromUtf8(appSettings.intraOutputDir), ec))
        return; // output dir gone/unmounted — nothing to find
    const auto out = ofs::util::intraOutputPath(ofs::util::fromUtf8(appSettings.intraOutputDir),
                                                ofs::util::fromUtf8(project.state.originalMediaPath));
    if (out.empty() || !std::filesystem::exists(out, ec))
        return;
    project.state.intraMediaPath = ofs::util::toUtf8(out);
}

void ProjectManager::onDurationChanged(const DurationChangedEvent &event) {
    if (!pendingResumeSeek)
        return;
    // openVideo() emits a duration of 0 to clear the previous file before the real length arrives;
    // wait for that real (positive) duration so the seek target isn't clamped away to 0.
    if (event.duration <= 0.0)
        return;
    eq.push(SeekEvent{std::clamp(*pendingResumeSeek, 0.0, event.duration)});
    pendingResumeSeek.reset();
}

void ProjectManager::clearProject() {
    // A user save scheduled just before this close (guardUnsaved → Save → close) is still in flight:
    // the save returns the moment the write is queued, then proceed() closes the project. Finalize it
    // now so its async completion stamps filePath onto *this* outgoing project — which resetDocument
    // wipes immediately below — instead of resurrecting filePath on the freshly reset document a frame
    // later, which would flip hasActiveProject() back to true and bounce the UI off the welcome screen.
    if (pendingWrite.has_value())
        finalizePendingWrite(pendingWrite->future.get());

    // Reset the whole project document to defaults so no setting (VR/video mode, overlay, metadata,
    // simulator placement, …) can bleed from the closed project into the next one — including a fresh
    // media-less project that never goes through loadFromProject.
    project.resetDocument();
    // Pick a fresh per-project offset for the auto-naming/coloring sequences so a new project doesn't
    // open on the same first region name / chapter color as the last. A load overwrites this from the
    // file right after (loadFromProject), keeping reopened projects reproducible.
    std::random_device rd;
    project.state.autoNameSeed = static_cast<int>(rd() & 0x7fffffffu);
    // Count the fresh project as session 1. A subsequent loadFromProject overwrites this from the file,
    // and loadProjectInternal then bumps it for the open — so only a brand-new project keeps this value.
    project.state.editSessionCount = 1;
    lastSceneViewIdx = -2;
    // Drop any resume seek still waiting on a duration from a prior load (e.g. one whose media the user
    // declined to relocate, so it never arrived) — it must not fire against the next project.
    pendingResumeSeek.reset();
    backupElapsed = 0.0;
    project.editRevision = 0;
    backupRevision = 0;
    lastSaveTime.reset();
    eq.push(LoadProjectEvent{});
}

void ProjectManager::onChangeDummyDuration(const ChangeDummyDurationEvent &event) {
    if (!hasActiveProject())
        return;
    // A zero/negative span has no editable timeline; reject it so no path (UI, plugin, drop) can ever
    // leave a media-less project with an unusable duration. The config window validates too — this is
    // the model-side backstop.
    if (event.durationSeconds <= 0.0)
        return;
    project.state.dummyDuration = event.durationSeconds;
    project.state.settingsDirty = true;
}

void ProjectManager::onChangeMediaPath(const ChangeMediaPathEvent &event) {
    if (!hasActiveProject())
        return;
    // A switch between the two already-remembered sources (original ↔ intra) shares one timeline, so it
    // must keep the playback position. A genuinely new/relocated file (matches neither) resets to 0.
    // Captured before the model is mutated below, since that overwrites originalMediaPath.
    const bool sourceToggle = !event.path.empty() && (event.path == project.state.intraMediaPath ||
                                                      event.path == project.state.originalMediaPath);
    if (sourceToggle && project.playback.cursorPos > 0.0)
        pendingResumeSeek = project.playback.cursorPos;
    project.state.mediaPath = event.path;
    // Keep the two-path model in sync. Loading the known intra copy is a source toggle to Intra;
    // anything else is a new/relocated original, which invalidates a previously generated intra copy
    // (it was made from a different source). resolveActiveMedia() is not used here: the event names the
    // exact path to load, so we honor it directly rather than re-resolving.
    if (!event.path.empty() && event.path == project.state.intraMediaPath) {
        project.activeSource = MediaSource::Intra;
    } else {
        if (event.path != project.state.originalMediaPath) {
            project.state.intraMediaPath.clear();
            project.state.intraOptimizeDeclined = false; // a different video — offer optimizing it afresh
        }
        project.state.originalMediaPath = event.path;
        project.activeSource = MediaSource::Original;
    }
    project.state.settingsDirty = true;
    if (!event.path.empty()) {
        // Keep dummyDuration intact: it's the no-media canvas length the user chose, not something the
        // video owns. Zeroing it here was the bug — unloading later would then have nothing to restore
        // and fall back to a default, silently discarding the user's setting.
        eq.push(LoadVideoEvent{event.path});
    } else {
        // Unloading just reactivates the dummy player at the preserved length (always valid; seed only
        // for legacy projects that stored 0). CloseVideoEvent drains before ChangeDummyDurationEvent, so
        // the player ends up on the dummy with this span.
        eq.push(CloseVideoEvent{});
        if (project.state.dummyDuration <= 0.0)
            project.state.dummyDuration = mediaLessDuration();
        eq.push(ChangeDummyDurationEvent{project.state.dummyDuration});
    }
}

void ProjectManager::onDeclineOptimize() {
    if (!hasActiveProject() || project.state.intraOptimizeDeclined)
        return;
    project.state.intraOptimizeDeclined = true; // persisted; suppresses the prompt for this original
    project.state.settingsDirty = true;
}

void ProjectManager::loadFromProject(const Project &proj) {
    // Restore the original source, then resolve which one feeds the player (prefer an existing intra
    // copy) — this also sets state.mediaPath, so openProjectVideo() loads the resolved path.
    project.state.originalMediaPath = proj.originalMediaPath;
    project.state.intraOptimizeDeclined = proj.intraOptimizeDeclined;
    resolveActiveMedia();
    project.state.dummyDuration = proj.dummyDuration;
    project.state.totalEditingSeconds = proj.totalEditingSeconds;
    project.state.createdAtUnix = proj.createdAtUnix;
    project.state.modifiedAtUnix = proj.modifiedAtUnix;
    project.state.editSessionCount = proj.editSessionCount;
    project.state.autoNameSeed = proj.autoNameSeed;
    project.state.lastExport = proj.lastExport;
    // The authored ids are applied verbatim to the stored fields; the effective fields start equal and
    // the routers validate them against their registries on the LoadProjectEvent, falling the *effective*
    // id back to follow-overlay / native if no loaded plugin registers it while leaving the stored id
    // intact (so a re-save preserves the authored selection).
    project.storedNavigator = proj.activeNavigator;
    project.storedEditMode = proj.activeEditMode;
    project.storedSelectionMode = proj.activeSelectionMode;
    project.activeNavigator = proj.activeNavigator;
    project.activeEditMode = proj.activeEditMode;
    project.activeSelectionMode = proj.activeSelectionMode;
    project.timelineView.showAudioWaveform = proj.showAudioWaveform;
    project.pluginData = proj.pluginData;
    project.metadata = proj.metadata;
    project.overlay = proj.overlaySettings;
    project.videoPlayer = proj.videoPlayerState;
    project.bookmarks = proj.bookmarkChapters;
    project.simulator.p1 = proj.simP1;
    project.simulator.p2 = proj.simP2;
    project.simulator.sim3dPos = proj.sim3dPos;
    project.simulator.sim3dSize = proj.sim3dSize;
    project.defaultSceneView = proj.defaultSceneView;
    // Seed a sane active view; the first update() resolves the cursor's chapter (sentinel -2).
    project.activeSceneView = proj.defaultSceneView;
    lastSceneViewIdx = -2;

    // Load regions
    project.regions = proj.processingRegions;
    for (auto &region : project.regions) {
        region.axisRoles.reset();
        for (const auto &tag : region.axisRoleTags) {
            if (auto role = standardAxisFromTag(tag)) {
                region.axisRoles.set(static_cast<size_t>(*role));
            }
        }

        // Boundary: a foreign/corrupt file may carry node graphs the editor would never produce
        // (short param arrays, dangling links, cycles). Normalize + validate here so the processing
        // system only ever evaluates a trustworthy graph; a structurally invalid one is replaced.
        std::string err;
        if (!sanitizeProjectGraph(region.nodeGraph, effectReg, err)) {
            OFS_CORE_WARN("Region '{}' has an invalid node graph ({}); resetting it to default.", region.name, err);
            region.nodeGraph = buildDefaultGraph(region.axisRoles);
        }
    }
    project.sortRegions();

    // Load axes
    constexpr auto kDeviceAxisCount = static_cast<size_t>(StandardAxis::S0);
    std::array<bool, kDeviceAxisCount> standardPresent{};

    for (const auto &sa : proj.scriptAxes) {
        const auto roleIdx = static_cast<size_t>(sa.role);
        project.restoreAxis(sa.role, sa.role == StandardAxis::L0 ? true : sa.isVisible,
                            isScratchAxis(sa.role) ? true : sa.showInStrip, sa.isLocked, false, sa.actions, {});
        if (!isScratchAxis(sa.role) && roleIdx < kDeviceAxisCount)
            standardPresent[roleIdx] = true;
    }

    // Ensure all device axes exist (L0–A1), even if not in the file
    for (size_t i = 0; i < kDeviceAxisCount; ++i) {
        if (!standardPresent[i]) {
            const auto sa = static_cast<StandardAxis>(i);
            project.restoreAxis(sa, sa == StandardAxis::L0, sa == StandardAxis::L0, false, false, {}, {});
        }
    }

    // Uphold the invariant for older files: a project saved with media stored dummyDuration = 0, which
    // would leave nothing valid to fall back to on a later video unload. Seed a real length now (axes are
    // loaded, so it can cover existing actions).
    if (project.state.dummyDuration <= 0.0)
        project.state.dummyDuration = mediaLessDuration();
}

void ProjectManager::saveToProject(Project &proj) const {
    proj.mediaPath = project.state.mediaPath;
    // originalMediaPath is the source of truth for "the original" and feeds resolveActiveMedia() on the
    // next load — it must never be emptier than the resolved mediaPath, or a project whose mediaPath was
    // set without the original (a fixture, or legacy in-memory state) would reload with no media at all.
    proj.originalMediaPath =
        !project.state.originalMediaPath.empty() ? project.state.originalMediaPath : project.state.mediaPath;
    proj.intraOptimizeDeclined = project.state.intraOptimizeDeclined;
    proj.dummyDuration = project.state.dummyDuration;
    proj.activeAxisRole = project.state.activeAxis;
    proj.metadata = project.metadata;
    proj.overlaySettings = project.overlay;
    proj.videoPlayerState = project.videoPlayer;
    proj.bookmarkChapters = project.bookmarks;
    proj.totalEditingSeconds = project.state.totalEditingSeconds;
    proj.createdAtUnix = project.state.createdAtUnix;
    proj.modifiedAtUnix = project.state.modifiedAtUnix;
    proj.editSessionCount = project.state.editSessionCount;
    proj.playbackPosition = project.playback.cursorPos;
    proj.autoNameSeed = project.state.autoNameSeed;
    proj.lastExport = project.state.lastExport;
    // Persist the *stored* (authored) ids, never the effective ones — so a project saved while fallen
    // back to native/follow-overlay (its plugin absent) keeps the original selection on disk.
    proj.activeNavigator = project.storedNavigator;
    proj.activeEditMode = project.storedEditMode;
    proj.activeSelectionMode = project.storedSelectionMode;
    proj.showAudioWaveform = project.timelineView.showAudioWaveform;
    proj.pluginData = project.pluginData;
    proj.simP1 = project.simulator.p1;
    proj.simP2 = project.simulator.p2;
    proj.sim3dPos = project.simulator.sim3dPos;
    proj.sim3dSize = project.simulator.sim3dSize;
    proj.defaultSceneView = project.defaultSceneView;

    proj.scriptAxes.clear();
    for (size_t i = 0; i < kStandardAxisCount; ++i) {
        const auto &axis = project.axes[i];
        if (!axis.exists())
            continue;
        SerializedAxis sa;
        sa.role = static_cast<StandardAxis>(i);
        sa.isVisible = axis.isVisible;
        sa.showInStrip = axis.showInStrip;
        sa.isLocked = axis.isLocked;
        sa.actions = axis.actions;
        proj.scriptAxes.push_back(std::move(sa));
    }

    proj.processingRegions = project.regions;
    // Rebuild axisRoleTags from axisRoles bitset
    for (auto &region : proj.processingRegions) {
        region.axisRoleTags.clear();
        for (size_t i = 0; i < kStandardAxisCount; ++i) {
            if (region.axisRoles.test(i))
                region.axisRoleTags.emplace_back(standardAxisTag(static_cast<StandardAxis>(i)));
        }
    }
}

void ProjectManager::onOverlaySettingsChanged(const OverlaySettingsChangedEvent &event) {
    project.overlay = event.state;
    setDirty();
}

void ProjectManager::resolveActiveSceneView() {
    const int idx = project.bookmarks.chapterIndexAt(project.playback.cursorPos);
    if (idx == lastSceneViewIdx)
        return;
    lastSceneViewIdx = idx;
    // A chapter without its own snapshot (or no chapter) falls back to the project default, so a
    // timeline scrub is deterministic rather than dependent on the previously active scene.
    const SceneView *view = &project.defaultSceneView;
    if (idx >= 0) {
        const auto &chapterView = project.bookmarks.chapters[idx].sceneView;
        if (chapterView)
            view = &*chapterView;
    }
    project.activeSceneView = *view;
    eq.push(RestoreSceneViewEvent{view->framing});
}

SceneView &ProjectManager::sceneViewAtCursor() {
    const int idx = project.bookmarks.chapterIndexAt(project.playback.cursorPos);
    lastSceneViewIdx = idx; // a capture is for the current scene; don't re-restore over it
    if (idx < 0)
        return project.defaultSceneView;
    auto &ch = project.bookmarks.chapters[idx];
    if (!ch.sceneView)
        ch.sceneView = project.activeSceneView; // seed the other (unedited) half from the resolved view
    return *ch.sceneView;
}

void ProjectManager::onCaptureOverlayAnchor(const CaptureOverlayAnchorEvent &event) {
    sceneViewAtCursor().anchor = event.anchor;
    project.activeSceneView.anchor = event.anchor;
    setDirty();
}

void ProjectManager::onCaptureVideoFraming(const CaptureVideoFramingEvent &event) {
    sceneViewAtCursor().framing = event.framing;
    project.activeSceneView.framing = event.framing;
    setDirty();
}

void ProjectManager::onCaptureSimInverted(const CaptureSimInvertedEvent &event) {
    sceneViewAtCursor().inverted = event.inverted;
    project.activeSceneView.inverted = event.inverted;
    setDirty();
}

void ProjectManager::onSimulatorPositionChanged(const SimulatorPositionChangedEvent &event) {
    project.simulator = event.state;
    setDirty();
}

void ProjectManager::onMoveSelectionPosition(const MoveSelectionPositionEvent &event) {
    const AxisRoles targets = editTargets(project, event.axis, event.fanToGroup);
    const bool group = targets.count() > 1;
    forEachEditable(project, targets, [&](StandardAxis role, AxisState &axis) {
        if (!axis.selection.empty()) {
            project.mutate(
                role,
                [delta = event.delta](AxisState &a) {
                    auto sIt = a.selection.begin();
                    for (auto aIt = a.actions.begin(); aIt != a.actions.end() && sIt != a.selection.end(); ++aIt) {
                        if (aIt->at == sIt->at) {
                            aIt->pos = std::clamp(aIt->pos + delta, 0, 100);
                            ++sIt;
                        }
                    }
                },
                eq);
        } else if (!group) {
            const ScriptAxisAction *closest = closestActionByTime(axis.actions, project.playback.cursorPos);
            if (!closest)
                return;
            int newPos = std::clamp(closest->pos + event.delta, 0, 100);
            if (newPos == closest->pos)
                return;
            ScriptAxisAction orig = *closest;
            project.mutate(
                role,
                [orig, newPos](AxisState &a) {
                    a.actions.erase(orig);
                    a.actions.insert({orig.at, newPos});
                },
                eq);
        }
    });
}

void ProjectManager::onMoveSelectionTime(const MoveSelectionTimeEvent &event) {
    const AxisRoles targets = editTargets(project, event.axis, event.fanToGroup);
    const bool group = targets.count() > 1;
    const double delta =
        static_cast<double>(static_cast<int>(event.direction) * std::max(1, event.reps)) * stepTime(project.overlay);
    // Fire at most one SeekEvent for the whole (possibly multi-axis) move — off the lead axis.
    std::optional<double> leadSeek;

    forEachEditable(project, targets, [&](StandardAxis role, AxisState &axis) {
        auto isSelected = [&](double at) { return axis.selection.contains(ScriptAxisAction{at, 0}); };
        if (!axis.selection.empty()) {
            for (const auto &action : axis.selection) {
                double newAt = std::max(0.0, action.at + delta);
                if (axis.actions.contains(ScriptAxisAction{newAt, 0}) && !isSelected(newAt))
                    return; // a selected point would land on an unselected one — skip this member
            }

            auto newSelView = axis.selection | std::views::transform([&](const auto &a) {
                                  return ScriptAxisAction{std::max(0.0, a.at + delta), a.pos};
                              });
            VectorSet<ScriptAxisAction> newSel(newSelView.begin(), newSelView.end());

            project.mutate(
                role,
                [&](AxisState &a) {
                    auto rebuiltView = a.actions | std::views::transform([&](const auto &action) {
                                           if (isSelected(action.at))
                                               return ScriptAxisAction{std::max(0.0, action.at + delta), action.pos};
                                           return action;
                                       });
                    a.actions = VectorSet<ScriptAxisAction>(rebuiltView.begin(), rebuiltView.end());
                    a.selection = newSel;
                },
                eq);

            if (role == project.state.activeAxis && !newSel.empty())
                leadSeek = newSel.front().at;
        } else if (!group) {
            const ScriptAxisAction *closest = closestActionByTime(axis.actions, project.playback.cursorPos);
            if (!closest)
                return;
            double newAt = std::max(0.0, closest->at + delta);
            if (axis.actions.contains(ScriptAxisAction{newAt, 0}))
                return;
            moveAction(role, closest->at, newAt, closest->pos);
            leadSeek = newAt;
        }
    });

    if (event.seekAfter && leadSeek)
        eq.push(SeekEvent{*leadSeek});
}

void ProjectManager::onMoveActionToCurrentTime(const MoveActionToCurrentTimeEvent &event) {
    const double currentTime = project.playback.cursorPos;
    forEachEditable(project, editTargets(project, event.axis, event.fanToGroup),
                    [&](StandardAxis role, AxisState &axis) {
                        if (axis.actions.empty())
                            return;
                        const ScriptAxisAction *closest = closestActionByTime(axis.actions, currentTime);
                        if (!closest || closest->at == currentTime)
                            return;
                        if (axis.actions.contains(ScriptAxisAction{currentTime, 0}))
                            return;
                        moveAction(role, closest->at, currentTime, closest->pos);
                    });
}

void ProjectManager::onCopySelection(const CopySelectionEvent &) {
    clipboard.clear();
    // Capture every selected axis in the active group (one AxisClip each). Copying reads only, so a
    // locked axis is still copyable; paste is what skips locked targets.
    const AxisRoles targets = project.effectiveEditSet();
    for (size_t i = 0; i < kStandardAxisCount; ++i) {
        if (!targets.test(i))
            continue;
        const auto &axis = project.axes[i];
        if (axis.selection.empty())
            continue;
        clipboard.push_back({.role = static_cast<StandardAxis>(i), .actions = axis.selection});
    }
}

void ProjectManager::onPasteActions(const PasteActionsEvent &event) {
    if (clipboard.empty())
        return;

    // Resolve the clipboard into target clips by policy: a single copied pattern is broadcast across
    // every editable axis in the current group; a grouped clipboard pastes each clip onto its own role.
    std::vector<AxisClip> clips;
    auto editable = [&](StandardAxis role) {
        const auto idx = static_cast<size_t>(role);
        return role < StandardAxis::Count && !project.axes[idx].isLocked;
    };
    if (clipboard.size() == 1) {
        const auto &src = clipboard.front();
        const AxisRoles targets = project.effectiveEditSet();
        for (size_t i = 0; i < kStandardAxisCount; ++i)
            if (targets.test(i) && editable(static_cast<StandardAxis>(i)))
                clips.push_back({.role = static_cast<StandardAxis>(i), .actions = src.actions});
    } else {
        for (const auto &clip : clipboard)
            if (editable(clip.role))
                clips.push_back(clip);
    }
    if (clips.empty())
        return;

    // One shared anchor across all clips (the earliest action time) so a grouped paste keeps the
    // inter-axis timing the copy captured — each clip is offset by the same amount.
    double anchor = std::numeric_limits<double>::max();
    for (const auto &clip : clips)
        if (!clip.actions.empty())
            anchor = std::min(anchor, clip.actions.front().at);
    if (anchor == std::numeric_limits<double>::max())
        return; // every clip empty

    const double offsetTime = event.pasteTime - anchor;
    double maxEnd = 0.0;

    for (const auto &clip : clips) {
        if (clip.role >= StandardAxis::Count || clip.actions.empty())
            continue;
        const double clipStart = clip.actions.front().at;
        const double clipEnd = clip.actions.back().at;
        if (event.exact) {
            project.mutate(
                clip.role,
                [&](AxisState &a) {
                    auto itStart = a.actions.lowerBound(ScriptAxisAction{clipStart, 0});
                    auto itEnd = a.actions.upperBound(ScriptAxisAction{clipEnd, 0});
                    a.actions.replaceRange(itStart, itEnd, clip.actions.begin(), clip.actions.end());
                },
                eq);
            maxEnd = std::max(maxEnd, clipEnd);
        } else {
            const double destStart = clipStart + offsetTime;
            const double destEnd = clipEnd + offsetTime;
            // Widen the cleared range by a sub-millisecond epsilon so an existing action sitting exactly on
            // a paste boundary is replaced, not left as a duplicate next to the pasted one (time keys are
            // doubles and rarely compare exactly equal).
            constexpr double kPasteTimeEpsilon = 0.0005;
            project.mutate(
                clip.role,
                [&](AxisState &a) {
                    auto itStart = a.actions.lowerBound(ScriptAxisAction{destStart - kPasteTimeEpsilon, 0});
                    auto itEnd = a.actions.upperBound(ScriptAxisAction{destEnd + kPasteTimeEpsilon, 0});
                    auto clipView = clip.actions | std::views::transform([offsetTime](const auto &x) {
                                        return ScriptAxisAction{x.at + offsetTime, x.pos};
                                    });
                    a.actions.replaceRange(itStart, itEnd, clipView.begin(), clipView.end());
                },
                eq);
            maxEnd = std::max(maxEnd, destEnd);
        }
    }

    if (!event.exact)
        eq.push(SeekEvent{maxEnd});
}

void ProjectManager::onRemoveActionAtTime(const RemoveActionAtTimeEvent &event) {
    forEachEditable(
        project, editTargets(project, event.axis, event.fanToGroup), [&](StandardAxis role, AxisState &axis) {
            // Skip members with no point at this time — an erase would be a no-op but still dirty the axis.
            if (!axis.actions.contains(ScriptAxisAction{event.time, 0}))
                return;
            project.mutate(role, [&](AxisState &a) { a.actions.erase(ScriptAxisAction{event.time, 0}); }, eq);
        });
}

void ProjectManager::moveAction(StandardAxis role, double fromAt, double toAt, int toPos) {
    const ScriptAxisAction dest = clampedAction(toAt, toPos); // dragging left of t=0 / off-scale clamps
    project.mutate(
        role,
        [fromAt, dest](AxisState &a) {
            // Carry the selection entry to the new time inside the same mutate, so mutate's
            // selection-sync re-fetches it from actions rather than dropping the stale fromAt.
            if (a.selection.contains(ScriptAxisAction{fromAt, 0})) {
                a.selection.erase(ScriptAxisAction{fromAt, 0});
                a.selection.insert(dest);
            }
            a.actions.erase(ScriptAxisAction{fromAt, 0});
            a.actions.insert(dest);
        },
        eq);
}

void ProjectManager::onMoveAction(const MoveActionEvent &event) {
    if (event.axis >= StandardAxis::Count)
        return;
    const auto leadIdx = static_cast<size_t>(event.axis);

    const AxisRoles targets = editTargets(project, event.axis, event.fanToGroup);

    // The lead keeps the absolute toPos the UI computed; the rest of the group mirrors the *delta*.
    // dpos is measured against the lead's point at fromAt (read before any mutation moves it).
    int leadPosFrom = event.toPos;
    if (auto it = project.axes[leadIdx].actions.find(ScriptAxisAction{event.fromAt, 0});
        it != project.axes[leadIdx].actions.end())
        leadPosFrom = it->pos;
    const double dt = event.toAt - event.fromAt;
    const int dpos = event.toPos - leadPosFrom;

    forEachEditable(project, targets, [&](StandardAxis role, AxisState &axis) {
        if (role == event.axis) {
            moveAction(event.axis, event.fromAt, event.toAt, event.toPos); // lead: absolute
            return;
        }
        auto it = axis.actions.find(ScriptAxisAction{event.fromAt, 0});
        if (it == axis.actions.end())
            return; // member has no point at this time — can't mirror, skip it
        const int memberPos = it->pos;
        const double memberTo = std::max(0.0, event.fromAt + dt);
        // Skip if the destination is already occupied by a different (non-moving) action.
        if (memberTo != event.fromAt && axis.actions.contains(ScriptAxisAction{memberTo, 0}))
            return;
        moveAction(role, event.fromAt, memberTo, std::clamp(memberPos + dpos, 0, 100));
    });
}

void ProjectManager::onAddScratchAxis(const AddScratchAxisEvent &) {
    // Claim the first slot that doesn't yet exist (no data, not in strip, not locked). A scratch axis
    // that holds actions behaves like a standard axis — it persists even while hidden — so it must never
    // be reused here, which would silently wipe its data.
    for (auto i = static_cast<size_t>(StandardAxis::S0); i < kStandardAxisCount; ++i) {
        if (!project.axes[i].exists()) {
            const auto role = static_cast<StandardAxis>(i);
            project.mutate(
                role,
                [](AxisState &a) {
                    a.isVisible = true;
                    a.showInStrip = true;
                    a.isLocked = false;
                    a.actions = {};
                    a.selection = {};
                },
                eq);
            eq.push(AxisPresenceChangedEvent{.change = AxisPresence::AddedToStrip});
            eq.push(AxisSelectedEvent{role});
            return;
        }
    }
}

void ProjectManager::onRemoveAxis(const RemoveAxisEvent &event) {
    const auto roleIdx = static_cast<size_t>(event.axisRole);
    // Only an empty scratch axis is removable. One that holds actions behaves like a standard axis: it is
    // hidden via "Show in Panel", never removed — so removal never has to clear script data.
    if (!isScratchAxis(event.axisRole) || !project.axes[roleIdx].showInStrip || !project.axes[roleIdx].actions.empty())
        return;

    bool wasActive = (project.state.activeAxis == event.axisRole);
    project.state.axesGrouping.reset(roleIdx); // a removed axis can't stay in the edit group

    std::erase_if(project.regions, [roleIdx](const ProcessingRegion &r) { return r.axisRoles.test(roleIdx); });

    // The axis is empty, so dropping it from the strip is enough to make it cease to exist(); there is
    // no script data to clear.
    project.mutate(
        event.axisRole,
        [](AxisState &a) {
            a.showInStrip = false;
            a.isVisible = false;
        },
        eq);

    if (wasActive) {
        for (size_t i = 0; i < kStandardAxisCount; ++i) {
            if (project.axes[i].showInStrip) {
                eq.push(AxisSelectedEvent{static_cast<StandardAxis>(i)});
                break;
            }
        }
    }
    setDirty(true);
    eq.push(AxisPresenceChangedEvent{.change = AxisPresence::RemovedFromStrip});
}

void ProjectManager::onToggleAxisVisibility(const ToggleAxisVisibilityEvent &event) {
    const bool was = project.axes[static_cast<size_t>(event.axisRole)].isVisible;
    project.mutate(event.axisRole, [v = event.visible](AxisState &a) { a.isVisible = v; }, eq, /*affectsData=*/false);
    setDirty(true);
    if (was != event.visible)
        eq.push(AxisPresenceChangedEvent{.change = event.visible ? AxisPresence::Shown : AxisPresence::Hidden});
}

void ProjectManager::onToggleAxisPanelVisibility(const ToggleAxisPanelVisibilityEvent &event) {
    if (event.axisRole == StandardAxis::L0)
        return; // L0 is always shown in the panel
    const bool was = project.axes[static_cast<size_t>(event.axisRole)].showInStrip;
    bool wasActive = (project.state.activeAxis == event.axisRole);
    if (!event.inPanel)
        project.state.axesGrouping.reset(static_cast<size_t>(event.axisRole)); // hidden from strip ⇒ out of group
    project.mutate(
        event.axisRole, [v = event.inPanel](AxisState &a) { a.showInStrip = v; }, eq,
        /*affectsData=*/false);
    if (was != event.inPanel)
        eq.push(AxisPresenceChangedEvent{.change = event.inPanel ? AxisPresence::AddedToStrip
                                                                 : AxisPresence::RemovedFromStrip});
    if (!event.inPanel && wasActive) {
        for (size_t i = 0; i < kStandardAxisCount; ++i) {
            if (project.axes[i].showInStrip) {
                eq.push(AxisSelectedEvent{static_cast<StandardAxis>(i)});
                break;
            }
        }
    }
    setDirty(true);
}

void ProjectManager::onShowMultiAxis(const ShowMultiAxisEvent &) {
    bool changed = false;
    for (size_t i = 0; i <= static_cast<size_t>(StandardAxis::R2); ++i) {
        if (project.axes[i].showInStrip)
            continue;
        project.mutate(
            static_cast<StandardAxis>(i),
            [](AxisState &a) {
                a.isVisible = true;
                a.showInStrip = true;
            },
            eq);
        changed = true;
    }
    if (changed) {
        setDirty(true);
        eq.push(AxisPresenceChangedEvent{.change = AxisPresence::AddedToStrip});
    }
}

void ProjectManager::onShowL0Only(const ShowL0OnlyEvent &) {
    bool changed = false;
    for (size_t i = 0; i < kStandardAxisCount; ++i) {
        const auto role = static_cast<StandardAxis>(i);
        if (role == StandardAxis::L0) {
            if (project.axes[i].showInStrip && project.axes[i].isVisible)
                continue;
            project.mutate(
                role,
                [](AxisState &a) {
                    a.isVisible = true;
                    a.showInStrip = true;
                },
                eq);
            changed = true;
            continue;
        }
        if (!project.axes[i].showInStrip)
            continue;
        // Hide every other axis from the strip, scratch axes included, but never clear data: a scratch
        // axis that holds actions still exists() and stays listed in the Axes menu (re-show via "Show in
        // Panel"). An empty scratch axis has nothing to keep, so hiding it simply removes it.
        project.mutate(role, [](AxisState &a) { a.showInStrip = false; }, eq);
        changed = true;
    }
    // Re-select L0: it sets the active axis (the prior one may have just been hidden) and dissolves any
    // multi-axis group, which can no longer be valid now that only L0 is in the strip.
    eq.push(AxisSelectedEvent{StandardAxis::L0});
    if (changed) {
        setDirty(true);
        eq.push(AxisPresenceChangedEvent{.change = AxisPresence::RemovedFromStrip});
    }
}

void ProjectManager::onToggleAxisLock(const ToggleAxisLockEvent &event) {
    project.mutate(event.axisRole, [v = event.locked](AxisState &a) { a.isLocked = v; }, eq, /*affectsData=*/false);
    setDirty(true);
}

void ProjectManager::onCreateRegion(const CreateRegionEvent &event) {
    const auto roleIdx = static_cast<size_t>(event.axisRole);
    if (event.axisRole >= StandardAxis::Count)
        return;

    constexpr double kMinRegionDur = 0.5;

    int newId = 0;
    for (const auto &r : project.regions)
        newId = std::max(newId, r.id + 1);

    // Place the region in the first free slot at or after the requested start (snap forward past an
    // occupied anchor instead of silently doing nothing). Regions never overlap and are kept sorted
    // by startTime; the timeline length bounds the slot so a snapped region can't run off the media.
    const TimeSlot slot = firstFreeSlot(
        project.regions, event.startTime, event.timelineDuration, [](const ProcessingRegion &r) { return r.startTime; },
        [](const ProcessingRegion &r) { return r.endTime; });

    // Keep the caller's requested span; clamp it into the gap. Preserving the duration (rather than
    // the stale end time) keeps a sensible-length region even when the start was snapped forward.
    const double desiredDur = std::max(0.0, event.endTime - event.startTime);
    const double newStart = slot.start;
    const double newEnd = std::min(slot.end, slot.start + desiredDur);
    if (newEnd - newStart < kMinRegionDur) {
        eq.push(NotifyEvent{.level = NotifyLevel::Warning, .message = Str::PmNoRoomRegion.c_str()});
        return;
    }

    // Span every requested role plus the lead, restricted to shown axes (a stale grouping bit must never
    // seed a region onto a hidden axis). axisRoles defaults empty ⇒ single-axis on the lead.
    AxisRoles roles = event.axisRoles;
    roles.set(roleIdx);
    for (size_t i = 0; i < kStandardAxisCount; ++i)
        if (roles.test(i) && i != roleIdx && !project.axes[i].showInStrip)
            roles.reset(i);

    ProcessingRegion region;
    region.id = newId;
    region.startTime = newStart;
    region.endTime = newEnd;
    region.name = ofs::generateMnemonic(project.state.autoNameSeed + newId);
    region.color =
        ofs::util::goldenRatioColor(static_cast<size_t>(project.state.autoNameSeed) + project.regions.size());
    region.nodeGraph = buildDefaultGraph(roles);
    region.showSourceActions = true;
    region.axisRoles = roles;

    project.procSelRegionId = newId;
    project.regions.push_back(std::move(region));
    project.sortRegions();

    for (size_t i = 0; i < kStandardAxisCount; ++i)
        if (roles.test(i))
            eq.push(AxisModifiedEvent{static_cast<StandardAxis>(i)});
    setDirty();
    eq.push(RegionChangedEvent{.kind = RegionChangeKind::Created});
}

void ProjectManager::onDeleteRegion(const DeleteRegionEvent &event) {
    auto it = std::ranges::find_if(project.regions, [&](const ProcessingRegion &r) { return r.id == event.regionId; });
    if (it == project.regions.end())
        return;
    AxisRoles roles = it->axisRoles;
    if (project.procSelRegionId == event.regionId)
        project.procSelRegionId = -1;
    project.regions.erase(it);

    for (size_t i = 0; i < kStandardAxisCount; ++i) {
        if (!roles.test(i))
            continue;
        eq.push(AxisModifiedEvent{static_cast<StandardAxis>(i)});
    }
    setDirty();
    eq.push(RegionChangedEvent{.kind = RegionChangeKind::Deleted});
}

void ProjectManager::onModifyRegion(const ModifyRegionEvent &event) {
    auto *region = project.findRegion(event.regionId);
    if (!region)
        return;
    const auto &u = event.updatedRegion;
    // A drag-gesture start pushes this event carrying the still-unmodified region purely so
    // UndoSystem snapshots the pre-edit state (snapshot = true). Applying it would be a no-op, and
    // the AxisModifiedEvent below would kick off a pointless re-eval. Skip identical regions.
    if (*region == u)
        return;
    // Name and color are cosmetic; everything else feeds graph evaluation. A recolor (which the picker
    // pushes every frame of the drag) must not re-evaluate the whole node graph each frame.
    const bool evalChanged = region->startTime != u.startTime || region->endTime != u.endTime ||
                             region->showSourceActions != u.showSourceActions || region->hz != u.hz ||
                             region->nodeGraph != u.nodeGraph;
    region->startTime = u.startTime;
    region->endTime = u.endTime;
    region->name = u.name;
    region->color = u.color;
    region->nodeGraph = u.nodeGraph;
    region->showSourceActions = u.showSourceActions;
    region->hz = u.hz;
    const auto axisRoles = region->axisRoles;
    project.sortRegions();

    if (evalChanged)
        for (size_t i = 0; i < kStandardAxisCount; ++i)
            if (axisRoles.test(i))
                eq.push(AxisModifiedEvent{static_cast<StandardAxis>(i)});
    setDirty();
}

void ProjectManager::onMoveRegionNodes(const MoveRegionNodesEvent &event) {
    auto *region = project.findRegion(event.regionId);
    if (!region)
        return;
    for (auto &node : region->nodeGraph.nodes) {
        const auto *src = event.updatedRegion.nodeGraph.findNode(node.id);
        if (src) {
            node.posX = src->posX;
            node.posY = src->posY;
        }
    }
    // Node positions are editor layout, not evaluable data — mark the project dirty so the
    // move is saved, but don't push AxisModifiedEvent (it would trigger a pointless re-eval).
    setDirty();
}

void ProjectManager::onBakeRegion(const BakeRegionEvent &event) {
    auto *region = project.findRegion(event.regionId);
    if (!region)
        return;

    const double startTime = region->startTime;
    const double endTime = region->endTime;
    const AxisRoles axisRoles = region->axisRoles;

    struct BakeEntry {
        StandardAxis role;
        VectorSet<ScriptAxisAction> slice;
    };
    std::vector<BakeEntry> entries;

    for (const auto &node : region->nodeGraph.nodes) {
        if (node.type != GraphNodeType::Output)
            continue;
        const auto roleIdx = static_cast<size_t>(node.role);
        const auto &optResolved1 = project.axes[roleIdx].resolved;
        if (!optResolved1)
            continue;
        const auto &resolved = optResolved1->actions;
        auto view = resolved | std::views::filter([startTime, endTime](const auto &a) {
                        return a.at >= startTime && a.at <= endTime;
                    });
        entries.push_back({.role = node.role, .slice = VectorSet<ScriptAxisAction>(view.begin(), view.end())});
    }

    if (entries.empty()) {
        for (size_t i = 0; i < kStandardAxisCount; ++i) {
            if (!axisRoles.test(i))
                continue;
            const auto &optResolved2 = project.axes[i].resolved;
            if (!optResolved2)
                continue;
            const auto &resolved = optResolved2->actions;
            auto view = resolved | std::views::filter([startTime, endTime](const auto &a) {
                            return a.at >= startTime && a.at <= endTime;
                        });
            entries.push_back(
                {.role = static_cast<StandardAxis>(i), .slice = VectorSet<ScriptAxisAction>(view.begin(), view.end())});
            break;
        }
    }

    if (project.procSelRegionId == event.regionId)
        project.procSelRegionId = -1;

    std::erase_if(project.regions, [&](const ProcessingRegion &r) { return r.id == event.regionId; });

    for (auto &[role, slice] : entries) {
        project.mutate(
            role,
            [&](AxisState &a) {
                auto itStart = a.actions.lowerBound(ScriptAxisAction{startTime, 0});
                auto itEnd = a.actions.upperBound(ScriptAxisAction{endTime, 0});
                a.actions.replaceRange(itStart, itEnd, slice.begin(), slice.end());
            },
            eq);
    }
    eq.push(RegionChangedEvent{.kind = RegionChangeKind::Baked});
}

void ProjectManager::onAssignAxis(const AssignAxisToRegionEvent &event) {
    auto *region = project.findRegion(event.regionId);
    if (!region)
        return;
    const auto axisIdx = static_cast<size_t>(event.axis);

    if (event.assign) {
        if (region->axisRoles.test(axisIdx))
            return;

        // Assignment only adds graph I/O; it never moves the region in time and regions can no
        // longer overlap, so no intersection check is needed.
        region->axisRoles.set(axisIdx);
        float maxY = 0.0f;
        for (const auto &n : region->nodeGraph.nodes)
            if (n.posY > maxY)
                maxY = n.posY;
        const float newY = maxY + 150.0f;
        int inputId = region->nodeGraph.allocId();
        int outputId = region->nodeGraph.allocId();
        region->nodeGraph.nodes.push_back(
            {.id = inputId, .type = GraphNodeType::Input, .posX = 50.0f, .posY = newY, .role = event.axis});
        region->nodeGraph.nodes.push_back(
            {.id = outputId, .type = GraphNodeType::Output, .posX = 400.0f, .posY = newY, .role = event.axis});
        int linkId = region->nodeGraph.allocId();
        region->nodeGraph.links.push_back({.id = linkId, .fromNode = inputId, .toNode = outputId, .toPin = 0});
    } else {
        if (!region->axisRoles.test(axisIdx))
            return;
        // A region may drop to zero axes: it still renders and reserves its time slot,
        // its graph just produces no output until an axis is assigned again.
        region->axisRoles.reset(axisIdx);
        std::vector<int> removedIds;
        std::erase_if(region->nodeGraph.nodes, [&](const ProcessingGraphNode &n) {
            if ((n.type == GraphNodeType::Input || n.type == GraphNodeType::Output) && n.role == event.axis) {
                removedIds.push_back(n.id);
                return true;
            }
            return false;
        });
        std::erase_if(region->nodeGraph.links, [&](const ProcessingGraphLink &l) {
            return std::ranges::find(removedIds, l.fromNode) != removedIds.end() ||
                   std::ranges::find(removedIds, l.toNode) != removedIds.end();
        });
    }

    // Trigger re-evaluation for all still-assigned axes.
    // Also notify the removed axis (if any) so its pending eval is cancelled and resolved cleared.
    const auto updatedRoles = region->axisRoles;
    for (size_t i = 0; i < kStandardAxisCount; ++i) {
        if (updatedRoles.test(i) || (!event.assign && i == axisIdx))
            eq.push(AxisModifiedEvent{static_cast<StandardAxis>(i)});
    }
    setDirty();
}

// ── Graph presets ───────────────────────────────────────────────────────────

static std::filesystem::path graphsDir() {
    auto dir = ofs::util::getPrefPath() / "graphs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

static std::filesystem::path scriptsDir() {
    auto dir = ofs::util::getPrefPath() / "scripts";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

co::Fire ProjectManager::onSaveGraph(SaveGraphEvent event) {
    // Build the entire preset BEFORE the dialog await: `region` is a pointer into project.regions,
    // which may move while the dialog is open, so nothing region-derived may be touched afterward.
    const auto *region = project.findRegion(event.regionId);
    if (!region)
        co_return;

    GraphPreset preset;
    preset.graph = region->nodeGraph;
    for (size_t i = 0; i < kStandardAxisCount; ++i)
        if (region->axisRoles.test(i))
            preset.axisRoleTags.emplace_back(standardAxisTag(static_cast<StandardAxis>(i)));
    // Make the preset self-contained: inline each file-backed Script node's source onto the node so
    // a shared graph carries all its code (graph-embedded nodes already carry theirs). The region's
    // own nodes are untouched — preset.graph is a copy.
    for (auto &n : preset.graph.nodes) {
        if (n.type != GraphNodeType::Script || !n.scriptEmbeddedSource.empty() || n.scriptFile.empty())
            continue;
        auto src = ofs::util::readFile(scriptsDir() / ofs::util::fromUtf8(n.scriptFile));
        if (src)
            n.scriptEmbeddedSource = std::move(*src);
    }
    preset.name = region->name;
    preset.hz = region->hz;
    const std::string defaultName = fmt::format("{}.json", region->name.empty() ? "graph" : region->name);

    std::string file = co_await FileDialog{eq,
                                           {.kind = FileDialogKind::Save,
                                            .key = "graph",
                                            .title = Str::PmDlgSaveGraph.c_str(),
                                            .defaultName = defaultName,
                                            .filterPatterns = {"*.json"},
                                            .filterDesc = Str::PmFilterGraph.c_str(),
                                            .fallbackDir = ofs::util::toUtf8(graphsDir())}};
    if (file.empty())
        co_return;
    std::filesystem::path path = ofs::util::fromUtf8(file);
    if (path.extension() != ".json")
        path += ofs::util::fromUtf8(".json");
    if (!preset.save(path, effectReg))
        showError(eq, Str::PmErrorTitle.c_str(), Str::PmErrSaveGraphFailed.c_str());
}

co::Fire ProjectManager::onLoadGraph(LoadGraphEvent event) {
    // Cheap existence check first (so a gone region shows no dialog); re-find after the await.
    if (!project.findRegion(event.regionId))
        co_return;

    std::string file = co_await FileDialog{eq,
                                           {.kind = FileDialogKind::Open,
                                            .key = "graph",
                                            .title = Str::PmDlgLoadGraph.c_str(),
                                            .filterPatterns = {"*.json"},
                                            .filterDesc = Str::PmFilterGraph.c_str(),
                                            .fallbackDir = ofs::util::toUtf8(graphsDir())}};
    if (file.empty())
        co_return;

    // The region list may have changed while the dialog was open — re-find before using it below.
    const auto *region = project.findRegion(event.regionId);
    if (!region)
        co_return;

    GraphLoadResult loaded = loadGraphPreset(ofs::util::fromUtf8(file), effectReg);
    if (!loaded.preset) {
        showError(eq, Str::PmErrorTitle.c_str(), Str::PmErrLoadGraph.fmt(loaded.error));
        co_return;
    }
    GraphPreset &preset = *loaded.preset;

    if (!loaded.missingDeps.empty()) {
        std::string msg = Str::PmMissingDepsHeader.c_str();
        for (const auto &dep : loaded.missingDeps)
            msg += fmt::format("\n  {}", dep);
        msg += Str::PmMissingDepsFooter.c_str();
        showWarning(eq, Str::PmMissingDepsTitle.c_str(), msg);
    }

    // Distinct axes the graph targets, from its Input/Output nodes (encounter order).
    std::vector<StandardAxis> savedAxes;
    AxisRoles savedRoles;
    for (const auto &n : preset.graph.nodes) {
        if (n.type != GraphNodeType::Input && n.type != GraphNodeType::Output)
            continue;
        const auto idx = static_cast<size_t>(n.role);
        if (!savedRoles.test(idx)) {
            savedRoles.set(idx);
            savedAxes.push_back(n.role);
        }
    }
    if (savedAxes.empty()) {
        showError(eq, Str::PmErrorTitle.c_str(), Str::PmErrGraphNoAxes.c_str());
        co_return;
    }

    // A graph needs the trust gate only if it ships embedded code we have not accepted before. Once
    // its code is trusted (hash recorded), it loads without re-prompting until that code changes.
    const std::string trustHash = graphTrustHash(preset.graph, shippedScriptHashes);
    const bool needsTrust = !trustHash.empty() && !trustedGraphHashes.contains(trustHash);

    // A graph that needs no trust and matches the region's axes applies immediately. Anything else
    // is deferred to a confirmation: a trust warning when it ships untrusted scripts (its code is
    // not compiled until accepted), and/or the axis-remap dialog.
    if (!needsTrust && savedRoles == region->axisRoles) {
        if (applyLoadedGraph(event.regionId, std::move(preset.graph), savedRoles, preset.name, preset.hz))
            setDirty();
        co_return;
    }

    project.pendingGraphLoad = PendingGraphLoad{.regionId = event.regionId,
                                                .graph = std::move(preset.graph),
                                                .savedAxes = std::move(savedAxes),
                                                .name = std::move(preset.name),
                                                .hz = preset.hz,
                                                .needsTrust = needsTrust};
}

void ProjectManager::onApplyGraphRemap(const ApplyGraphRemapEvent &event) {
    if (!project.pendingGraphLoad || project.pendingGraphLoad->regionId != event.regionId)
        return;
    auto &pending = *project.pendingGraphLoad;
    auto *region = project.findRegion(event.regionId);
    if (!region) {
        project.pendingGraphLoad.reset();
        return;
    }

    // Build the new axis set; reject collisions (two saved axes onto one target).
    AxisRoles newRoles;
    for (const auto &m : event.mapping) {
        const auto idx = static_cast<size_t>(m.to);
        if (newRoles.test(idx)) {
            showError(eq, Str::PmErrorTitle.c_str(), Str::PmErrDuplicateTarget.c_str());
            return;
        }
        newRoles.set(idx);
    }

    // Rewrite Input/Output node roles via the mapping.
    ProcessingNodeGraph graph = std::move(pending.graph);
    for (auto &n : graph.nodes) {
        if (n.type != GraphNodeType::Input && n.type != GraphNodeType::Output)
            continue;
        for (const auto &m : event.mapping) {
            if (n.role == m.from) {
                n.role = m.to;
                break;
            }
        }
    }

    std::string name = std::move(pending.name);
    const int hz = pending.hz;
    project.pendingGraphLoad.reset();
    if (applyLoadedGraph(event.regionId, std::move(graph), newRoles, name, hz))
        setDirty();
}

void ProjectManager::onCancelGraphLoad(const CancelGraphLoadEvent &event) {
    if (project.pendingGraphLoad && project.pendingGraphLoad->regionId == event.regionId)
        project.pendingGraphLoad.reset();
}

void ProjectManager::onRemapCurrentGraph(const RemapCurrentGraphEvent &event) {
    const auto *region = project.findRegion(event.regionId);
    if (!region)
        return;

    // Distinct axes the region's current graph targets, from its Input/Output nodes (encounter order).
    std::vector<StandardAxis> savedAxes;
    AxisRoles savedRoles;
    for (const auto &n : region->nodeGraph.nodes) {
        if (n.type != GraphNodeType::Input && n.type != GraphNodeType::Output)
            continue;
        const auto idx = static_cast<size_t>(n.role);
        if (!savedRoles.test(idx)) {
            savedRoles.set(idx);
            savedAxes.push_back(n.role);
        }
    }
    if (savedAxes.empty())
        return; // nothing to remap

    // Reuse the load-time remap flow on a copy of the live graph. The original stays put until the
    // user confirms (ApplyGraphRemapEvent) or cancels; the code already ran, so no trust gate.
    project.pendingGraphLoad = PendingGraphLoad{.regionId = event.regionId,
                                                .graph = region->nodeGraph,
                                                .savedAxes = std::move(savedAxes),
                                                .name = region->name,
                                                .hz = region->hz,
                                                .needsTrust = false};
}

void ProjectManager::onConfirmGraphTrust(const ConfirmGraphTrustEvent &event) {
    if (!project.pendingGraphLoad || project.pendingGraphLoad->regionId != event.regionId)
        return;
    auto &pending = *project.pendingGraphLoad;
    if (!pending.needsTrust)
        return;
    auto *region = project.findRegion(event.regionId);
    if (!region) {
        project.pendingGraphLoad.reset();
        return;
    }

    // The user accepted. The scripts already live on their nodes (graph-embedded); they run
    // in-memory and nothing is written to the scripts folder. applyLoadedGraph kicks off the
    // compiles; the user can later promote a node to a file via SaveEmbeddedScriptEvent.
    pending.needsTrust = false;

    // Remember this code so the same graph (or any graph carrying identical scripts) loads without
    // prompting again, until the embedded code changes.
    if (std::string hash = graphTrustHash(pending.graph, shippedScriptHashes);
        !hash.empty() && trustedGraphHashes.insert(hash).second)
        saveTrustedGraphs();

    // If the graph's axes already match the region, apply immediately; otherwise leave the
    // now-trusted pending load in place so the axis-remap dialog can finish it.
    AxisRoles savedRoles;
    for (const auto &n : pending.graph.nodes)
        if (n.type == GraphNodeType::Input || n.type == GraphNodeType::Output)
            savedRoles.set(static_cast<size_t>(n.role));
    if (savedRoles == region->axisRoles) {
        ProcessingNodeGraph graph = std::move(pending.graph);
        std::string name = std::move(pending.name);
        const int hz = pending.hz;
        project.pendingGraphLoad.reset();
        if (applyLoadedGraph(event.regionId, std::move(graph), savedRoles, name, hz))
            setDirty();
    }
}

void ProjectManager::onReviewGraphScripts(const ReviewGraphScriptsEvent &event) {
    if (!project.pendingGraphLoad || project.pendingGraphLoad->regionId != event.regionId)
        return;
    const auto &pending = *project.pendingGraphLoad;

    std::error_code ec;
    const auto reviewDir = std::filesystem::temp_directory_path(ec) / "ofs-graph-review";
    if (ec)
        return;
    // Wipe and recreate so a previous review's files can never linger and mislead the reader.
    std::filesystem::remove_all(reviewDir, ec);
    std::filesystem::create_directories(reviewDir, ec);

    std::vector<std::filesystem::path> written;
    std::set<std::filesystem::path> usedNames;
    int idx = 0;
    for (const auto &n : pending.graph.nodes) {
        if (!n.scriptEmbedded())
            continue;
        // Only the filename component — never honor any directory parts an author may have baked
        // into scriptFile, which could escape the throwaway review dir (path traversal).
        std::filesystem::path name =
            n.scriptFile.empty() ? std::filesystem::path{} : ofs::util::fromUtf8(n.scriptFile).filename();
        if (name.empty())
            name = ofs::util::fromUtf8(fmt::format("embedded_{}.cs", idx));
        if (name.extension() != ".cs")
            name += ".cs";
        // Disambiguate same-named scripts so each node's source is reviewable on its own.
        if (!usedNames.insert(name).second)
            name = ofs::util::fromUtf8(fmt::format("{}_{}.cs", ofs::util::toUtf8(name.stem()), idx));
        ++idx;

        const auto path = reviewDir / name;
        if (ofs::util::writeFile(path, n.scriptEmbeddedSource))
            written.push_back(path);
    }

    for (const auto &p : written)
        ofs::util::openInDefaultApp(p);
}

void ProjectManager::onSaveEmbeddedScript(const SaveEmbeddedScriptEvent &event) {
    auto *region = project.findRegion(event.regionId);
    if (!region || event.fileName.empty())
        return;
    auto *node = region->nodeGraph.findNode(event.nodeId);
    if (!node || node->type != GraphNodeType::Script)
        return;

    // The source to write: an embedded node carries it inline; a file node forks the referenced
    // file's current contents into the new name.
    std::string source = node->scriptEmbeddedSource;
    if (source.empty()) {
        if (node->scriptFile.empty())
            return;
        auto existing = ofs::util::readFile(scriptsDir() / ofs::util::fromUtf8(node->scriptFile));
        if (!existing) {
            showError(eq, Str::PmErrorTitle.c_str(), Str::PmErrForkRead.c_str());
            return;
        }
        source = std::move(*existing);
    }

    const auto path = scriptsDir() / ofs::util::fromUtf8(event.fileName);
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        // The UI only sends a name that is free or byte-identical; re-check here so a race or a
        // stale dialog can never clobber a different local file.
        auto local = ofs::util::readFile(path);
        if (!local || *local != source) {
            showError(eq, Str::PmErrorTitle.c_str(), Str::PmErrScriptNameExists.c_str());
            return;
        }
    } else {
        if (!ofs::util::writeFile(path, source)) {
            showError(eq, Str::PmErrorTitle.c_str(), Str::PmErrWriteScript.c_str());
            return;
        }
    }

    // Promote/repoint the node to the (new) file: it now references the saved file and carries no source.
    node->scriptFile = event.fileName;
    node->scriptEmbeddedSource.clear();
    setDirty();
    eq.push(CompileScriptEvent{.fileName = event.fileName});
}

bool ProjectManager::applyLoadedGraph(int regionId, ProcessingNodeGraph graph, AxisRoles newRoles,
                                      const std::string &name, int hz) {
    auto *region = project.findRegion(regionId);
    if (!region)
        return false;
    const AxisRoles oldRoles = region->axisRoles;
    region->nodeGraph = std::move(graph);
    region->axisRoles = newRoles;
    region->hz = hz;
    if (!name.empty())
        region->name = name;

    // Compile this graph's script nodes (the snapshot resolves a ref only once compiled): embedded
    // nodes by their source, file nodes by name (the file may exist on this machine even if it was
    // missing when the graph was saved).
    for (const auto &n : region->nodeGraph.nodes) {
        if (n.type != GraphNodeType::Script)
            continue;
        if (!n.scriptEmbeddedSource.empty())
            eq.push(CompileEmbeddedScriptEvent{.source = n.scriptEmbeddedSource});
        else if (!n.scriptFile.empty())
            eq.push(CompileScriptEvent{.fileName = n.scriptFile});
    }

    project.sortRegions();

    // Re-evaluate the union of old and new roles; axes dropped from the region get
    // an AxisModified so their stale resolved/pending eval is cleared.
    const AxisRoles affected = oldRoles | newRoles;
    for (size_t i = 0; i < kStandardAxisCount; ++i)
        if (affected.test(i))
            eq.push(AxisModifiedEvent{static_cast<StandardAxis>(i)});
    return true;
}

void ProjectManager::onVideoModeChanged(const VideoModeChangedEvent &event) {
    project.videoPlayer.activeMode = event.mode;
    setDirty();
}

void ProjectManager::onVideoResolutionChanged(const VideoResolutionChangedEvent &event) {
    project.videoPlayer.resolutionScale = event.scale;
    setDirty();
}

void ProjectManager::onSelectRegion(const SelectRegionEvent &event) {
    project.procSelRegionId = event.regionId;
}

void ProjectManager::onClearRegionSelection(const ClearRegionSelectionEvent &) {
    project.procSelRegionId = -1;
}

void ProjectManager::onSetProcPanelLocked(const SetProcPanelLockedEvent &event) {
    project.procPanelLocked = event.locked;
}

void ProjectManager::onUpdateTimelineView(const UpdateTimelineViewEvent &event) {
    project.timelineView.visibleTime = event.visibleTime;
    project.timelineView.offsetTime = event.offsetTime;
}

void ProjectManager::onSetTimelineShowPoints(const SetTimelineShowPointsEvent &event) {
    project.timelineView.showPoints = event.show;
}

void ProjectManager::onSetTimelineShowWaveform(const SetTimelineShowWaveformEvent &event) {
    project.timelineView.showAudioWaveform = event.show;
}

void ProjectManager::onCommitAxisActions(const CommitAxisActionsEvent &event) {
    if (event.axis >= StandardAxis::Count)
        return;
    // A plugin supplies these actions, so clamp each to the action invariants at this boundary.
    auto clamped = event.actions | std::views::transform([](const auto &a) { return clampedAction(a.at, a.pos); });
    project.mutate(
        event.axis, [&](AxisState &a) { a.actions = VectorSet<ScriptAxisAction>(clamped.begin(), clamped.end()); }, eq);
}

void ProjectManager::onSetAxisSelection(const SetAxisSelectionEvent &event) {
    if (event.axis >= StandardAxis::Count)
        return;
    project.setSelection(event.axis, event.selection, eq);
}

void ProjectManager::onSetPluginProjectData(const SetPluginProjectDataEvent &event) {
    if (event.pluginName.empty() || event.key.empty())
        return;
    if (!project.pluginData.is_object()) // defensive: a corrupt load could leave it non-object
        project.pluginData = nlohmann::json::object();
    if (event.value.is_null()) {
        auto slot = project.pluginData.find(event.pluginName);
        if (slot == project.pluginData.end())
            return; // nothing to erase, nothing changed → no dirty
        slot->erase(event.key);
        if (slot->empty())
            project.pluginData.erase(slot); // don't leave an empty plugin object behind
    } else {
        project.pluginData[event.pluginName][event.key] = event.value;
    }
    setDirty();
}

void ProjectManager::onOpenOrNewProjectRequest(const OpenOrNewProjectRequestEvent &) {
    guardUnsaved([this] { startOpenOrNewProject(); });
}

void ProjectManager::onOpenProjectRequest(const OpenProjectRequestEvent &event) {
    if (event.path.empty())
        guardUnsaved([this] { startOpenOrNewProject(); });
    else
        guardUnsaved([this, path = ofs::util::fromUtf8(event.path)] {
            doClose();
            loadProjectInternal(path);
        });
}

void ProjectManager::onRestoreBackupRequest(const RestoreBackupRequestEvent &event) {
    // Capture the current file association now, before guardUnsaved/doClose clears it: the restored
    // backup must re-adopt the project it belongs to so a later Save targets the real file.
    std::filesystem::path target = ofs::util::fromUtf8(project.state.filePath);
    guardUnsaved([this, backupPath = ofs::util::fromUtf8(event.backupPath), target] {
        doClose();
        restoreBackupInternal(backupPath, target);
    });
}

void ProjectManager::onCloseProjectRequest(const CloseProjectRequestEvent &) {
    guardUnsaved([this] {
        doClose();
        // Only the explicit user-driven close suppresses the next launch's auto-reopen; the open/new
        // flow's internal doClose() does not push this, as its following load re-arms reopenLastProject.
        eq.push(ProjectClosedEvent{});
    });
}

void ProjectManager::onRequestExit(const RequestExitEvent &) {
    guardUnsaved([this] { eq.push(ExitConfirmedEvent{}); });
}

void ProjectManager::onImportFunscriptRequest(const ImportFunscriptRequestEvent &) {
    importFunscript();
}

void ProjectManager::onExportFunscriptRequest(const ExportFunscriptRequestEvent &event) {
    if (event.format == 0)
        exportMultipleFunscript10(event.axes, event.targetPath);
    else
        exportMultiAxisFunscript(event.axes, event.format == 2, event.targetPath);
}

void ProjectManager::recordLastExport(int format, std::vector<StandardAxis> axes, std::string outputPath) {
    project.state.lastExport =
        ExportConfig{.format = format, .axes = std::move(axes), .outputPath = std::move(outputPath)};
    // Mark dirty so the remembered config is captured on the next save — that is what lets Quick
    // Export survive a reopen of the project.
    setDirty(true);
}

} // namespace ofs
