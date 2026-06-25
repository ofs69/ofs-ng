#pragma once

#include "Services/ProjectSnapshot.h"
#include <cstdint>
#include <vector>

namespace ofs {

// The undo-snapshot codec: bitsery serializes a ProjectSnapshot into a flat byte buffer (PackedSnapshot),
// stored verbatim — no compression. Kept behind this narrow interface so bitsery stays confined to
// SnapshotCodec.cpp — no other TU pays its compile cost or sees its headers.
//
// pack serializes straight into the kept PackedSnapshot, so there is no scratch buffer and no extra copy;
// the codec is stateless. unpack is the exact inverse of pack for any snapshot pack produced; on a decode
// failure (corrupt/empty buffer) it logs and returns a default-constructed snapshot. Main-thread use only,
// in keeping with the rest of UndoSystem.
class SnapshotCodec {
  public:
    [[nodiscard]] PackedSnapshot pack(const ProjectSnapshot &snapshot);
    [[nodiscard]] ProjectSnapshot unpack(const PackedSnapshot &packed);
};

// Convenience free functions equivalent to constructing a SnapshotCodec and calling it (the codec is
// stateless, so there is no difference beyond call-site brevity).
[[nodiscard]] PackedSnapshot packSnapshot(const ProjectSnapshot &snapshot);
[[nodiscard]] ProjectSnapshot unpackSnapshot(const PackedSnapshot &packed);

} // namespace ofs
