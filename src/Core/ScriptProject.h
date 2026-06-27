#pragma once

#include "Core/AppState.h"
#include "Core/BookmarkChapterState.h"
#include "Core/Events.h"
#include "Core/ExportConfig.h"
#include "Core/FunscriptMetadata.h"
#include "Core/OverlaySettings.h"
#include "Core/PendingGraphLoad.h"
#include "Core/ProcessingRegion.h"
#include "Core/SceneView.h"
#include "Core/ScriptAxisAction.h"
#include "Core/SimulatorSettings.h"
#include "Core/StandardAxis.h"
#include "Core/TranscodeState.h"
#include "Core/VectorSet.h"
#include "Services/JobSystem.h"
#include "Video/VideoPlayerSettings.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace ofs {

class EventQueue;

struct ResolvedActions {
    VectorSet<ScriptAxisAction> actions;
    double evalMs = 0.0;
};

struct AxisState {
    StandardAxis role = StandardAxis::L0;
    bool isVisible = true;
    bool showInStrip = false; // strip-row visibility / UI interactability only — NOT existence; see exists()
    bool isLocked = false;
    bool dirty = false;

    VectorSet<ScriptAxisAction> actions;
    VectorSet<ScriptAxisAction> selection;
    std::optional<ResolvedActions> resolved;

    std::shared_ptr<EvalJob> pendingEval; // null = idle; UI checks for spinner

    // Single source of truth for "this axis is part of the project". An axis exists once the user
    // surfaces it in the strip, it carries script data, or it is locked — i.e. anything that must
    // round-trip through a save. showInStrip alone is the narrower UI question of whether a strip row
    // is drawn (and thus whether the axis can be activated/grouped): a hidden axis that still holds
    // data exists, persists, and is visible to plugins, but draws no strip row until re-shown.
    // Persistence (the save filter) and plugin enumeration key off exists(); strip render, activation,
    // and grouping key off showInStrip. The two diverge for any axis hidden with data — including a
    // scratch axis (S0–S9): once it holds actions it persists and behaves like a standard axis, hidden
    // or not, and can no longer be removed (only an empty scratch axis is removable).
    bool exists() const { return showInStrip || !actions.empty() || isLocked; }
};

// Timeline length given to a media-less project (an "empty" project, or "Start without video"). Never
// zero: a zero-length timeline has no editable span, so an empty project would have nowhere to place
// actions. Five minutes is a sane starting canvas the user can change in Project Configuration.
inline constexpr double kDefaultDummyDuration = 300.0;

