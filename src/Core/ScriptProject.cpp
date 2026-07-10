#include "Core/ScriptProject.h"
#include "Core/EventQueue.h"
#include "Core/Events.h"
#include <algorithm>
#include <ranges>

namespace ofs {

namespace {
// Keep only the entries of `src` that still exist in `actions`, mapped to their canonical stored value
// (so an entry's position tracks the action it refers to). Reconciles a selection after the action set
// changes or when a fresh selection is assigned.
VectorSet<ScriptAxisAction> selectionWithinActions(const VectorSet<ScriptAxisAction> &src,
                                                   const VectorSet<ScriptAxisAction> &actions) {
    // A plain loop rather than a `views::filter | views::transform` pipe: clang paired with libstdc++-12
    // (the ubuntu-22.04 release runner) fails to instantiate a ref_view over a const class range, and
    // src is already sorted/unique so the filtered-and-mapped result stays ordered for the VectorSet ctor.
    std::vector<ScriptAxisAction> kept;
    kept.reserve(src.size());
    for (const auto &e : src) {
        if (auto it = actions.find(e); it != actions.end())
            kept.push_back(*it);
    }
    return {kept.begin(), kept.end()};
}
} // namespace

void ScriptProject::mutate(StandardAxis role, const std::function<void(AxisState &)> &fn, EventQueue &eq,
                           bool affectsData) {
    auto &axis = axes[static_cast<size_t>(role)];
    fn(axis);
    // Sync selection: remove any selected actions that no longer exist in the axis.
    if (!axis.selection.empty())
        axis.selection = selectionWithinActions(axis.selection, axis.actions);
    axis.dirty = true;
    ++editRevision;
    // Display-only flag writes (isVisible/isLocked/showInStrip) leave the action data untouched, so they
    // pass affectsData=false to skip AxisModifiedEvent — that event means "the axis's resolved data may
    // have changed" and would otherwise trigger a pointless processing re-eval and plugin notify for a
    // change the graph never sees. The edit still persists (dirty + editRevision bump above).
    if (affectsData)
        eq.push(AxisModifiedEvent{role});
}

void ScriptProject::setSelection(StandardAxis role, const VectorSet<ScriptAxisAction> &newSel, EventQueue &eq) {
    auto &axis = axes[static_cast<size_t>(role)];
    axis.selection = newSel.empty() ? VectorSet<ScriptAxisAction>{} : selectionWithinActions(newSel, axis.actions);
    eq.push(SelectionChangedEvent{role});
}

AxisRoles ScriptProject::effectiveEditSet() const {
    AxisRoles set;
    // A stale single-bit group is treated as "no group"; only a real (>1) group widens the scope.
    if (state.axesGrouping.count() > 1)
        set = state.axesGrouping;
    if (state.activeAxis < StandardAxis::Count)
        set.set(static_cast<size_t>(state.activeAxis)); // the lead is always part of the set
    return set;
}

void ScriptProject::clearDirtyFlags() {
    for (auto &axis : axes)
        axis.dirty = false;
    state.settingsDirty = false;
}

void ScriptProject::resetAxes() {
    for (auto &axis : axes) {
        axis.isVisible = false;
        axis.showInStrip = false;
        axis.isLocked = false;
        axis.dirty = false;
        axis.actions = {};
        axis.selection = {};
        axis.resolved = std::nullopt;
        axis.pendingEval = nullptr;
    }
}

void ScriptProject::resetDocument() {
    resetAxes();
    regions.clear();
    procSelRegionId = -1;
    pluginData = nlohmann::json::object();
    pendingGraphLoad.reset();
    // Effective + stored interaction selections. A load overwrites the stored ids from the file right
    // after and the routers re-derive the effective ids from them on LoadProjectEvent.
    activeNavigator = "follow-overlay";
    activeEditMode = "native";
    activeSelectionMode = "native";
    storedNavigator = "follow-overlay";
    storedEditMode = "native";
    storedSelectionMode = "native";
    // Recomputed by loadFromProject on open (prefer Intra); a fresh/empty project starts on Original.
    activeSource = MediaSource::Original;
    transcode = TranscodeState{};
    // axesGrouping lives in ProjectState and is reset below by `state = ProjectState{}`.

    // Whole-struct default-assignment is deliberate: it means a field added to any of these structs
    // later is reset automatically, without anyone having to remember to extend this list.
    state = ProjectState{};
    overlay = OverlayState{};
    videoPlayer = VideoPlayerState{};
    metadata = FunscriptMetadata{};
    bookmarks = BookmarkChapterState{};
    playback = PlaybackState{};
    timelineView = TimelineViewState{};
    defaultSceneView = SceneView{};
    activeSceneView = SceneView{};

    // SimulatorState is split: the 3D ranges, feature toggles and transient posing fields are app-level
    // (seeded from AppSettings at startup, persisted back at exit) and must survive a project close. Only
    // the widget placement is per-project document state, so reset just that.
    simulator.resetPlacement();
}

void ScriptProject::restoreAxis(StandardAxis role, bool isVisible, bool showInStrip, bool isLocked, bool dirty,
                                VectorSet<ScriptAxisAction> actions, VectorSet<ScriptAxisAction> selection) {
    auto &axis = axes[static_cast<size_t>(role)];
    axis.isVisible = isVisible;
    axis.showInStrip = showInStrip;
    axis.isLocked = isLocked;
    axis.dirty = dirty;
    axis.actions = std::move(actions);
    axis.selection = std::move(selection);
    axis.resolved = std::nullopt;
    axis.pendingEval = nullptr;
}

void ScriptProject::sortRegions() {
    std::ranges::sort(regions, [](const ProcessingRegion &a, const ProcessingRegion &b) {
        if (a.startTime != b.startTime)
            return a.startTime < b.startTime;
        return a.axisRoles.count() > b.axisRoles.count();
    });
}

ProcessingRegion *ScriptProject::findRegion(int id) {
    for (auto &r : regions)
        if (r.id == id)
            return &r;
    return nullptr;
}

const ProcessingRegion *ScriptProject::findRegion(int id) const {
    for (const auto &r : regions)
        if (r.id == id)
            return &r;
    return nullptr;
}

} // namespace ofs
