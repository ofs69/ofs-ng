#include "SnapshotCodec.h"
#include "Core/BookmarkChapterState.h"
#include "Core/ProcessingRegion.h"
#include "Core/ScriptAxisAction.h"
#include "Core/StandardAxis.h"
#include "Util/Log.h"

#include <bitsery/adapter/buffer.h>
#include <bitsery/bitsery.h>
#include <bitsery/details/adapter_common.h> // writeSize / readSize
#include <bitsery/ext/std_bitset.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <type_traits>

// bitsery serializes a ProjectSnapshot into a flat byte buffer, which is stored verbatim — no compression.
// The serialize functions live here so bitsery stays confined to this TU. bitsery uses one
// `serialize(S&, T&)` per type for BOTH directions, found by ADL — so each is
// in namespace ofs (the types' namespace). MAINTENANCE: a field added to any struct below is silently
// dropped from undo/redo unless it is added to the matching serialize function. test_undo_snapshot_codec
// round-trips a fully-populated snapshot to guard this; extend it when you add a field.
// Sanity ceilings for bitsery's container/text size reads. These guard against a corrupt length in the
// buffer; snapshots are in-memory (never loaded from untrusted disk), so the bounds only need to exceed
// any real project. Exceeding one fails the decode (debug: also asserts).
namespace ofs::undo_detail {
constexpr size_t kMaxActions = 1ULL << 28; // ~268M actions per axis
constexpr size_t kMaxRegions = 1ULL << 20;
constexpr size_t kMaxNodes = 1ULL << 20;
constexpr size_t kMaxLinks = 1ULL << 20;
constexpr size_t kMaxParams = 1ULL << 16;
constexpr size_t kMaxChapters = 1ULL << 20;
constexpr size_t kMaxStr = 1ULL << 24;    // 16 MB — names, ids, file refs
constexpr size_t kMaxSource = 1ULL << 28; // 256 MB — embedded script source / node TState JSON

// bitsery extension serializing a VectorSet exactly like StdSet does an ordered std::set: write the
// element count then each element; on read, clear and re-insert. Snapshot data is already sorted, so the
// ascending inserts land at the end (O(n) total), preserving the VectorSet invariant without a re-sort
// pass. Keeps VectorSet free of any bitsery dependency.
// bitsery extension (de)serializing the actions VectorSet — the dominant data, by far the hottest loop in
// the codec. It bypasses bitsery's per-element object dispatch entirely: rather than call a
// serialize(ScriptAxisAction) through `fnc` (which would route the time through value8b and the `pos` byte
// through the whole s.ext template machinery), it drives the adapter directly — 8 raw bytes for `at`, 1 for
// `pos`. That collapses ~5 non-inlined call layers per action down to two writeBytes, which in a debug
// build (no inlining, checked iterators) is the difference between tens of milliseconds and a few for a
// large axis. `at` is written as its raw IEEE-754 bits (bit_cast to u64, no endian swap): snapshots never
// leave the process, so native byte order is correct and lossless for every value including NaN/±0/±Inf.
// `pos` is clamped to a full byte (0–255) — wider than the 0–100 invariant, so the round-trip is exact.
//
// On read the elements arrive in stored (ascending, unique) order, so reserve the count up front and
// appendSorted each one — an O(1) tail append (push_back) that skips insert()'s binary search, making the
// whole rebuild O(n). appendSorted self-corrects if the bytes are somehow out of order, so the sorted
// invariant holds regardless; `size` is bounded by maxSize (readSize), matching the in-memory trust model.
class ActionVectorExt {
  public:
    constexpr explicit ActionVectorExt(size_t cap) : maxSize(cap) {}

    template <typename Ser, typename T, typename Fnc> void serialize(Ser &ser, const T &obj, Fnc &&) const {
        const size_t size = obj.size();
        assert(size <= maxSize);
        bitsery::details::writeSize(ser.adapter(), size);
        for (const auto &a : obj) {
            ser.adapter().template writeBytes<8>(std::bit_cast<uint64_t>(a.at));
            ser.adapter().template writeBytes<1>(static_cast<uint8_t>(std::clamp(a.pos, 0, 255)));
        }
    }

