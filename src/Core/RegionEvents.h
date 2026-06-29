#pragma once

#include "Core/ProcessingRegion.h" // ProcessingRegion, AxisRoles
#include "Core/StandardAxis.h"

#include <limits>

namespace ofs {

struct CreateRegionEvent {
    StandardAxis axisRole; // lead — drives the present-check and is always part of the region
    AxisRoles axisRoles;   // full spanned set; empty default ⇒ just {axisRole}
    double startTime;
    double endTime;
    // End of the timeline (video/dummy duration). Bounds where a region may be placed so that
    // snapping a region forward past an occupied anchor can't run it off the end of the media.
    // Defaults to unbounded for programmatic callers that have no timeline length to enforce.
    double timelineDuration = std::numeric_limits<double>::max();
};

struct DeleteRegionEvent {
    int regionId;
};

struct ModifyRegionEvent {
    int regionId;
    ProcessingRegion updatedRegion;
    bool snapshot = true;
};

struct MoveRegionNodesEvent {
    int regionId;
    ProcessingRegion updatedRegion; // only nodeGraph.nodes[*].posX/posY are read
};

struct BakeRegionEvent {
    int regionId;
};

// Emitted by ProjectManager after a region create/delete/bake *actually* completed — a create that found
// room, a delete/bake that found its target. The request handlers bail on a no-op (no free slot, missing
// region), so observers (the audio-feedback cue) react to this outcome rather than the
// Create/Delete/BakeRegionEvent *request*. Derived/informational only — not undoable (the originating
// edit already is).
enum class RegionChangeKind { Created, Deleted, Baked };
struct RegionChangedEvent {
    RegionChangeKind kind;
};

struct AssignAxisToRegionEvent {
    int regionId;
    StandardAxis axis;
    bool assign; // true = add this axis to the region, false = remove
};

struct ClearRegionSelectionEvent {};

// Toggle the processing panel's focus lock. Transient session state (ScriptProject::procPanelLocked).
struct SetProcPanelLockedEvent {
    bool locked;
};

struct SelectRegionEvent {
    int regionId;
};

struct UpdateTimelineViewEvent {
    double visibleTime;
    double offsetTime;
};

struct SetTimelineShowPointsEvent {
    bool show;
};

struct SetTimelineShowWaveformEvent {
    bool show;
};

} // namespace ofs
