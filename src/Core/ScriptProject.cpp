#include "Core/ScriptProject.h"
#include "Core/EventQueue.h"
#include "Core/Events.h"
#include <algorithm>
#include <ranges>

namespace ofs {

void ScriptProject::mutate(StandardAxis role, const std::function<void(AxisState &)> &fn, EventQueue &eq) {
    auto &axis = axes[static_cast<size_t>(role)];
    fn(axis);
    // Sync selection: remove any selected actions that no longer exist in the axis.
    if (!axis.selection.empty()) {
        auto view = axis.selection |
                    std::views::filter([&axis](const auto &entry) { return axis.actions.contains(entry); }) |
                    std::views::transform([&axis](const auto &entry) { return *axis.actions.find(entry); });
        axis.selection = VectorSet<ScriptAxisAction>(view.begin(), view.end());
    }
    axis.dirty = true;
    eq.push(AxisModifiedEvent{role});
}

void ScriptProject::setSelection(StandardAxis role, VectorSet<ScriptAxisAction> newSel, EventQueue &eq) {
    auto &axis = axes[static_cast<size_t>(role)];
    if (newSel.empty()) {
        axis.selection.clear();
    } else {
        auto view = newSel | std::views::filter([&axis](const auto &e) { return axis.actions.contains(e); }) |
                    std::views::transform([&axis](const auto &e) { return *axis.actions.find(e); });
        axis.selection = VectorSet<ScriptAxisAction>(view.begin(), view.end());
    }
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

    // SimulatorState is split: the 3D ranges, feature toggles and transient posing fields are
    // app-level (seeded from AppSettings at startup, persisted back at exit) and must survive a
    // project close. Only the widget placement is per-project document state (saved/loaded as
    // simP1/simP2/sim3dPos/sim3dSize), so reset just those — keep this in sync with
    // ProjectManager::saveToProject / loadFromProject.
    const SimulatorState defaults;
    simulator.p1 = defaults.p1;
    simulator.p2 = defaults.p2;
    simulator.sim3dPos = defaults.sim3dPos;
    simulator.sim3dSize = defaults.sim3dSize;
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