struct ProjectState {
    std::string filePath;
    // The resolved active media path (UTF-8) — always equals whichever of original/intra `activeSource`
    // points at, and the value handed to LoadVideoEvent. Kept so the existing load path is unchanged.
    std::string mediaPath;
    // The two sources behind `mediaPath` (both UTF-8). originalMediaPath is the video the user opened
    // and is persisted; intraMediaPath is the all-intra optimized copy in the shared output dir
    // ("" = none). intraMediaPath is transient — its deterministic <fingerprint>.mp4 location is
    // rediscovered on load (discoverExistingIntra), never serialized.
    std::string originalMediaPath;
    std::string intraMediaPath;
    // Sticky "Not Now" for the optimize prompt, scoped to the current originalMediaPath: set when the
    // user dismisses the offer, persisted, and cleared when a different original is loaded so a new video
    // is offered afresh. See OfsApp::maybeOfferOptimize.
    bool intraOptimizeDeclined = false;
    double dummyDuration = 0.0;
    StandardAxis activeAxis = StandardAxis::L0;
    // Transient fan-out scope for multi-axis editing (session-only): NOT serialized to .ofp, NOT
    // captured in undo snapshots. Effective edit set = axesGrouping when it has >1 member, else just
    // {activeAxis}; when non-empty it always includes activeAxis (the lead). Formed in the timeline left
    // strip; dissolved by activating an axis outside it. Reset with the rest of ProjectState on a new
    // project (resetDocument does `state = ProjectState{}`). See ScriptProject::effectiveEditSet.
    AxisRoles axesGrouping;
    bool settingsDirty = false;
    double totalEditingSeconds = 0.0;
    // Wall-clock provenance (Unix seconds). createdAtUnix is stamped once on the first save; modifiedAtUnix
    // on every user save. Both 0 until the project has been saved at least once. Round-trip with the .ofp.
    int64_t createdAtUnix = 0;
    int64_t modifiedAtUnix = 0;
    // Number of editing sessions (one per project open / new). A fresh project starts at 1 (set in
    // ProjectManager::clearProject); bumped on each load. Persisted so the running total survives reopens.
    int editSessionCount = 0;
    // Action total (across all axes) captured when this session began — the baseline for the Info tab's
    // "this session" net-edits delta. Transient: recomputed on every open, never serialized.
    int sessionBaselineActions = 0;
    std::optional<ExportConfig> lastExport; // last funscript export; drives dialog-less Quick Export
    // Per-project offset into the auto-naming/coloring sequences (region mnemonics, chapter hues), so
    // two projects don't both open on "Bold Arc"/the same first color. Randomized for a fresh project
    // in ProjectManager::clearProject, persisted so a reopened project reproduces its names/colors.
    int autoNameSeed = 0;
};

// Which of ProjectState's two remembered sources is currently loaded into the player. Transient
// session state, resolved on open (Intra is preferred whenever it exists) — never serialized.
enum class MediaSource { Original, Intra };

struct ScriptProject {
    AxisState axes[kStandardAxisCount];
    ProjectState state;
    OverlayState overlay;
    SimulatorState simulator;
    VideoPlayerState videoPlayer;
    FunscriptMetadata metadata;
    BookmarkChapterState bookmarks;
    PlaybackState playback;
    TimelineViewState timelineView;
    int procSelRegionId = -1;
    std::vector<ProcessingRegion> regions; // always sorted by startTime

    // Per-plugin custom data persisted inside the .ofp (round-trips with save/load). A JSON object
    // shaped pluginName → { key → value }: the outer key is the plugin name (== currentPluginName),
    // each plugin owning its own key→value store. The host never interprets the values. Written only
    // via SetPluginProjectDataEvent (ProjectManager), which marks the project dirty; metadata-like, so
    // NOT captured in undo. Always an object (never null) so it serializes as {} when empty.
    nlohmann::json pluginData = nlohmann::json::object();

    // Per-chapter scene memory. defaultSceneView is the serialized fallback used when the
    // cursor is outside every chapter. activeSceneView is transient — the resolved view
    // (chapter override or fallback) for the current cursor; ProjectManager recomputes it
    // when the cursor's chapter changes, and rendering reads its anchor each frame.
    SceneView defaultSceneView;
    SceneView activeSceneView;

    // Transient: a graph preset loaded from disk awaiting axis-remap confirmation.
    // Not serialized; reset on project close. See PendingGraphLoad.
    std::optional<PendingGraphLoad> pendingGraphLoad;

    // Transient: when false, axis edits no longer trigger automatic re-evaluation; the user
    // recomputes on demand via the processing panel. Not serialized; resets to true each session.
    bool autoEvalEnabled = true;

    // Transient: which remembered source (state.original/intraMediaPath) is loaded into the player.
    // Recomputed on open (prefer Intra when present); NOT serialized.
    MediaSource activeSource = MediaSource::Original;

    // Transient: progress mirror of the running intra-frame transcode (if any). Driven by the
    // transcode events, read by the blocking progress modal. NOT serialized.
    TranscodeState transcode;

