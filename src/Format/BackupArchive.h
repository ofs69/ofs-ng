#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

// Single source of truth for the on-disk auto-backup archive: where a project's dated backups live, how
// they are named, how they are listed for the restore UI, and how the rolling window is pruned. Both the
// writer (ProjectManager) and the reader (BackupRestoreWindow) go through here so the naming and the
// directory key can never drift apart.
namespace ofs::backup {

// One dated backup on disk, as surfaced to the restore UI.
struct BackupFile {
    std::filesystem::path path; // absolute path to the backup .ofp (the restore source)
    std::string display;        // "YYYY-MM-DD HH:MM:SS" parsed from the filename, or the raw stem on mismatch
    std::uintmax_t bytes = 0;   // file size
};

// The backup directory for a project under <pref>/backup. Keyed by the project's *full* path, not just
// its filename stem: two projects that share a stem in different folders (a very common "video.ofp")
// must not share a backup folder, or one project's pruning would silently delete the other's history and
// the restore list would offer foreign backups to load over the wrong file. A never-saved project (empty
// path) maps to the shared "_unnamed_" bucket. Does not create the directory.
std::filesystem::path dirForProject(const std::filesystem::path &projectFile);

// Filename for a backup taken at local time `when`: "backup-YYYY-MM-DD_HH-MM-SS.ofp". Filesystem-safe
// (no ':') and lexicographically sortable, so a plain sort over filenames is chronological. Must be
// called on the main thread — localtime is not thread-safe.
std::string fileName(std::time_t when);

// The dated backups in `dir`, newest first. Cheap, disk-touching. A missing directory yields an empty
// list. Only our own "backup-*.ofp" files are reported.
std::vector<BackupFile> list(const std::filesystem::path &dir);

// Delete the oldest backups in `dir` beyond `keepCount` (clamped to >=1) so the rolling window never
// grows without bound. Only our own "backup-*.ofp" files are touched. Safe to call from a worker thread.
void prune(const std::filesystem::path &dir, int keepCount);

} // namespace ofs::backup
