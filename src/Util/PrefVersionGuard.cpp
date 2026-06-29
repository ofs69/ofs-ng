#include "PrefVersionGuard.h"
#include "Util/FileUtil.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"
#include <filesystem>
#include <nlohmann/json.hpp>

namespace ofs {

static std::filesystem::path getMarkerPath() {
    return ofs::util::getPrefPath() / "pref-version.json";
}

PrefVersionStatus checkPrefDirVersion(std::string_view thisBuildVersion) {
    PrefVersionStatus status;

    // Read the existing stamp. A missing or malformed marker is treated as "no stamp" (schema 0):
    // an old pref dir from a pre-guard build, or a first run, is freely writable and we stamp it.
    if (auto text = ofs::util::readFile(getMarkerPath())) {
        try {
            const nlohmann::json j = nlohmann::json::parse(*text);
            status.markerSchema = j.value("schema", 0);
            status.writtenBy = j.value("writtenBy", std::string{});
        } catch (const std::exception &e) {
            OFS_CORE_WARN("Ignoring unreadable pref-version marker: {}", e.what());
        }
    }

    if (status.markerSchema > kPrefSchemaVersion) {
        // A newer build owns this pref dir. Do NOT write the marker (or anything else) — the caller
        // refuses to launch so the newer preferences are left intact.
        status.ok = false;
        OFS_CORE_ERROR("Pref dir schema {} (written by {}) is newer than this build's schema {}; refusing to start.",
                       status.markerSchema, status.writtenBy.empty() ? "an unknown version" : status.writtenBy,
                       kPrefSchemaVersion);
        return status;
    }

    // Compatible (or absent) stamp: (re)write the marker so it records the newest build that has
    // written here. Raising the schema is what makes a future older build refuse. Skip the write when
    // the marker already names this exact build, so a normal relaunch costs no I/O.
    if (status.markerSchema != kPrefSchemaVersion || status.writtenBy != thisBuildVersion) {
        const nlohmann::json j = {{"schema", kPrefSchemaVersion}, {"writtenBy", std::string(thisBuildVersion)}};
        if (!ofs::util::writeFileAtomic(getMarkerPath(), j.dump(2)))
            OFS_CORE_WARN(
                "Failed to write pref-version marker; pref-dir downgrade protection is degraded this session.");
    }

    return status;
}

} // namespace ofs
