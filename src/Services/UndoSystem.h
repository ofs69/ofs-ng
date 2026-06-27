#pragma once

#include "Core/Events.h"
#include "Core/ScriptProject.h"
#include "Core/StandardAxis.h"
#include "Services/ProjectSnapshot.h"
#include "Services/SnapshotCodec.h"
#include "Services/SnapshotHistory.h"
#include <cstddef>
#include <optional>

namespace ofs {

class EventQueue;

// Default undo memory budget when none is supplied (e.g. in tests). The shipped app passes the
// user-configured AppSettings::undoMemoryLimitMb here. See SnapshotHistory for the budgeting policy.
inline constexpr size_t kDefaultUndoMemoryBytes = static_cast<size_t>(256) << 20; // 256 MB

class UndoSystem {
  public:
    explicit UndoSystem(ScriptProject &project, EventQueue &eq, size_t maxBytes = kDefaultUndoMemoryBytes);

    [[nodiscard]] bool canUndo() const { return history.canUndo(); }
    [[nodiscard]] bool canRedo() const { return history.canRedo(); }

    // Live undo/redo memory telemetry (bitsery-packed bytes currently held, and the budget). Surfaced
    // in the footer.
    [[nodiscard]] size_t memoryUsedBytes() const { return history.usedBytes(); }
    [[nodiscard]] size_t memoryMaxBytes() const { return history.maxBytes(); }

    // Live stack-depth telemetry (number of steps you can undo / redo). Surfaced next to the memory
    // readout so the footer shows both how much the history holds and how far it reaches.
    [[nodiscard]] size_t undoStepCount() const { return history.undoCount(); }
    [[nodiscard]] size_t redoStepCount() const { return history.redoCount(); }

    void push();

    // Resize the memory budget at runtime (e.g. the user changed the preference). Immediately trims the
    // history if the new limit is smaller.
    void setMaxBytes(size_t maxBytes) { history.setMaxBytes(maxBytes); }

    // Frame-loop hook: commit (or discard) any tentative snapshot captured during this frame's
    // drain. Called once per frame after EventQueue::drain(), so a successful discrete edit becomes
    // undoable the same frame and a no-op edit (e.g. a region create that found no room) leaves no
    // step. Exposed for tests that drive the queue manually without OfsApp's frame loop.
    void endFrame() { flushTentative(); }

  private:
    void undo();
    void redo();
    void clear();

    [[nodiscard]] ProjectSnapshot takeSnapshot() const;
    void restoreSnapshot(const ProjectSnapshot &snapshot);

    // The undo model for a *discrete* edit (one that mutates within the same drain it is requested,
    // as opposed to a multi-frame drag gesture) is "snapshot the pre-edit state, keep it only if the
    // edit actually changed the document". captureTentative() takes the pre-edit snapshot;
    // flushTentative() commits it iff the document differs now, otherwise drops it. This is what keeps
    // a failed/no-op edit from recording an empty undo step or wiping the redo stack.
    void captureTentative();
    void flushTentative();
    [[nodiscard]] bool documentChangedSince(const ProjectSnapshot &snapshot) const;

    ScriptProject &project;
    EventQueue &eq;

    // Memory-bounded undo/redo store. Each committed/navigated snapshot is packed (SnapshotCodec) before
    // it lands here; the live tentative snapshot below stays uncompressed since it is compared against the
    // document every frame.
    SnapshotHistory history;

    // Owns the pack/unpack scratch buffers, reused across every push/undo/redo so a burst of edits does no
    // per-operation intermediate allocation. Main-thread-only, like the rest of UndoSystem.
    SnapshotCodec codec;

    // Pre-edit snapshot for the in-flight discrete edit, pending the document-changed check above.
    std::optional<ProjectSnapshot> tentativeSnapshot_;
};

} // namespace ofs
