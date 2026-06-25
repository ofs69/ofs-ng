#include "Core/ProcessingRegion.h"
#include "Core/ScriptAxisAction.h"
#include "Core/StandardAxis.h"
#include "Services/ProjectSnapshot.h"
#include "Services/SnapshotCodec.h"
#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

// The undo/redo stacks store every snapshot bitsery-serialized into a flat buffer (see UndoSystem.cpp).
// The codec must be transparent: unpackSnapshot(packSnapshot(x)) == x for every field undo restores.
// This is the guard the maintenance note in the codec points at — a struct field added without a matching
// serialize line silently fails to round-trip here. Build a maximally-populated snapshot and check it back.

using ofs::PackedSnapshot;
using ofs::ProcessingRegion;
using ofs::ProjectSnapshot;
using ofs::ScriptAxisAction;
using ofs::StandardAxis;

namespace {

ProjectSnapshot makeFullSnapshot() {
    ProjectSnapshot snap;
    snap.activeAxis = StandardAxis::R1;
    snap.position = 12.5;
    snap.procSelRegionId = 7;

    // An L0 axis with non-default flags, actions, and a partial selection.
    auto &l0 = snap.axes[static_cast<size_t>(StandardAxis::L0)];
    l0.isVisible = false;
    l0.showInStrip = true;
    l0.isLocked = true;
    l0.dirty = true;
    l0.actions.insert({1.0, 50});
    l0.actions.insert({2.5, 80});
    l0.actions.insert({3.75, 0});
    l0.selection.insert({2.5, 80});

    // A scratch axis, present but otherwise default — exercises a second array slot.
    auto &s3 = snap.axes[static_cast<size_t>(StandardAxis::S3)];
    s3.showInStrip = true;
    s3.actions.insert({0.0, 0});
    s3.actions.insert({100.0, 100});

    // A region whose node graph touches every serialized node-type field.
    ProcessingRegion r;
    r.id = 42;
    r.startTime = 1.0;
    r.endTime = 9.0;
    r.name = "Région ☆ 日本語"; // non-ASCII: text1b must carry raw UTF-8 bytes
    r.color = IM_COL32(10, 20, 30, 200);
    r.hz = 60;
    r.showSourceActions = false;
    r.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    r.axisRoles.set(static_cast<size_t>(StandardAxis::S3));
    r.axisRoleTags = {"L0", "S3"};

    ofs::ProcessingGraphNode effect;
    effect.id = 1;
    effect.type = ofs::GraphNodeType::Effect;
    effect.effect.type = "invert";
    effect.effect.params = {0.25f, 1.0f, -3.5f};
    effect.posX = 50.0f;
    effect.posY = 100.0f;
    effect.role = StandardAxis::L0;

    ofs::ProcessingGraphNode plugin;
    plugin.id = 2;
    plugin.type = ofs::GraphNodeType::PluginNode;
    plugin.pluginInputCount = 3;
    plugin.pluginSignal = 1;
    plugin.pluginNodeId = "acme.warp";
    plugin.nodeState = R"({"Mix":0.9,"Note":"héllo"})";
    plugin.posX = 250.0f;

    ofs::ProcessingGraphNode script;
    script.id = 3;
    script.type = ofs::GraphNodeType::Script;
    script.scriptFile = "smooth.cs";
    script.scriptSignal = 2;
    script.scriptInputCount = 2;
    script.scriptWatch = true; // transient, but snapshot copies it — must survive the round-trip
    script.scriptEmbeddedSource = "// !ofs:param Mix\nreturn x * 0.5;";

    r.nodeGraph.nodes = {effect, plugin, script};
    r.nodeGraph.links = {{10, 1, 3, 0}, {11, 3, 2, 1}};
    r.nodeGraph.nextId = 12;

    snap.regions.push_back(std::move(r));

    // A chapter carrying a scene view (sceneView is captured-on-adjust outside undo and intentionally NOT
    // snapshotted — see SnapshotCodec serialize(Chapter)); the round-trip must drop it, restoring only
    // size/position/name/color. A second chapter exercises a plain (nullopt) sceneView.
    ofs::Chapter ch0;
    ch0.startTime = 2.0;
    ch0.endTime = 6.0;
    ch0.name = "Chapitre ☆ 日本語"; // non-ASCII: text1b must carry raw UTF-8 bytes
    ch0.color = IM_COL32(200, 30, 90, 255);
    ofs::SceneView sv;
    sv.framing.zoomFactor = 2.5f;
    sv.inverted = true;
    ch0.sceneView = sv;
    snap.bookmarks.chapters.push_back(std::move(ch0));

    ofs::Chapter ch1;
    ch1.startTime = 10.0;
    ch1.endTime = 14.0;
    ch1.name = "plain";
    snap.bookmarks.chapters.push_back(std::move(ch1));

    // Bookmarks ride in the same snapshot (one with a non-ASCII name, one unnamed).
    snap.bookmarks.bookmarks.push_back({.time = 3.5, .name = "repère ☆"});
    snap.bookmarks.bookmarks.push_back({.time = 11.0, .name = ""});
    return snap;
}

void checkChapterEqual(const ofs::Chapter &a, const ofs::Chapter &b) {
    CHECK(a.startTime == doctest::Approx(b.startTime));
    CHECK(a.endTime == doctest::Approx(b.endTime));
    CHECK(a.name == b.name);
    CHECK(a.color == b.color);
    // sceneView is intentionally excluded from the undo snapshot (captured-on-adjust, preserved live by
    // UndoSystem::restoreSnapshot, never serialized), so it must come back empty regardless of the source.
    CHECK_FALSE(a.sceneView.has_value());
}

void checkBookmarksEqual(const ofs::BookmarkChapterState &a, const ofs::BookmarkChapterState &b) {
    REQUIRE(a.bookmarks.size() == b.bookmarks.size());
    for (size_t i = 0; i < a.bookmarks.size(); ++i) {
        CHECK(a.bookmarks[i].time == doctest::Approx(b.bookmarks[i].time));
        CHECK(a.bookmarks[i].name == b.bookmarks[i].name);
    }
    REQUIRE(a.chapters.size() == b.chapters.size());
    for (size_t i = 0; i < a.chapters.size(); ++i)
        checkChapterEqual(a.chapters[i], b.chapters[i]);
}

void checkAxisEntryEqual(const ProjectSnapshot::AxisEntry &a, const ProjectSnapshot::AxisEntry &b) {
    CHECK(a.isVisible == b.isVisible);
    CHECK(a.showInStrip == b.showInStrip);
    CHECK(a.isLocked == b.isLocked);
    CHECK(a.dirty == b.dirty);
    CHECK(a.actions == b.actions);
    CHECK(a.selection == b.selection);
}

void checkSnapshotEqual(const ProjectSnapshot &restored, const ProjectSnapshot &original) {
    CHECK(restored.activeAxis == original.activeAxis);
    CHECK(restored.procSelRegionId == original.procSelRegionId);
    for (size_t i = 0; i < ofs::kStandardAxisCount; ++i)
        checkAxisEntryEqual(restored.axes[i], original.axes[i]);
    REQUIRE(restored.regions.size() == original.regions.size());
    for (size_t i = 0; i < original.regions.size(); ++i)
        CHECK(restored.regions[i] == original.regions[i]);
    checkBookmarksEqual(restored.bookmarks, original.bookmarks);
}

ProjectSnapshot roundTrip(const ProjectSnapshot &in) {
    const PackedSnapshot packed = ofs::packSnapshot(in);
    REQUIRE_FALSE(packed.bytes.empty());
    return ofs::unpackSnapshot(packed);
}

// Bit-exact double compare: bitsery copies the IEEE-754 bits verbatim, so even values == cannot
// distinguish (NaN, ±0, ±Inf) must survive the round-trip identically. doctest::Approx would mask that.
bool sameBits(double a, double b) {
    return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b);
}

} // namespace

