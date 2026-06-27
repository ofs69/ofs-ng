#pragma once

// Domain event headers, split out of this file. Events.h now carries only the cross-cutting,
// app-level events (the generic ModifyEvent<T> mutator, notifications, plugin-data, shortcut
// (re)loading) and aggregates the domain headers so existing `#include "Core/Events.h"` sites keep
// reaching every event. Prefer including the specific domain header in new code.
#include "Core/AxisEvents.h"
#include "Core/InputEvents.h"
#include "Core/IntentEvents.h"
#include "Core/LocalizationEvents.h"
#include "Core/PlaybackEvents.h"
#include "Core/RegionEvents.h"

#include "Core/FunscriptMetadata.h"
#include "Core/NotifyLevel.h"
#include "imgui.h"

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace ofs {
struct BookmarkChapterState; // forward declaration for ModifyBookmarkChapterEvent
} // namespace ofs

namespace ofs {

// Generic mutator event for passive, non-undoable state structs (metadata, the simulator/video-player
// settings copies, AppSettings). The pusher supplies a mutator; the per-type on<ModifyEvent<T>>()
// handler runs it against the one owned T and re-asserts that struct's invariants (sort order, dirty
// flag, deferred save, …).
//
// NOT for axis, region, or bookmark/chapter state: those are undoable, so they keep purpose-specific
// typed events because the event *type* (and the `snapshot` flag on ModifyRegionEvent / MoveActionEvent
// / ModifyBookmarkChapterEvent) is how UndoSystem decides when to snapshot and where gesture boundaries
// fall. Index-based edits must bounds-check inside the lambda — the handler cannot validate an opaque
// mutator.
template <typename T> struct ModifyEvent {
    std::function<void(T &)> apply;
};

// Bookmark/chapter edits are undoable, so they get their own typed event rather than ModifyEvent<T>:
// the `snapshot` flag (mirroring ModifyRegionEvent) lets UndoSystem coalesce a continuous gesture into
// one undo step. The pusher supplies an opaque mutator (same convenience as ModifyEvent — bounds-check
// inside the lambda); ProjectManager applies it and re-asserts invariants, UndoSystem snapshots only
// when snapshot=true. A discrete one-shot edit (add/remove) leaves snapshot at its default true; a
// continuous edit (color-picker drag, name typing) pushes live every frame for real-state preview but
// sets snapshot=true only on the first change so the whole gesture is a single undo step.
struct ModifyBookmarkChapterEvent {
    std::function<void(BookmarkChapterState &)> apply;
    bool snapshot = true;
};

// A plugin wrote its per-project custom data (HostApi::setProjectData). ProjectManager applies it to
// ScriptProject::pluginData[pluginName][key] and marks the project dirty. Metadata-like: NOT undoable
// (UndoSystem does not subscribe), so it survives undo/redo of unrelated edits unchanged. A null
// `value` erases the key. `pluginName` is resolved host-side from currentPluginName, so a plugin can
// only ever target its own slot — never another plugin's.
struct SetPluginProjectDataEvent {
    std::string pluginName;
    std::string key;
    nlohmann::json value; // already parsed on the ABI boundary; null = erase
};

// Pushes a user-facing message onto the footer notification center (bell + popup). Any thread may
// push it; the OfsApp handler appends it to the NotificationState on the main thread during drain().
struct NotifyEvent {
    NotifyLevel level = NotifyLevel::Info;
    std::string message;
};

struct UnregisterPluginShortcutsEvent {
    std::string group;
};

struct LoadShortcutBindingsEvent {};

// Metadata, simulator settings, and video-player settings are edited via ModifyEvent<FunscriptMetadata>
// / ModifyEvent<SimulatorState> / ModifyEvent<VideoPlayerState> (see the ModifyEvent<T> template above).
// Bookmarks/chapters are undoable, so they use the dedicated ModifyBookmarkChapterEvent instead.

} // namespace ofs
