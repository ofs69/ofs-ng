#pragma once

#include "Core/BookmarkChapterState.h"
#include "Core/ProcessingRegion.h"
#include "Core/ScriptAxisAction.h"
#include "Core/StandardAxis.h"
#include "Core/VectorSet.h"
#include <array>
#include <cstdint>
#include <vector>

namespace ofs {

// A point-in-time copy of every undoable part of the document: per-axis actions/selection/flags, the
// region list, and the bookmarks/chapters, with the navigation state (active axis, cursor, selected region)
// restored as a convenience on undo. Metadata stays out of undo by design (see UndoSystem). Produced by
// UndoSystem::takeSnapshot and consumed by restoreSnapshot. Kept in this dedicated header so the snapshot
// codec (SnapshotCodec) and store (SnapshotHistory) can use it without pulling in UndoSystem.
struct ProjectSnapshot {
    StandardAxis activeAxis = StandardAxis::Count;
    double position = 0.0;
    int procSelRegionId = -1;

    struct AxisEntry {
        bool isVisible = true;
        bool showInStrip = true;
        bool isLocked = false;
        bool dirty = false;
        VectorSet<ScriptAxisAction> actions;
        VectorSet<ScriptAxisAction> selection;
    };

    std::array<AxisEntry, kStandardAxisCount> axes{};
    std::vector<ProcessingRegion> regions;
    // Bookmarks + chapters, both kept sorted by time as the live state is.
    BookmarkChapterState bookmarks;
};

// A ProjectSnapshot serialized to a flat bitsery buffer (no compression). This is what the undo/redo
// history actually stores: snapshots are mostly-redundant full document copies (a typical edit touches one
// of 20 axes), so the packed form keeps history RAM in check by encoding each record tightly — pos in one
// byte, selection as delta indices into actions. See SnapshotCodec for the pack/unpack functions.
struct PackedSnapshot {
    // The raw bitsery buffer, used verbatim — no header or framing. See packSnapshot/unpackSnapshot.
    std::vector<uint8_t> bytes;
    [[nodiscard]] size_t sizeBytes() const { return bytes.size(); }
};

} // namespace ofs