TEST_CASE("snapshot codec: a fully-populated snapshot round-trips byte-for-byte") {
    const ProjectSnapshot original = makeFullSnapshot();

    const PackedSnapshot packed = ofs::packSnapshot(original);
    REQUIRE_FALSE(packed.bytes.empty());

    const ProjectSnapshot restored = ofs::unpackSnapshot(packed);

    CHECK(restored.activeAxis == original.activeAxis);
    CHECK(restored.position == doctest::Approx(original.position));
    CHECK(restored.procSelRegionId == original.procSelRegionId);

    for (size_t i = 0; i < ofs::kStandardAxisCount; ++i)
        checkAxisEntryEqual(restored.axes[i], original.axes[i]);

    REQUIRE(restored.regions.size() == original.regions.size());
    // ProcessingRegion has a defaulted operator== covering the whole node graph, so this one compare
    // pins every region/node/link field at once.
    CHECK(restored.regions[0] == original.regions[0]);

    checkBookmarksEqual(restored.bookmarks, original.bookmarks);
}

TEST_CASE("snapshot codec: an empty snapshot round-trips") {
    const ProjectSnapshot original; // all axes absent, no regions

    const ProjectSnapshot restored = ofs::unpackSnapshot(ofs::packSnapshot(original));

    CHECK(restored.activeAxis == original.activeAxis);
    CHECK(restored.procSelRegionId == original.procSelRegionId);
    CHECK(restored.regions.empty());
    for (size_t i = 0; i < ofs::kStandardAxisCount; ++i)
        checkAxisEntryEqual(restored.axes[i], original.axes[i]);
}

