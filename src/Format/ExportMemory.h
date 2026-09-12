#pragma once

#include "Core/ExportConfig.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace ofs {

// One project's remembered Quick Export parameters, keyed by the project file they belong to.
struct ProjectExportMemory {
    std::string projectPath; // UTF-8 absolute path to the .ofp the config belongs to
    ExportConfig config;
};

void to_json(nlohmann::json &j, const ProjectExportMemory &m);
void from_json(const nlohmann::json &j, ProjectExportMemory &m);

// How many projects keep a remembered export. Generous enough that a normal rotation of projects never
// forgets, bounded so the file can't grow without limit across years of use.
inline constexpr size_t kMaxExportMemories = 32;

// Every project's Quick Export config, persisted in export_configs.json beside settings.json.
//
// This is app-side rather than .ofp state on purpose: an export writes no document state, so recording
// one must not mark the project dirty (see ProjectManager::recordLastExport), and the output path is an
// absolute local one that has no business travelling with a shared project file. It gets its own file
// rather than a corner of settings.json because it is machine bookkeeping keyed by path — it grows and
// evicts on its own schedule, and a user editing or deleting it should not put their preferences at
// risk.
struct ExportMemory {
    // Most-recently-exported project first; maintained through remember() so the cap and the ordering
    // are enforced in one place.
    std::vector<ProjectExportMemory> entries;

    static ExportMemory load();
    void save() const;

    // Record `config` as `projectPath`'s remembered export, promoting it to the front and evicting the
    // least recently exported project past kMaxExportMemories. An empty path is ignored — an untitled
    // project has nothing to key on, and its config lives only in the session.
    void remember(std::string projectPath, ExportConfig config);

    // The remembered config for `projectPath`, or nullptr if that project has never exported (or has
    // been evicted). The pointer is invalidated by the next remember().
    [[nodiscard]] const ExportConfig *find(const std::string &projectPath) const;
};

} // namespace ofs
