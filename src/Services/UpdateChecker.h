#pragma once

#include "Core/UpdateEvents.h"

#include <functional>
#include <optional>
#include <string>

namespace ofs {

class EventQueue;
class JobSystem;

// Queries the GitHub releases feed and compares the latest stable release against the running build's
// git tag (semantic-version precedence — see Util/SemVer.h). The HTTPS GET + parse run on a JobSystem
// worker; the outcome is reported only through the update events, never by mutating shared state off the
// main thread. The service keeps a small Status the UI can read each frame (a passive read seam, like a
// registry); it is updated solely by the main-thread result handlers.
class UpdateChecker {
  public:
    UpdateChecker(EventQueue &eq, JobSystem &jobs);

    enum class State { Idle, Checking, UpToDate, Available, Failed };

    struct Status {
        State state = State::Idle;
        std::string latestVersion; // populated when state == Available
        std::string releaseUrl;    // populated when state == Available
        std::string notes;         // populated when state == Available
        std::string error;         // populated when state == Failed
    };

    [[nodiscard]] const Status &status() const { return status_; }

  private:
    void onCheckRequested(const CheckForUpdatesEvent &e);

    EventQueue &eq_;
    JobSystem &jobs_;
    Status status_;
    bool inFlight_ = false; // a check is running; guards against overlapping requests
};

// Test seam: when set, the checker calls this instead of issuing a real HTTPS GET. It returns the raw
// release-feed JSON body, or nullopt to simulate a transport failure. Set before triggering a check;
// pass nullptr to restore real network use. Mirrors setNativeDialogOverrideForTesting in ModalManager.
void setUpdateFetchOverrideForTesting(std::function<std::optional<std::string>()> fn);

// Test seam: override the running build's version used for the comparison (default: the baked git tag).
// Lets a test drive the "update available" path deterministically even when the test build carries no
// release tag (e.g. a pre-1.0 checkout). Pass "" to restore the real version.
void setUpdateCurrentVersionForTesting(std::string version);

} // namespace ofs
