#pragma once

#include "Services/ProjectSnapshot.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ofs {

// The undo/redo store, bounded by total memory rather than a fixed entry count. Each side (undo, redo) is
// a LIFO stack of packed snapshots kept in ONE contiguous byte arena — the snapshots are appended
// back-to-back and a parallel size index marks the boundaries — so the whole history is a single
// std::vector<uint8_t> per side that grows as you edit and shrinks as entries are popped or evicted.
//
// The cap is a byte budget over both arenas combined. New edits only ever grow the undo side (a new edit
// clears redo), so the budget is enforced there: when an undo push pushes the total over budget, the
// OLDEST undo entries (front) are dropped until it fits — you lose the ability to undo far-back edits, not
// recent ones. The newest undo entry is never evicted, so the last edit is always undoable even if it
// alone exceeds the budget. undo/redo navigation just moves a snapshot between the two stacks, so it keeps
// the total roughly constant and needs no eviction (callers pop the target before pushing the inverse).
class SnapshotHistory {
  public:
    explicit SnapshotHistory(size_t maxBytes) : maxBytes_(maxBytes) {}

    void setMaxBytes(size_t maxBytes) {
        maxBytes_ = maxBytes;
        enforceBudget();
    }
    [[nodiscard]] size_t maxBytes() const { return maxBytes_; }
    [[nodiscard]] size_t usedBytes() const { return undo_.bytes() + redo_.bytes(); }

    [[nodiscard]] bool canUndo() const { return !undo_.empty(); }
    [[nodiscard]] bool canRedo() const { return !redo_.empty(); }
    [[nodiscard]] size_t undoCount() const { return undo_.count(); }
    [[nodiscard]] size_t redoCount() const { return redo_.count(); }

    // Append to the undo stack, then trim the oldest undo entries back under budget.
    void pushUndo(const PackedSnapshot &snap) {
        undo_.pushBack(snap);
        enforceBudget();
    }
    // Append to the redo stack. No eviction: redo only grows while undo shrinks (the caller pops the undo
    // target first), so the total stays within budget, and evicting a redo entry would drop a reachable
    // future state. Redo is bounded instead by clearRedo() on every new edit.
    void pushRedo(const PackedSnapshot &snap) { redo_.pushBack(snap); }

    [[nodiscard]] PackedSnapshot undoBack() const { return undo_.back(); }
    [[nodiscard]] PackedSnapshot redoBack() const { return redo_.back(); }
    void popUndo() { undo_.popBack(); }
    void popRedo() { redo_.popBack(); }

    void clearRedo() { redo_.clear(); }
    void clear() {
        undo_.clear();
        redo_.clear();
    }

  private:
    // One snapshot stack as a single contiguous byte arena plus a per-entry size index. back()/popBack()
    // act on the newest entry (arena tail); popFront() drops the oldest (arena head) for eviction.
    class Arena {
      public:
        [[nodiscard]] bool empty() const { return sizes_.empty(); }
        [[nodiscard]] size_t count() const { return sizes_.size(); }
        [[nodiscard]] size_t bytes() const { return arena_.size(); }

        void pushBack(const PackedSnapshot &snap) {
            arena_.insert(arena_.end(), snap.bytes.begin(), snap.bytes.end());
            sizes_.push_back(snap.bytes.size());
        }

        [[nodiscard]] PackedSnapshot back() const {
            PackedSnapshot out;
            const size_t n = sizes_.back();
            out.bytes.assign(arena_.end() - static_cast<std::ptrdiff_t>(n), arena_.end());
            return out;
        }

        void popBack() {
            arena_.resize(arena_.size() - sizes_.back());
            sizes_.pop_back();
        }

        void popFront() {
            arena_.erase(arena_.begin(), arena_.begin() + static_cast<std::ptrdiff_t>(sizes_.front()));
            sizes_.erase(sizes_.begin());
        }

        void clear() {
            arena_.clear();
            arena_.shrink_to_fit(); // actually return the RAM, not just reset the logical size
            sizes_.clear();
            sizes_.shrink_to_fit();
        }

        // Return capacity to the OS when the arena holds far less than it reserved (after a run of
        // evictions/pops). Vector growth otherwise leaves capacity near the historical peak, so without
        // this the process keeps ~peak RAM even after the history shrinks well below the budget.
        void reclaimIfSlack() {
            if (arena_.capacity() > 2 * arena_.size() + 64)
                arena_.shrink_to_fit();
        }

      private:
        std::vector<uint8_t> arena_;
        std::vector<size_t> sizes_;
    };

    void enforceBudget() {
        // Drop oldest undo history first (keeping the newest entry), then, only if still over budget,
        // oldest redo entries. Pure navigation never reaches here; this fires on new edits and on a
        // lowered limit.
        while (usedBytes() > maxBytes_ && undo_.count() > 1)
            undo_.popFront();
        while (usedBytes() > maxBytes_ && !redo_.empty())
            redo_.popFront();
        undo_.reclaimIfSlack();
        redo_.reclaimIfSlack();
    }

    Arena undo_;
    Arena redo_;
    size_t maxBytes_;
};

} // namespace ofs