    // Effective navigator id — what the prev/next-step keys do *right now* (the id NavigatorRouter
    // dispatches on and the footer shows). Equals storedNavigator while that id resolves to a loaded
    // navigator; falls back to follow-overlay when the owning plugin is absent (uninstalled, disabled,
    // unloaded). Transient/derived — itself NOT serialized; storedNavigator carries persistence.
    // Default matches kFollowOverlayNavigatorId.
    std::string activeNavigator = "follow-overlay";

    // Effective edit mode id — how timeline edit gestures resolve *right now* (the id EditIntentRouter
    // dispatches on and the footer shows). Same effective/stored split as activeNavigator: equals
    // storedEditMode while resolvable, else native. Transient/derived; storedEditMode carries
    // persistence. Default matches kNativeEditModeId.
    std::string activeEditMode = "native";

    // Effective selection mode id — what a selection gesture (marquee / Ctrl+A / click) selects *right
    // now* (the id SelectIntentRouter dispatches on and the footer shows). Same effective/stored split
    // as the two above: equals storedSelectionMode while resolvable, else native. Transient/derived;
    // storedSelectionMode carries persistence. Default matches kNativeSelectionModeId.
    std::string activeSelectionMode = "native";

    // Stored (authored) navigator / edit-mode ids — the user's selection persisted with the .ofp
    // (Format/Project.cpp). They restore the interaction context a project was authored in. A *tool*
    // selection, not a content edit, so excluded from the undo snapshot. Held weakly by id: when the
    // owning plugin is absent the *effective* ids above fall back, but these stored ids are left intact
    // so a re-save preserves the authored selection (restored if the plugin returns). Changing the
    // selection in the footer updates both stored and effective.
    std::string storedNavigator = "follow-overlay";
    std::string storedEditMode = "native";
    std::string storedSelectionMode = "native";

    // Apply fn to the named axis, sync selection, set axis.dirty, push AxisModifiedEvent.
    // Main thread only.
    void mutate(StandardAxis role, const std::function<void(AxisState &)> &fn, EventQueue &eq);

    // Replace axis selection, sync against actions, push SelectionChangedEvent.
    // Does NOT set dirty. Main thread only.
    void setSelection(StandardAxis role, const VectorSet<ScriptAxisAction> &newSel, EventQueue &eq);

    // The set of axes an activeAxis-targeted edit fans out to: axesGrouping when it has >1 member,
    // else just {activeAxis}. Always includes activeAxis. Edit handlers loop this and skip
    // absent/locked members. Collapses to a single bit when no group is active.
    AxisRoles effectiveEditSet() const;

    // Clear all axis.dirty and state.settingsDirty. Call after a successful save.
    void clearDirtyFlags();

    // Reset all axes to absent/empty state, including transient fields. No events pushed.
    // Call before loadFromProject or when discarding a project.
    void resetAxes();

    // Reset every per-project document field to its default — axes, regions, and all the top-level
    // state structs that a .ofp can carry (overlay, video-player mode, metadata, bookmarks, scene
    // views, view/playback state, the per-project simulator placement). This is the single guard
    // against settings from one project bleeding into the next: a freshly created/empty project that
    // never calls loadFromProject still starts from defaults. App-level simulator state (3D ranges,
    // feature toggles, posing) is NOT touched — it lives in AppSettings and persists across projects.
    // No events pushed. Call when discarding a project (ProjectManager::clearProject).
    void resetDocument();

    // Bulk-write all document fields of one axis and reset its transient fields.
    // No events pushed — caller is responsible for pushing AxisModifiedEvent after.
    void restoreAxis(StandardAxis role, bool isVisible, bool showInStrip, bool isLocked, bool dirty,
                     VectorSet<ScriptAxisAction> actions, VectorSet<ScriptAxisAction> selection);

    // Keep regions sorted by startTime; multi-axis regions sort before single-axis on ties.
    void sortRegions();

    // Find a region by its integer id (-1 if not found).
    ProcessingRegion *findRegion(int id);
    const ProcessingRegion *findRegion(int id) const;
};

} // namespace ofs
