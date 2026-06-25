#pragma once

#include "Core/StandardAxis.h"

#include <vector>

namespace ofs {

// ── Graph presets (shareable single-graph .json files under <prefPath>/graphs) ──

// Save the selected region's node graph (nodes/links + axis roles, no time range) to a file.
struct SaveGraphEvent {
    int regionId;
};

// Load a graph preset from a file and replace the selected region's graph.
// If the file's axes differ from the region's, ProjectManager stashes a
// PendingGraphLoad and the UI raises a remap dialog (-> ApplyGraphRemapEvent).
struct LoadGraphEvent {
    int regionId;
};

// One saved-axis -> target-axis remap entry.
struct AxisRemapEntry {
    StandardAxis from; // axis as stored in the loaded graph
    StandardAxis to;   // axis to retarget it onto in the region
};

// Confirm a pending graph load, remapping each saved axis onto a region axis.
struct ApplyGraphRemapEvent {
    int regionId;
    std::vector<AxisRemapEntry> mapping;
};

// Discard a pending graph load without applying it.
struct CancelGraphLoadEvent {
    int regionId;
};

// Open the axis-remap dialog for the region's *existing* graph (no file load). ProjectManager
// stashes a PendingGraphLoad built from the region's current node graph; the UI then raises the
// same remap dialog (-> ApplyGraphRemapEvent) used for differing-axis loads.
struct RemapCurrentGraphEvent {
    int regionId;
};

// Accept the embedded scripts of a pending graph load: stamp each script's source onto its node
// (so it becomes a graph-embedded node that runs in-memory — nothing is written to the scripts
// folder), then proceed (apply directly, or fall through to the axis-remap step). The user later
// promotes a node to a file with SaveEmbeddedScriptEvent. Pushed by the trust dialog; never auto-fired.
struct ConfirmGraphTrustEvent {
    int regionId;
};

// Let the user read a pending graph's embedded scripts before accepting the trust prompt.
// ProjectManager writes each embedded script to a throwaway <temp>/ofs-graph-review/<file>.cs
// and opens it in the system editor. Pure review side-effect: it does not accept or apply the
// load, and never touches <pref>/scripts (the authoritative materialize still happens only on
// ConfirmGraphTrustEvent / SaveEmbeddedScriptEvent).
struct ReviewGraphScriptsEvent {
    int regionId;
};

} // namespace ofs