TEST_CASE("snapshot codec: a default-constructed PackedSnapshot decodes to an empty snapshot") {
    // SnapshotHistory returns an empty PackedSnapshot{} for a popped/cleared slot; unpacking one must not
    // crash and must yield a clean default snapshot (empty bytes -> inflate returns {}).
    const ProjectSnapshot restored = ofs::unpackSnapshot(PackedSnapshot{});
    CHECK(restored.regions.empty());
    CHECK(restored.procSelRegionId == ProjectSnapshot{}.procSelRegionId);
}

TEST_CASE("snapshot codec: every axis slot round-trips independently (no cross-slot bleed)") {
    // The full-snapshot test only populates L0 + S3. The axes array is serialized as a fixed sequence with
    // no per-element id, so an off-by-one in the loop would shift every slot. Give all 20 slots distinct
    // data and flags, then verify each lands back in its own slot.
    ProjectSnapshot snap;
    for (size_t i = 0; i < ofs::kStandardAxisCount; ++i) {
        auto &e = snap.axes[i];
        e.showInStrip = (i % 2) == 0;
        e.isVisible = (i % 3) != 0;
        e.showInStrip = (i % 4) != 0;
        e.isLocked = (i % 5) == 0;
        e.dirty = (i % 2) == 1;
        // A point whose time/pos encode the slot index, so a shifted slot is detectable by value.
        e.actions.insert({static_cast<double>(i) + 0.5, static_cast<int>(i)});
        e.actions.insert({static_cast<double>(i) + 1.5, 100 - static_cast<int>(i)});
    }
    checkSnapshotEqual(roundTrip(snap), snap);
}

TEST_CASE("snapshot codec: multiple regions preserve order and per-region data") {
    ProjectSnapshot snap;
    for (int i = 0; i < 5; ++i) {
        ProcessingRegion r;
        r.id = 100 + i;
        r.startTime = i * 10.0;
        r.endTime = i * 10.0 + 5.0;
        r.name = "region-" + std::to_string(i);
        r.hz = 30 + i;
        snap.regions.push_back(std::move(r));
    }
    const ProjectSnapshot restored = roundTrip(snap);
    REQUIRE(restored.regions.size() == 5);
    for (int i = 0; i < 5; ++i)
        CHECK(restored.regions[static_cast<size_t>(i)].id == 100 + i); // order, not just set membership
    checkSnapshotEqual(restored, snap);
}

TEST_CASE("snapshot codec: numeric boundary values survive exactly") {
    ProjectSnapshot snap;
    snap.activeAxis = StandardAxis::S9; // last valid enumerator — value1b edge
    snap.procSelRegionId = std::numeric_limits<int>::min();

    auto &a = snap.axes[static_cast<size_t>(StandardAxis::L0)];
    a.showInStrip = true;
    // pos is stored in one byte (invariant 0–100); 0 and 255 pin the encoding's full byte range.
    a.actions.insert({0.0, 0});
    a.actions.insert({1.0, 255});

    ProcessingRegion r;
    r.id = std::numeric_limits<int>::max();
    r.startTime = std::numeric_limits<double>::lowest();
    r.endTime = std::numeric_limits<double>::max();
    r.color = 0xFFFFFFFFU; // full ImU32, all bits set
    r.hz = std::numeric_limits<int>::min();

    ofs::ProcessingGraphNode n;
    n.id = std::numeric_limits<int>::max();
    n.type = ofs::GraphNodeType::Script;
    n.constantValue = std::numeric_limits<float>::max();
    n.posX = std::numeric_limits<float>::lowest();
    n.posY = std::numeric_limits<float>::denorm_min();
    n.role = StandardAxis::S9;
    // Every value1b count/kind at its type maximum — guards against a narrowing in the codec.
    n.pluginInputCount = 255;
    n.pluginSignal = 255;
    n.scriptSignal = 255;
    n.scriptInputCount = 255;
    n.effect.params = {std::numeric_limits<float>::lowest(), 0.0f, std::numeric_limits<float>::max()};
    r.nodeGraph.nodes = {n};
    r.nodeGraph.nextId = std::numeric_limits<int>::max();
    snap.regions.push_back(std::move(r));

    checkSnapshotEqual(roundTrip(snap), snap);
}

