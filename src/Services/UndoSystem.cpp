#include "UndoSystem.h"
#include "Core/BookmarkChapterState.h"
#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/GraphPresetEvents.h"
#include "Core/ProjectLifecycleEvents.h"
#include "Core/ScriptProject.h"
#include "Services/SnapshotCodec.h"
#include "Util/Benchmark.h"
#include "Util/Log.h"

namespace ofs {

UndoSystem::UndoSystem(ScriptProject &project, EventQueue &eq, size_t maxBytes)
    : project(project), eq(eq), history(maxBytes) {
    eq.on<UndoEvent>([this](const UndoEvent &) { undo(); });
    eq.on<RedoEvent>([this](const RedoEvent &) { redo(); });
    eq.on<LoadProjectEvent>([this](const LoadProjectEvent &) { clear(); });

    // ── Discrete edits ────────────────────────────────────────────────────────────────────────
    // Each of these mutates ProjectManager state within the same drain it is requested. Several can
    // legitimately no-op (a create that finds no room, a delete of a missing region, a remove with no
    // point under the playhead, a paste from an empty clipboard, …). They capture a tentative pre-edit
    // snapshot here (UndoSystem registers before ProjectManager, so it is the true pre-edit state);
    // flushTentative() — run once per frame and before undo/redo — commits it only if the edit actually
    // changed the document. A no-op therefore records no undo step and leaves the redo stack intact.
    // snapshot=false marks the 2nd..Nth mutation of a single edit-mode gesture (e.g. Shaped Approach, or
    // a Replace that mixes adds/removes): skip the recapture so the batch keeps the one tentative snapshot
    // the first mutation took, coalescing the whole gesture into a single undo step. flushTentative() at
    // frame end commits it once. Add/Remove/Paste all honor the flag so a heterogeneous Replace batch
    // coalesces too; a standalone push leaves snapshot=true (its default) and snapshots on its own.
    eq.on<AddActionAtTimeEvent>([this](const AddActionAtTimeEvent &e) {
        if (e.snapshot)
            captureTentative();
    });
    eq.on<RemoveSelectedActionsEvent>([this](const RemoveSelectedActionsEvent &e) {
        if (e.snapshot)
            captureTentative();
    });
    eq.on<RemoveActionAtTimeEvent>([this](const RemoveActionAtTimeEvent &e) {
        if (e.snapshot)
            captureTentative();
    });
    eq.on<PasteActionsEvent>([this](const PasteActionsEvent &e) {
        if (e.snapshot)
            captureTentative();
    });
    eq.on<MoveActionToCurrentTimeEvent>([this](const MoveActionToCurrentTimeEvent &) { captureTentative(); });
    eq.on<AddScratchAxisEvent>([this](const AddScratchAxisEvent &) { captureTentative(); });
    eq.on<ShowMultiAxisEvent>([this](const ShowMultiAxisEvent &) { captureTentative(); });
    eq.on<ShowL0OnlyEvent>([this](const ShowL0OnlyEvent &) { captureTentative(); });
    eq.on<RemoveAxisEvent>([this](const RemoveAxisEvent &) { captureTentative(); });
    eq.on<ToggleAxisPanelVisibilityEvent>([this](const ToggleAxisPanelVisibilityEvent &) { captureTentative(); });
    eq.on<CreateRegionEvent>([this](const CreateRegionEvent &) { captureTentative(); });
    eq.on<DeleteRegionEvent>([this](const DeleteRegionEvent &) { captureTentative(); });
    eq.on<BakeRegionEvent>([this](const BakeRegionEvent &) { captureTentative(); });
    eq.on<AssignAxisToRegionEvent>([this](const AssignAxisToRegionEvent &) { captureTentative(); });
    eq.on<LoadGraphEvent>([this](const LoadGraphEvent &) { captureTentative(); });
    eq.on<ApplyGraphRemapEvent>([this](const ApplyGraphRemapEvent &) { captureTentative(); });
    eq.on<CommitAxisActionsEvent>([this](const CommitAxisActionsEvent &) { captureTentative(); });
    eq.on<ImportFunscriptDataEvent>([this](const ImportFunscriptDataEvent &) { captureTentative(); });
    // Bookmark and chapter edits (add/remove, rename, recolor, move/resize-on-release) all arrive as
    // ModifyBookmarkChapterEvent; snapshot the pre-edit bookmarks+chapters so each is undoable. A
    // continuous gesture (color-picker drag, name typing) pushes a live mutation every frame but only
    // sets snapshot=true on the first; honor the flag so the whole gesture coalesces into one undo step
    // (mirroring ModifyRegionEvent), instead of one snapshot per frame.
    eq.on<ModifyBookmarkChapterEvent>([this](const ModifyBookmarkChapterEvent &e) {
        if (e.snapshot)
            captureTentative();
    });

    // ── Multi-frame drag gestures ─────────────────────────────────────────────────────────────
    // These differ from the discrete edits above: the snapshot=true fire carries the still-unmodified
    // state, and the actual mutation arrives in *later* frames (drag) or later fires of the same hold.
    // A same-frame document-changed check would see no change yet and wrongly drop the step, so these
    // push eagerly on gesture start and coalesce the rest of the gesture via the snapshot flag.
    eq.on<MoveSelectionPositionEvent>([this](const MoveSelectionPositionEvent &e) {
        if (e.snapshot)
            push();
    });
    eq.on<MoveSelectionTimeEvent>([this](const MoveSelectionTimeEvent &e) {
        if (e.snapshot)
            push();
    });
    eq.on<MoveActionEvent>([this](const MoveActionEvent &e) {
        if (e.snapshot)
            push();
    });
    eq.on<ModifyRegionEvent>([this](const ModifyRegionEvent &e) {
        if (e.snapshot)
            push();
    });
}

void UndoSystem::clear() {
    history.clear();
    tentativeSnapshot_.reset();
    OFS_CORE_INFO("Undo state cleared. Undo: {}, Redo: {}", history.undoCount(), history.redoCount());
}

void UndoSystem::captureTentative() {
    // A second discrete edit before this frame's flush (rare — two edits in one drain): commit the
    // first now so its pre-edit snapshot isn't lost, then snapshot the state the new edit will mutate.
    flushTentative();
    tentativeSnapshot_ = takeSnapshot();
}

void UndoSystem::flushTentative() {
    if (!tentativeSnapshot_)
        return;
    if (documentChangedSince(*tentativeSnapshot_)) {
        // New edit: clear redo first so the budget check in pushUndo accounts only for the undo side.
        history.clearRedo();
        const util::Stopwatch sw;
        history.pushUndo(codec.pack(*tentativeSnapshot_));
        OFS_CORE_INFO("Undo state pushed. Undo: {}, Redo: {}, Mem: {} KB, packed in {:.3f} ms", history.undoCount(),
                      history.redoCount(), history.usedBytes() / 1024, sw.elapsedMs());
    }
    tentativeSnapshot_.reset();
}

bool UndoSystem::documentChangedSince(const ProjectSnapshot &snapshot) const {
    // Compare only undoable document content. activeAxis/procSelRegionId/cursor position are
    // navigation state (restored as a convenience on undo, never the sole effect of an edit), and
    // `dirty` is transient — including them would let an incidental same-frame seek/selection record
    // an empty undo step. Every field an edit can actually change is covered here, so a real edit is
    // never missed (a missed edit would silently lose its undo step).
    if (project.regions != snapshot.regions)
        return true;
    // Bookmark/chapter add/remove/move/resize/rename/recolor each change a count or one of these fields. A
    // chapter's sceneView is captured-on-adjust outside undo and not snapshotted at all (see restoreSnapshot
    // / SnapshotCodec), so it neither triggers a step here nor is compared.
    const auto &curBm = project.bookmarks.bookmarks;
    const auto &snapBm = snapshot.bookmarks.bookmarks;
    if (curBm.size() != snapBm.size())
        return true;
    for (size_t i = 0; i < snapBm.size(); ++i) {
        if (curBm[i].time != snapBm[i].time || curBm[i].name != snapBm[i].name)
            return true;
    }
    const auto &curCh = project.bookmarks.chapters;
    const auto &snapCh = snapshot.bookmarks.chapters;
    if (curCh.size() != snapCh.size())
        return true;
    for (size_t i = 0; i < snapCh.size(); ++i) {
        const auto &cur = curCh[i];
        const auto &e = snapCh[i];
        if (cur.startTime != e.startTime || cur.endTime != e.endTime || cur.name != e.name || cur.color != e.color)
            return true;
    }
    for (size_t i = 0; i < kStandardAxisCount; ++i) {
        const auto &cur = project.axes[i];
        const auto &e = snapshot.axes[i];
        if (cur.isVisible != e.isVisible || cur.showInStrip != e.showInStrip || cur.isLocked != e.isLocked ||
            cur.actions != e.actions || cur.selection != e.selection)
            return true;
    }
    return false;
}

ProjectSnapshot UndoSystem::takeSnapshot() const {
    ProjectSnapshot snap;
    snap.activeAxis = project.state.activeAxis;
    snap.position = project.playback.cursorPos;
    snap.procSelRegionId = project.procSelRegionId;
    snap.regions = project.regions;
    snap.bookmarks = project.bookmarks;

    for (size_t i = 0; i < kStandardAxisCount; ++i) {
        const auto &axis = project.axes[i];
        snap.axes[i] = {.isVisible = axis.isVisible,
                        .showInStrip = axis.showInStrip,
                        .isLocked = axis.isLocked,
                        .dirty = axis.dirty,
                        .actions = axis.actions,
                        .selection = axis.selection};
    }
    return snap;
}

void UndoSystem::restoreSnapshot(const ProjectSnapshot &snapshot) {
    // A region edit (create/delete/node-graph change) alters a region's contribution to its
    // axes without touching those axes' source actions, so the per-axis `changed` check below
    // won't catch it. ProcessingSystem only re-evaluates on AxisModifiedEvent, so gather the
    // axes whose regions differ (in either direction) and announce them too — otherwise the
    // resolved output goes stale after undo/redo. Computed before regions are overwritten.
    AxisRoles regionDirtyAxes;
    if (project.regions != snapshot.regions) {
        for (const auto &r : project.regions)
            regionDirtyAxes |= r.axisRoles;
        for (const auto &r : snapshot.regions)
            regionDirtyAxes |= r.axisRoles;
    }

    project.regions = snapshot.regions;
    project.procSelRegionId = snapshot.procSelRegionId;
    // Bookmarks/chapters are plain document state the UI (band/bookmark bar) and scene-view resolution read
    // fresh each frame, so a direct overwrite suffices — no event or re-eval needed. One exception: a
    // chapter's sceneView is captured-on-adjust outside undo and intentionally not snapshotted (see
    // SnapshotCodec serialize(Chapter)), so carry the live framing across the overwrite instead of
    // reverting it. Matched by index — exact when the chapter set is unchanged (the common case: an
    // unrelated edit being undone); a chapter add/remove shifts later views, an accepted limitation since
    // framing is not itself undoable.
    std::vector<std::optional<SceneView>> liveSceneViews;
    liveSceneViews.reserve(project.bookmarks.chapters.size());
    for (const auto &ch : project.bookmarks.chapters)
        liveSceneViews.push_back(ch.sceneView);
    project.bookmarks = snapshot.bookmarks;
    for (size_t i = 0; i < project.bookmarks.chapters.size() && i < liveSceneViews.size(); ++i)
        project.bookmarks.chapters[i].sceneView = liveSceneViews[i];

    const bool activeAxisChanged = project.state.activeAxis != snapshot.activeAxis;
    project.state.activeAxis = snapshot.activeAxis;

    for (size_t i = 0; i < kStandardAxisCount; ++i) {
        const auto &e = snapshot.axes[i];
        auto &cur = project.axes[i];
        bool changed = cur.actions != e.actions || cur.selection != e.selection || cur.showInStrip != e.showInStrip;
        const bool reeval = changed || regionDirtyAxes.test(i);

        // restoreAxis unconditionally clears `resolved`/`pendingEval`. For an axis we are *not* about to
        // re-evaluate (no action/region change), its prior evaluation is still valid — preserve it across
        // the restore. Otherwise a no-op gesture (e.g. dragging a region and releasing it in place) would
        // strand the axis with an empty resolved output and no recompute scheduled, forcing a manual Recompute.
        std::optional<ResolvedActions> keepResolved;
        if (!reeval)
            keepResolved = std::move(cur.resolved);

        project.restoreAxis(static_cast<StandardAxis>(i), e.isVisible, e.showInStrip, e.isLocked, e.dirty, e.actions,
                            e.selection);
        if (reeval)
            eq.push(AxisModifiedEvent{static_cast<StandardAxis>(i)});
        else
            cur.resolved = std::move(keepResolved);
    }

    // Notify consumers (plugin onAxisSelected, UI) that the active axis was restored.
    if (activeAxisChanged)
        eq.push(AxisSelectedEvent{snapshot.activeAxis});

    eq.push(SeekEvent{snapshot.position});
}

void UndoSystem::push() {
    // A gesture starting mid-frame must land after any discrete edit captured earlier this frame, so
    // commit the pending tentative first to keep stack order matching edit order.
    flushTentative();
    history.clearRedo();
    const util::Stopwatch sw;
    history.pushUndo(codec.pack(takeSnapshot()));
    OFS_CORE_INFO("Undo state pushed. Undo: {}, Redo: {}, Mem: {} KB, packed in {:.3f} ms", history.undoCount(),
                  history.redoCount(), history.usedBytes() / 1024, sw.elapsedMs());
}

void UndoSystem::undo() {
    flushTentative(); // commit any just-made discrete edit before walking the stack
    if (!canUndo())
        return;
    // Pop the undo target before pushing the current state onto redo: the two moves net to zero, so the
    // combined memory never transiently exceeds the budget. takeSnapshot() reads the still-current
    // document (popUndo only touches the history), so it must run before restoreSnapshot overwrites it.
    const util::Stopwatch sw;
    const ProjectSnapshot restored = codec.unpack(history.undoBack());
    history.popUndo();
    history.pushRedo(codec.pack(takeSnapshot()));
    restoreSnapshot(restored);
    OFS_CORE_INFO("Undo performed. Undo: {}, Redo: {}, in {:.3f} ms", history.undoCount(), history.redoCount(),
                  sw.elapsedMs());
}

void UndoSystem::redo() {
    flushTentative();
    if (!canRedo())
        return;
    const util::Stopwatch sw;
    const ProjectSnapshot restored = codec.unpack(history.redoBack());
    history.popRedo();
    history.pushUndo(codec.pack(takeSnapshot()));
    restoreSnapshot(restored);
    OFS_CORE_INFO("Redo performed. Undo: {}, Redo: {}, in {:.3f} ms", history.undoCount(), history.redoCount(),
                  sw.elapsedMs());
}

} // namespace ofs
