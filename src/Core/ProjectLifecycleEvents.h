#pragma once

#include "Core/ScriptAxisAction.h"
#include "Core/StandardAxis.h"
#include "Core/VectorSet.h"

#include <optional>
#include <string>
#include <vector>

namespace ofs {

struct LoadProjectEvent {};

struct SaveProjectEvent {
    bool saveAs = false;
};

// ── Project lifecycle request events (UI pushes; ProjectManager handles) ──────
// Merged New+Open entry: one file picker decides intent — an .ofp opens that project, a video
// starts a fresh one. Replaces the old separate New/Open menu items.
struct OpenOrNewProjectRequestEvent {};

struct OpenProjectRequestEvent {
    std::string path; // a specific project (recent list, startup); empty falls back to the merged picker
};

// Create a fresh, media-less project straight away (the welcome screen's "Create Empty Project" button).
// Unlike OpenOrNewProjectRequestEvent this skips the file picker and the "create empty project?" confirm.
struct CreateEmptyProjectEvent {};

// A file dropped onto the window (welcome screen only). ProjectManager dispatches by extension, exactly
// like the merged Open/New picker: .ofp opens, .funscript starts a project around the script, anything
// else is treated as media.
struct OpenDroppedFileEvent {
    std::string path; // UTF-8 absolute path from the OS drag-and-drop
};

// Clear the recent-projects list (the welcome screen's only management affordance for it). Handled by
// OfsApp, which owns AppSettings.
struct ClearRecentProjectsEvent {};

// Drop a single entry from the recent-projects list (welcome screen context menu). Handled by OfsApp,
// which owns AppSettings.
struct RemoveRecentProjectEvent {
    std::string path; // UTF-8 path of the entry to remove
};

// A project file was just opened or saved-as; promote its path to the front of the recent-projects
// list. Pushed by ProjectManager whenever project.state.filePath is (re)assigned; handled by OfsApp,
// which owns AppSettings, so the welcome screen reflects it live (not only after a restart).
struct RememberRecentProjectEvent {
    std::string path; // UTF-8 absolute path to the .ofp
};

// Restore a chosen auto-backup into the current editing session (the Restore-from-Backup window). The
// backup's contents replace the project, but its file association is retargeted to the project the
// backup belongs to (the path open at request time) so a following Save writes back to the real file,
// not the dated backup. Goes through the unsaved-changes guard like any other open. Empty path → the
// backup was made for an unnamed project; the restore stays untitled until the user saves.
struct RestoreBackupRequestEvent {
    std::string backupPath; // UTF-8 absolute path to the dated backup .ofp to load
};

struct CloseProjectRequestEvent {};

// A project was explicitly closed by the user (the close went through, unsaved guard cleared). Pushed by
// ProjectManager; handled by OfsApp to clear AppSettings::reopenLastProject so the next launch starts on
// the welcome screen instead of silently reopening the project the user just dismissed. Distinct from the
// open/new flow's internal doClose() (which is followed by a load that re-arms the flag).
struct ProjectClosedEvent {};

struct RequestExitEvent {};

struct ExitConfirmedEvent {};

struct ImportFunscriptRequestEvent {};

// Carries a parsed funscript ready to place, pushed by the importFunscript() coroutine once the file
// dialog + load have finished (the request event only kicks off that I/O). Splitting the apply into
// its own event makes a whole import one undoable step: UndoSystem snapshots on this event — it
// registers before ProjectManager, so the snapshot captures the pre-import state — then ProjectManager
// places every axis in a single handler. The tag→role matching is already resolved on the I/O side:
// `role` holds the matched standard (non-scratch) axis to overwrite, or nullopt for "first free scratch
// slot" (single-axis imports and any unmatched / scratch-tagged entry).
struct ImportFunscriptDataEvent {
    struct Axis {
        std::optional<StandardAxis> role; // matched standard axis, or nullopt → first free scratch slot
        VectorSet<ScriptAxisAction> actions;
    };
    std::vector<Axis> axes;
};

struct ExportFunscriptRequestEvent {
    std::vector<StandardAxis> axes;
    int format; // 0 = funscript 1.0 per-file, 1 = 1.1 multi-axis, 2 = 2.0 channels
    // Engaged → write here and skip the file picker (Quick Export re-export). UTF-8: a folder for
    // format 0, a single .funscript file for formats 1/2. Empty optional → prompt as usual.
    std::optional<std::string> targetPath;
};

// Re-export with the project's remembered ExportConfig, no dialog. If nothing has been exported yet
// the handler opens the regular Export modal instead. Bound to Ctrl+Shift+S.
struct QuickExportEvent {};

} // namespace ofs
