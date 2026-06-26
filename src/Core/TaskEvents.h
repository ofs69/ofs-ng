#pragma once

#include <cstdint>
#include <string>

namespace ofs {

// Generic, non-blocking background-task indicator. A service running async work on the JobSystem posts
// these to surface a live progress entry in the footer task stack (NotificationState::tasks) instead of
// raising a blocking modal. The producer mints `id`, keeps it stable for the task's lifetime, matches it
// back on CancelTaskEvent to abort, and removes the entry with TaskEndedEvent on success/failure/cancel.
struct TaskStartedEvent {
    uint32_t id = 0;
    std::string label;        // localized, e.g. "Audio waveform"
    bool cancellable = false; // show an abort button that pushes CancelTaskEvent{id}
};

// Updates a running task's indicator. `progress` < 0 renders an indeterminate (animated) bar; 0..1 a
// determinate one. `detail` is an optional sub-line (phase / ETA). A progress event for an unknown id is
// ignored by the handler (the task may have already ended).
struct TaskProgressEvent {
    uint32_t id = 0;
    std::string detail;
    float progress = -1.0f;
};

// Removes the task's indicator. The same event ends a task whether it succeeded, failed, or was
// cancelled — any user-facing outcome message is a separate NotifyEvent the producer chooses to push.
struct TaskEndedEvent {
    uint32_t id = 0;
};

// User clicked a running task's abort button. The owning service matches `id` and signals its cancel
// flag; the worker then stops and reports its terminal result (which the service turns into TaskEnded).
struct CancelTaskEvent {
    uint32_t id = 0;
};

} // namespace ofs
