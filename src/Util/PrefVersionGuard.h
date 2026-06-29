#pragma once

#include <string>
#include <string_view>

namespace ofs {

// Monotonic schema version of the writable preference directory *as a whole* — every file the app
// persists under getPrefPath() (settings.json, layouts.json, bindings, custom commands, plugin state,
// themes, trusted graphs, …) treated as one versioned unit.
//
// Bump this by one whenever a change to ANY pref-dir file format is incompatible with older builds —
// i.e. when an older build, having refused to *read* the new file, would then *overwrite* it with its
// old format and lose or corrupt the newer data. (A purely additive field that old builds ignore is
// NOT a break and must not bump this.)
//
// The pref dir is stamped with this number (see checkPrefDirVersion). A build started against a pref
// dir stamped HIGHER than the value it was compiled with refuses to launch, so an older build can
// never clobber a newer build's preferences. The guard is necessarily forward-looking: it only
// protects against builds that themselves contain it — a pre-guard release left no stamp and is not
// bound by it.
inline constexpr int kPrefSchemaVersion = 1;

struct PrefVersionStatus {
    // False only when the pref dir was stamped by a newer, incompatible build: the caller MUST refuse
    // to launch rather than overwrite preferences this build can't safely round-trip. True both when
    // the stamp is compatible and when there is no stamp / it could not be read (fail-open).
    bool ok = true;
    int markerSchema = 0;  // schema recorded in the pref dir (0 = absent / unreadable)
    std::string writtenBy; // version string the newer build recorded, for the refusal message
};

// Reconcile the pref-dir version marker with this build:
//   - marker schema > kPrefSchemaVersion  → returns {ok = false} and leaves the marker untouched.
//   - otherwise                           → stamps the marker up to kPrefSchemaVersion (recording
//                                           `thisBuildVersion`) and returns {ok = true}.
// `thisBuildVersion` is the running build's version string (e.g. ofs::generated::kGitTag), written
// into the marker so a future older build can name the culprit in its refusal message.
PrefVersionStatus checkPrefDirVersion(std::string_view thisBuildVersion);

} // namespace ofs