TEST_CASE("snapshot codec: special double bit-patterns round-trip bitwise") {
    // ±0, ±Inf and NaN must come back with identical bits. operator== would treat -0==+0 and NaN!=NaN,
    // so the region path can't test these — drive them through snap.position and compare the raw bits.
    for (double v : {-0.0, std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::quiet_NaN()}) {
        ProjectSnapshot snap;
        snap.position = v;
        const ProjectSnapshot restored = roundTrip(snap);
        CHECK(sameBits(restored.position, v));
    }
}

TEST_CASE("snapshot codec: a fully-set axis-role bitset round-trips") {
    ProjectSnapshot snap;
    ProcessingRegion r;
    r.id = 1;
    for (size_t i = 0; i < ofs::kStandardAxisCount; ++i)
        r.axisRoles.set(i); // all 20 bits — exercises the StdBitset extension at full width
    snap.regions.push_back(std::move(r));
    const ProjectSnapshot restored = roundTrip(snap);
    REQUIRE(restored.regions.size() == 1);
    CHECK(restored.regions[0].axisRoles.count() == ofs::kStandardAxisCount);
    CHECK(restored.regions[0] == snap.regions[0]);
}

TEST_CASE("snapshot codec: a large action set round-trips at the packed size") {
    // Exercises the VectorSetExt extension at scale and pins the per-action cost of the flat (verbatim)
    // encoding: 9 bytes/action (value8b at + 1-byte pos) plus small fixed overhead, no compression.
    ProjectSnapshot snap;
    auto &a = snap.axes[static_cast<size_t>(StandardAxis::L0)];
    a.showInStrip = true;
    constexpr int kCount = 50'000;
    for (int i = 0; i < kCount; ++i)
        a.actions.insert({static_cast<double>(i) * 0.01, i % 101});

    const PackedSnapshot packed = ofs::packSnapshot(snap);
    REQUIRE_FALSE(packed.bytes.empty());
    CHECK(packed.sizeBytes() >= static_cast<size_t>(kCount) * 9);
    CHECK(packed.sizeBytes() < static_cast<size_t>(kCount) * 10); // tight: ~9 bytes/action, no bloat

    const ProjectSnapshot restored = ofs::unpackSnapshot(packed);
    REQUIRE(restored.axes[static_cast<size_t>(StandardAxis::L0)].actions.size() == kCount);
    checkSnapshotEqual(restored, snap);
}

TEST_CASE("snapshot codec: selection round-trips across coverage patterns (index encoding)") {
    // Selection is serialized as delta indices into the axis's actions set (SelectionExt). Exercise the
    // shapes that stress that encoding: none selected, every point, a sparse scatter, and the two ends.
    constexpr int kCount = 200;
    auto build = [](const std::vector<int> &selectedIdx) {
        ProjectSnapshot snap;
        auto &e = snap.axes[static_cast<size_t>(StandardAxis::L0)];
        e.showInStrip = true;
        for (int i = 0; i < kCount; ++i)
            e.actions.insert({static_cast<double>(i) * 0.5, i % 101});
        for (int i : selectedIdx)
            e.selection.insert({static_cast<double>(i) * 0.5, i % 101}); // a genuine subset of actions
        return snap;
    };

    SUBCASE("nothing selected") {
        checkSnapshotEqual(roundTrip(build({})), build({}));
    }
    SUBCASE("every action selected") {
        std::vector<int> all(kCount);
        for (int i = 0; i < kCount; ++i)
            all[static_cast<size_t>(i)] = i;
        checkSnapshotEqual(roundTrip(build(all)), build(all));
    }
    SUBCASE("sparse scatter") {
        checkSnapshotEqual(roundTrip(build({0, 3, 4, 99, 137, 199})), build({0, 3, 4, 99, 137, 199}));
    }
    SUBCASE("first and last only") {
        checkSnapshotEqual(roundTrip(build({0, kCount - 1})), build({0, kCount - 1}));
    }
}

