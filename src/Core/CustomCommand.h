#pragma once

#include "Core/IntentEvents.h"
#include <string>

namespace ofs {

// One user-defined command. A plain serializable struct with no behavior — its CustomCommandTemplate
// (resolved by `templateKey`, see Services/CustomCommandTemplate.h) renders its editor, turns it into a
// Command, and (de)serializes its params. Only the fields the resolved template reads are meaningful;
// the rest stay at their documented defaults (harmless — the flat-carrier decision). The typed fields
// below are the shared carrier for the first-party templates; an opaque param bag for plugin-contributed
// templates is the reserved seam.
struct CustomCommand {
    std::string id;          // stable: "custom.<n>" — assigned once, never changes on edit/rename
    std::string name;        // user-supplied display title (NOT localized — like a plugin command title)
    std::string templateKey; // which template builds/edits/serializes this; matches CustomCommandTemplate::key

    StepDirection direction = StepDirection::Forward; // Step / MoveTime: Backward / Forward
    int reps = 1; // Step / MoveTime: base step count (>=1); scaled by holdRepeats on a held tick
    StepGranularity granularity = StepGranularity::Frame; // Step only: Frame | Action | ActionAllAxes
    int delta = 1;                                        // MovePosition only: signed position units (e.g. +7, -25)
    bool seekAfter = false;                               // MoveTime only: seek the playhead after moving
};

} // namespace ofs
