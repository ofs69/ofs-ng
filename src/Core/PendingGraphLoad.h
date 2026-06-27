#pragma once

#include "Core/ProcessingRegion.h"
#include "Core/StandardAxis.h"

#include <string>
#include <vector>

namespace ofs {

// Transient (never serialized): a graph awaiting the user's axis-remap confirmation. Either a preset
// loaded from disk whose axis set differs from the target region's, or a copy of the region's own
// current graph staged by the on-demand Remap button. Lives on ScriptProject; the ProcessingPanel
// reads it to raise the remap dialog.
struct PendingGraphLoad {
    int regionId = -1;
    ProcessingNodeGraph graph;           // the graph to remap; roles still as saved
    std::vector<StandardAxis> savedAxes; // distinct Input/Output axes in `graph`, in encounter order
    std::string name;                    // region name saved with the graph; restored on apply
    int hz = kDefaultRegionHz;           // region discretization rate saved with the graph
    // If any Script node in `graph` carries embedded source, the graph holds executable code and
    // MUST clear the trust gate (needsTrust) before it is applied or compiled.
    bool needsTrust = false; // true until the user accepts the embedded scripts
};

} // namespace ofs