    template <typename Des, typename T, typename Fnc> void deserialize(Des &des, T &obj, Fnc &&) const {
        size_t size{};
        bitsery::details::readSize(des.adapter(), size, maxSize,
                                   std::integral_constant<bool, Des::TConfig::CheckDataErrors>{});
        obj.clear();
        obj.reserve(size);
        for (size_t i = 0; i < size; ++i) {
            uint64_t bits{};
            uint8_t pos{};
            des.adapter().template readBytes<8>(bits);
            des.adapter().template readBytes<1>(pos);
            obj.appendSorted(ScriptAxisAction{std::bit_cast<double>(bits), static_cast<int>(pos)});
        }
    }

  private:
    size_t maxSize;
};

// bitsery extension storing an axis's selection as indices into its actions set, not as full {at,pos}
// records. The selection is always a subset of actions (ScriptProject::mutate keeps it synced) and both
// are sorted by time, so the selected indices are strictly ascending — written as ascending *deltas* via
// writeSize varints (one byte for a typical small gap). A selected point therefore drops from 9 bytes to
// ~1, exactly losslessly: an index is a precise reference, no precision question. Holds a reference to the
// SAME actions set being (de)serialized and relies on actions being serialized before selection in
// serialize(AxisEntry&), so on read the indices resolve against an already-populated actions set. This is
// safe only because a snapshot is frozen — in the live model selection stays value-keyed (see the codec
// header / ScriptProject::mutate) since indices would drift as actions are edited.
class SelectionExt {
  public:
    SelectionExt(const VectorSet<ScriptAxisAction> &actions, size_t cap) : actionSet(actions), maxSize(cap) {}

    template <typename Ser, typename T, typename Fnc> void serialize(Ser &ser, const T &sel, Fnc &&) const {
        const size_t size = sel.size();
        assert(size <= maxSize);
        bitsery::details::writeSize(ser.adapter(), size);
        // Two-pointer merge: selection ⊆ actions and both ascending, so one forward walk of actions finds
        // every selected index in O(n). Emit the gap from the previous index, never the absolute one.
        auto aIt = actionSet.begin();
        size_t idx = 0;
        size_t prev = 0;
        for (const auto &entry : sel) {
            while (aIt != actionSet.end() && *aIt < entry) {
                ++aIt;
                ++idx;
            }
            assert(aIt != actionSet.end() && !(entry < *aIt)); // invariant: every selected point is an action
            bitsery::details::writeSize(ser.adapter(), idx - prev);
            prev = idx;
        }
    }

    template <typename Des, typename T, typename Fnc> void deserialize(Des &des, T &sel, Fnc &&) const {
        size_t size{};
        bitsery::details::readSize(des.adapter(), size, maxSize,
                                   std::integral_constant<bool, Des::TConfig::CheckDataErrors>{});
        sel.clear();
        sel.reserve(size);
        const size_t actionCount = actionSet.size();
        size_t idx = 0;
        for (size_t i = 0; i < size; ++i) {
            size_t delta{};
            // A delta can't exceed the number of actions; bound the read so a corrupt length fails the
            // decode rather than spinning. actionCount may be 0 only when size is too (empty selection).
            bitsery::details::readSize(des.adapter(), delta, actionCount,
                                       std::integral_constant<bool, Des::TConfig::CheckDataErrors>{});
            idx += delta;
            if (idx < actionCount)                                  // guard against a corrupt index
                sel.appendSorted(ScriptAxisAction{actionSet[idx]}); // ascending ⇒ O(1) tail append
        }
    }

  private:
    const VectorSet<ScriptAxisAction> &actionSet;
    size_t maxSize;
};
} // namespace ofs::undo_detail

