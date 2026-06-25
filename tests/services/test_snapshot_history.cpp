#include "Services/SnapshotHistory.h"
#include <doctest/doctest.h>
#include <vector>

// SnapshotHistory bounds the undo/redo history by total bytes, not entry count: a new edit grows the undo
// arena, and when it crosses the budget the OLDEST steps are evicted while the newest is always kept.
// These tests pin that policy. The blobs are synthetic (the store treats them as opaque bytes), so no
// codec is involved.

using ofs::PackedSnapshot;
using ofs::SnapshotHistory;

namespace {

// A blob of `size` bytes tagged with `id` in byte 0, so a popped snapshot can be identified.
PackedSnapshot blob(uint8_t id, size_t size) {
    PackedSnapshot p;
    p.bytes.assign(size, id);
    if (!p.bytes.empty())
        p.bytes[0] = id;
    return p;
}

uint8_t idOf(const PackedSnapshot &p) {
    return p.bytes.empty() ? 0 : p.bytes[0];
}

// Drain the undo stack newest-first into a list of ids.
std::vector<uint8_t> drainUndo(SnapshotHistory &h) {
    std::vector<uint8_t> ids;
    while (h.canUndo()) {
        ids.push_back(idOf(h.undoBack()));
        h.popUndo();
    }
    return ids;
}

} // namespace

TEST_CASE("SnapshotHistory: evicts oldest undo entries to stay within budget") {
    SnapshotHistory h(1000);
    for (uint8_t i = 0; i < 10; ++i) {
        h.pushUndo(blob(i, 200));
        CHECK(h.usedBytes() <= 1000); // never exceeds the budget while pushing equal-size entries
    }
    // 1000 / 200 = 5 entries fit; ids 0..4 were evicted, 5..9 survive, newest (9) on top.
    CHECK(h.undoCount() == 5);
    CHECK(drainUndo(h) == std::vector<uint8_t>{9, 8, 7, 6, 5});
}

TEST_CASE("SnapshotHistory: keeps the newest entry even if it alone exceeds the budget") {
    SnapshotHistory h(100);
    h.pushUndo(blob(1, 50));
    h.pushUndo(blob(2, 500)); // far over budget, but it's the most recent edit
    REQUIRE(h.undoCount() == 1);
    CHECK(idOf(h.undoBack()) == 2);
    CHECK(h.usedBytes() == 500); // budget is a target; the last edit stays undoable regardless
}

TEST_CASE("SnapshotHistory: a new edit clears redo and frees its memory") {
    SnapshotHistory h(10'000);
    h.pushUndo(blob(1, 100));
    h.pushUndo(blob(2, 100));
    // Simulate one undo: pop the target, push current state to redo.
    h.popUndo();
    h.pushRedo(blob(9, 100));
    REQUIRE(h.canRedo());

    h.clearRedo();
    CHECK_FALSE(h.canRedo());
    CHECK(h.redoCount() == 0);
}

TEST_CASE("SnapshotHistory: undo/redo navigation preserves order and stays within budget") {
    SnapshotHistory h(1000);
    for (uint8_t i = 1; i <= 4; ++i)
        h.pushUndo(blob(i, 100)); // 400 bytes, well under budget — nothing evicted

    // Walk back twice (undo): pop undo target, push current onto redo.
    h.pushRedo(h.undoBack()); // stand-in for "current state"; id 4
    h.popUndo();              // undo top now 3
    CHECK(idOf(h.redoBack()) == 4);
    CHECK(h.usedBytes() <= 1000);

    // Walk forward (redo): pop redo target, push onto undo.
    const PackedSnapshot target = h.redoBack();
    h.popRedo();
    h.pushUndo(target);
    CHECK(idOf(h.undoBack()) == 4);
    CHECK(h.redoCount() == 0);
}

TEST_CASE("SnapshotHistory: setMaxBytes trims the history immediately") {
    SnapshotHistory h(2000);
    for (uint8_t i = 1; i <= 8; ++i)
        h.pushUndo(blob(i, 200)); // 1600 bytes, all fit under 2000
    REQUIRE(h.undoCount() == 8);

    h.setMaxBytes(500); // 500 / 200 = 2 newest entries survive (ids 7, 8)
    CHECK(h.undoCount() == 2);
    CHECK(h.usedBytes() <= 500);
    CHECK(drainUndo(h) == std::vector<uint8_t>{8, 7});
}

TEST_CASE("SnapshotHistory: raising the limit drops nothing and preserves order") {
    SnapshotHistory h(1000);
    for (uint8_t i = 1; i <= 4; ++i)
        h.pushUndo(blob(i, 200)); // 800 bytes used, well under 1000
    // Walk one step back so there is also a redo entry to protect (id 4 moves to redo).
    const PackedSnapshot target = h.undoBack();
    h.popUndo();
    h.pushRedo(target);
    REQUIRE(h.undoCount() == 3);
    REQUIRE(h.redoCount() == 1);
    const size_t before = h.usedBytes();

    h.setMaxBytes(1U << 30); // raise far above current usage

    // Nothing evicted on either side; counts, bytes, and ordering are untouched.
    CHECK(h.maxBytes() == (1U << 30));
    CHECK(h.usedBytes() == before);
    CHECK(h.undoCount() == 3);
    CHECK(h.redoCount() == 1);
    CHECK(idOf(h.redoBack()) == 4);
    CHECK(drainUndo(h) == std::vector<uint8_t>{3, 2, 1}); // newest-first, oldest survives
}

TEST_CASE("SnapshotHistory: clear empties both stacks") {
    SnapshotHistory h(1000);
    h.pushUndo(blob(1, 100));
    h.pushRedo(blob(2, 100));
    h.clear();
    CHECK_FALSE(h.canUndo());
    CHECK_FALSE(h.canRedo());
    CHECK(h.usedBytes() == 0);
}