TEST_CASE("snapshot codec: an index-encoded selection is far smaller than its actions") {
    // A fully-selected axis stores each selected point as a 1-byte delta index, not a 9-byte record. With
    // compression off (verbatim path) that ratio is directly observable in the blob: the selection adds
    // ~1 byte/point on top of the ~9 byte/point actions, i.e. well under a second full copy.
    ProjectSnapshot snap;
    auto &e = snap.axes[static_cast<size_t>(StandardAxis::L0)];
    e.showInStrip = true;
    constexpr int kCount = 10'000;
    for (int i = 0; i < kCount; ++i) {
        e.actions.insert({static_cast<double>(i) * 0.01, i % 101});
        e.selection.insert({static_cast<double>(i) * 0.01, i % 101}); // select all
    }
    const size_t withSel = ofs::packSnapshot(snap).sizeBytes();

    snap.axes[static_cast<size_t>(StandardAxis::L0)].selection.clear();
    const size_t withoutSel = ofs::packSnapshot(snap).sizeBytes();

    // The whole selection (kCount points) must cost far less than re-storing the actions (≥9 bytes each):
    // index deltas are ~1 byte, so the delta is closer to kCount than to kCount*9.
    CHECK((withSel - withoutSel) < static_cast<size_t>(kCount) * 3);
}

TEST_CASE("snapshot codec: restored VectorSets stay sorted and unique") {
    ProjectSnapshot snap;
    auto &a = snap.axes[static_cast<size_t>(StandardAxis::L0)].actions;
    // Insert out of order; VectorSet sorts. The codec writes in stored (sorted) order and re-inserts on
    // read — confirm the restored set is still strictly ascending in time.
    for (double t : {5.0, 1.0, 9.0, 3.0, 7.0, 2.0})
        a.insert({t, 50});
    const ProjectSnapshot restored = roundTrip(snap);
    const auto &ra = restored.axes[static_cast<size_t>(StandardAxis::L0)].actions;
    REQUIRE(ra.size() == 6);
    for (size_t i = 1; i < ra.size(); ++i)
        CHECK(ra.begin()[i - 1].at < ra.begin()[i].at);
}

TEST_CASE("snapshot codec: packing is deterministic") {
    // SnapshotHistory's budget accounting and any future de-dup rely on identical input -> identical bytes.
    const ProjectSnapshot snap = makeFullSnapshot();
    const PackedSnapshot a = ofs::packSnapshot(snap);
    const PackedSnapshot b = ofs::packSnapshot(snap);
    CHECK(a.bytes == b.bytes);
}

TEST_CASE("snapshot codec: corrupt input fails safe (no crash, empty snapshot)") {
    // The decode path must never throw or crash on a malformed buffer — it logs and yields a default
    // snapshot. (Trusted in-memory data, but the SnapshotHistory arena can hand back a partial slot.) The
    // buffer is fed straight to bitsery, whose reader reports an error when it runs out of bytes before the
    // schema is satisfied; unpack then returns a clean default rather than a partially-filled snapshot.
    SUBCASE("a truncated buffer") {
        PackedSnapshot p;
        p.bytes = {0x01, 0x02, 0x03}; // far too short to satisfy the ProjectSnapshot schema
        const ProjectSnapshot restored = ofs::unpackSnapshot(p);
        CHECK(restored.regions.empty());
    }
    SUBCASE("garbage bytes") {
        PackedSnapshot p;
        // A short run of arbitrary bytes: bitsery consumes a few fields then runs out → reader error.
        p.bytes = {64, 0, 0, 0, 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0xFF};
        const ProjectSnapshot restored = ofs::unpackSnapshot(p);
        CHECK(restored.regions.empty());
    }
    SUBCASE("a valid pack with one byte flipped") {
        PackedSnapshot p = ofs::packSnapshot(makeFullSnapshot());
        REQUIRE(p.bytes.size() > 6);
        p.bytes[p.bytes.size() / 2] ^= 0xFFU;                    // corrupt mid-buffer
        const ProjectSnapshot restored = ofs::unpackSnapshot(p); // must not crash
        // The flat buffer has no integrity check, so a single flipped byte may decode to a
        // plausible-but-wrong snapshot or trip a reader error — either way the only guarantee (and all we
        // assert) is that decoding stays safe and never crashes. Reading `restored` keeps it observed.
        (void)restored;
    }
}