// bitsery looks up ExtensionTraits in its own namespace; the object overload (s.ext(set, VectorSetExt{}))
// serializes each element via the element type's serialize function.
namespace bitsery::traits {
// Field names are bitsery's extension contract — they must match verbatim, so the naming linter is off here.
// NOLINTBEGIN(readability-identifier-naming)
// ActionVectorExt and SelectionExt both drive the adapter directly and ignore fnc, so each advertises only
// the object overload with a void TValue — mirrors bitsery's own ValueRange traits.
template <typename T> struct ExtensionTraits<ofs::undo_detail::ActionVectorExt, T> {
    using TValue = void;
    static constexpr bool SupportValueOverload = false;
    static constexpr bool SupportObjectOverload = true;
    static constexpr bool SupportLambdaOverload = false;
};
template <typename T> struct ExtensionTraits<ofs::undo_detail::SelectionExt, T> {
    using TValue = void;
    static constexpr bool SupportValueOverload = false;
    static constexpr bool SupportObjectOverload = true;
    static constexpr bool SupportLambdaOverload = false;
};
// NOLINTEND(readability-identifier-naming)
} // namespace bitsery::traits

namespace ofs {

// Note: there is no serialize(ScriptAxisAction) — the actions set is encoded in bulk by ActionVectorExt
// (time as 8 raw bytes + pos as 1), bypassing per-element dispatch. The selection references those same
// actions by index via SelectionExt, so it does not serialize action values either.

template <typename S> void serialize(S &s, ProcessingEffect &e) {
    s.text1b(e.type, undo_detail::kMaxStr);
    s.container4b(e.params, undo_detail::kMaxParams);
}

template <typename S> void serialize(S &s, ProcessingGraphNode &n) {
    s.value4b(n.id);
    s.value1b(n.type);
    s.object(n.effect);
    s.value4b(n.constantValue);
    s.value4b(n.posX);
    s.value4b(n.posY);
    s.value1b(n.role);
    s.value1b(n.pluginInputCount);
    s.value1b(n.pluginOutputCount);
    s.value1b(n.pluginSignal);
    s.text1b(n.scriptFile, undo_detail::kMaxStr);
    s.value1b(n.scriptSignal);
    s.value1b(n.scriptInputCount);
    s.value1b(n.scriptOutputCount);
    s.boolValue(n.scriptWatch);
    s.text1b(n.scriptEmbeddedSource, undo_detail::kMaxSource);
    s.text1b(n.pluginNodeId, undo_detail::kMaxStr);
    s.text1b(n.nodeState, undo_detail::kMaxSource);
}

template <typename S> void serialize(S &s, ProcessingGraphLink &l) {
    s.value4b(l.id);
    s.value4b(l.fromNode);
    s.value4b(l.fromPin);
    s.value4b(l.toNode);
    s.value4b(l.toPin);
}

template <typename S> void serialize(S &s, ProcessingNodeGraph &g) {
    s.container(g.nodes, undo_detail::kMaxNodes);
    s.container(g.links, undo_detail::kMaxLinks);
    s.value4b(g.nextId);
}

template <typename S> void serialize(S &s, ProcessingRegion &r) {
    s.value4b(r.id);
    s.value8b(r.startTime);
    s.value8b(r.endTime);
    s.text1b(r.name, undo_detail::kMaxStr);
    s.value4b(r.color);
    s.object(r.nodeGraph);
    s.value4b(r.hz);
    s.boolValue(r.showSourceActions);
    s.ext(r.axisRoles, bitsery::ext::StdBitset{});
    s.container(r.axisRoleTags, kStandardAxisCount, [](S &s2, std::string &t) { s2.text1b(t, undo_detail::kMaxStr); });
}

template <typename S> void serialize(S &s, ProjectSnapshot::AxisEntry &e) {
    s.boolValue(e.isVisible);
    s.boolValue(e.showInStrip);
    s.boolValue(e.isLocked);
    s.boolValue(e.dirty);
    s.ext(e.actions, undo_detail::ActionVectorExt{undo_detail::kMaxActions});
    // Selection after actions: SelectionExt encodes it as indices into the just-(de)serialized actions set.
    s.ext(e.selection, undo_detail::SelectionExt{e.actions, undo_detail::kMaxActions});
}

// A chapter's per-scene framing (Chapter::sceneView) is deliberately NOT snapshotted: it is captured-on-
// adjust outside the undo path, so undo/redo must leave the live framing untouched rather than revert it
// (UndoSystem::restoreSnapshot carries the live sceneView across the restore). Only size/position, name,
// and color participate in undo.
template <typename S> void serialize(S &s, Chapter &c) {
    s.value8b(c.startTime);
    s.value8b(c.endTime);
    s.text1b(c.name, undo_detail::kMaxStr);
    s.value4b(c.color);
}

template <typename S> void serialize(S &s, Bookmark &b) {
    s.value8b(b.time);
    s.text1b(b.name, undo_detail::kMaxStr);
}

template <typename S> void serialize(S &s, BookmarkChapterState &bc) {
    s.container(bc.bookmarks, undo_detail::kMaxChapters);
    s.container(bc.chapters, undo_detail::kMaxChapters);
}

template <typename S> void serialize(S &s, ProjectSnapshot &snap) {
    s.value1b(snap.activeAxis);
    s.value8b(snap.position);
    s.value4b(snap.procSelRegionId);
    for (auto &e : snap.axes) // fixed kStandardAxisCount array — no length prefix needed
        s.object(e);
    s.container(snap.regions, undo_detail::kMaxRegions);
    s.object(snap.bookmarks);
}

namespace {

using Buffer = std::vector<uint8_t>;
using OutputAdapter = bitsery::OutputBufferAdapter<Buffer>;
using InputAdapter = bitsery::InputBufferAdapter<Buffer>;

} // namespace

PackedSnapshot SnapshotCodec::pack(const ProjectSnapshot &snapshot) {
    // No compression and no framing: PackedSnapshot::bytes *is* the flat bitsery buffer. Serialize straight
    // into it — the result is the kept history entry, so this allocation is inherent (there is no separate
    // scratch to reuse and no extra copy to a payload).
    PackedSnapshot packed;
    // Reserve up front so the adapter never reallocates mid-serialize (it would otherwise double from empty,
    // re-copying the whole buffer ~log2(N) times — pure waste, and costly in a debug build). Actions
    // dominate at 9 bytes each; the fixed slack covers flags, regions, and the selection index lists.
    size_t estimate = 4096;
    for (const auto &e : snapshot.axes)
        estimate += e.actions.size() * 9 + e.selection.size();
    packed.bytes.reserve(estimate);
    const size_t written = bitsery::quickSerialization<OutputAdapter>(OutputAdapter{packed.bytes}, snapshot);
    packed.bytes.resize(written); // the adapter may over-allocate; trim to the bytes actually written
    return packed;
}

ProjectSnapshot SnapshotCodec::unpack(const PackedSnapshot &packed) {
    ProjectSnapshot snapshot;
    if (packed.bytes.empty()) // a popped/cleared history slot hands back empty bytes → default snapshot
        return snapshot;
    const auto [error, completed] =
        bitsery::quickDeserialization<InputAdapter>(InputAdapter{packed.bytes.begin(), packed.bytes.size()}, snapshot);
    if (error != bitsery::ReaderError::NoError || !completed) {
        // A truncated/garbage buffer can leave `snapshot` partially filled; never hand that back. The size
        // ceilings in the serialize functions bound the damage a corrupt length can do before we get here.
        OFS_CORE_ERROR("unpackSnapshot: bitsery deserialization failed (error={})", static_cast<int>(error));
        return ProjectSnapshot{};
    }
    return snapshot;
}

PackedSnapshot packSnapshot(const ProjectSnapshot &snapshot) {
    return SnapshotCodec{}.pack(snapshot);
}
ProjectSnapshot unpackSnapshot(const PackedSnapshot &packed) {
    return SnapshotCodec{}.unpack(packed);
}

} // namespace ofs
