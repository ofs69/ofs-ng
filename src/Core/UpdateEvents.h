#pragma once

#include <string>

namespace ofs {

// Update-checker domain events. The checker queries the GitHub releases feed off the main thread and
// reports the outcome back through these. `userInitiated` flows from the request into every result so
// the UI can distinguish a manual "Check for updates" (always worth a visible answer, including "you're
// up to date") from the silent startup check (stay quiet unless an update actually exists).

// Request: query the release feed. Pushed by the "Check for updates" command (userInitiated=true) and
// once at startup when the preference is enabled (userInitiated=false).
struct CheckForUpdatesEvent {
    bool userInitiated = false;
};

// Result: a release newer than the running build exists.
struct UpdateAvailableEvent {
    std::string version;    // release tag, e.g. "v1.2.0"
    std::string releaseUrl; // the release page (html_url) to open in a browser
    std::string notes;      // release body / changelog (may be empty)
    bool userInitiated = false;
};

// Result: the running build is current (or the local version is unknown, so nothing is offered).
struct UpdateUpToDateEvent {
    bool userInitiated = false;
};

// Result: the check could not complete (no network, bad response, parse error).
struct UpdateCheckFailedEvent {
    std::string reason;
    bool userInitiated = false;
};

} // namespace ofs
