#pragma once

#include "Format/BackupArchive.h" // ofs::backup::BackupFile

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
    // Rescan the current project's backup directory into `entries_`, newest first. Cheap, disk-touching:
    // called on open and when the open project changes, not every frame.
    void refresh(const ScriptProject &project);

    std::vector<backup::BackupFile> entries_;
    std::string scannedFor_; // project file path the cache was built for; a change forces a rescan
    bool loaded_ = false;    // false ⇒ rescan on the next render (covers re-open)
    int selected_ = -1;      // row index into entries_, or -1
};

} // namespace ofs
