#pragma once

#include "Core/ScriptAxisAction.h"
#include "Core/StandardAxis.h"
#include "Core/VectorSet.h"

namespace ofs {

// One axis's worth of copied/cut actions. The clipboard is a list of these (one per axis captured
// from the active group), and a paste carries a list of them so a grouped copy round-trips per axis.
struct AxisClip {
    StandardAxis role;
    VectorSet<ScriptAxisAction> actions;
};

} // namespace ofs
