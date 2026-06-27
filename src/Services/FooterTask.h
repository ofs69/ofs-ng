#pragma once

#include "Core/EventQueue.h"
#include "Core/TaskEvents.h"

#include <cstdint>
#include <string>

namespace ofs {

// Lifecycle helper for a footer background-task indicator — the TaskStarted/Progress/Ended event trio a
// service raises while running async JobSystem work (waveform extraction, transcode, …). start() mints a
// fresh monotonic id and raises the entry, progress() updates it, end() removes it (idempotent). The
// monotonic id lets a superseded op's late event be told apart from its replacement via matches(). Main-
// thread only, like every event it pushes.
class FooterTask {
  public:
    explicit FooterTask(EventQueue &eq) : eq_(eq) {}

    // Raise a fresh indicator, abandoning any current one (its late events no longer match()).
    // `cancellable` shows an abort button that pushes CancelTaskEvent{id}.
    void start(std::string label, bool cancellable) {
        id_ = ++seq_;
        eq_.push(TaskStartedEvent{.id = id_, .label = std::move(label), .cancellable = cancellable});
    }

    void progress(std::string detail, float fraction) {
        if (id_ != 0)
            eq_.push(TaskProgressEvent{.id = id_, .detail = std::move(detail), .progress = fraction});
    }

    // Remove the indicator if one is showing. Idempotent.
    void end() {
        if (id_ != 0) {
            eq_.push(TaskEndedEvent{.id = id_});
            id_ = 0;
        }
    }

    [[nodiscard]] bool active() const { return id_ != 0; }
    [[nodiscard]] uint32_t id() const { return id_; }
    // True when `eventId` names the currently-showing task (e.g. a CancelTaskEvent for our entry).
    [[nodiscard]] bool matches(uint32_t eventId) const { return id_ != 0 && eventId == id_; }

  private:
    EventQueue &eq_;
    uint32_t id_ = 0;
    uint32_t seq_ = 0;
};

} // namespace ofs
