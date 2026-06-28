#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ofs {

class EventQueue;
struct ScriptProject;

// Restore-from-Backup window: lists the dated auto-backups for the currently open project and lets the
// user load one back into the session (pushes RestoreBackupRequestEvent; ProjectManager owns the restore,
// including the unsaved-changes guard). Pure renderer — its only persistent state is the cached file list,
// scanned when the window opens (and on demand), never per frame.
class BackupRestoreWindow {
  public:
    void render(bool &open, const ScriptProject &project, EventQueue &eq);

  private:
    struct Entry {
        std::string path;     // UTF-8 absolute path to the backup .ofp (the restore target)
        std::string display;  // "YYYY-MM-DD HH:MM:SS" parsed from the filename, or the raw stem on mismatch
        std::uintmax_t bytes; // file size, formatted for display per frame
    };

    // Rescan the current project's backup directory into `entries_`, newest first. Cheap, disk-touching:
    // called on open and when the open project changes, not every frame.
    void refresh(const ScriptProject &project);

    std::vector<Entry> entries_;
    std::string scannedStem_; // project stem the cache was built for; a change forces a rescan
    bool loaded_ = false;     // false ⇒ rescan on the next render (covers re-open)
    int selected_ = -1;       // row index into entries_, or -1
};

} // namespace ofs
