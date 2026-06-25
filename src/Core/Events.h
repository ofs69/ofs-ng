#pragma once

#include "Core/FunscriptMetadata.h"
#include "Core/IntentEvents.h"
#include "Core/NotifyLevel.h"
#include "Core/OverlaySettings.h"
#include "Core/ProcessingRegion.h"
#include "Core/ScriptAxisAction.h"
#include "Core/SimulatorSettings.h"
#include "Core/StandardAxis.h"
#include "Core/VectorSet.h"
#include "Video/VideoMode.h"
#include "imgui.h"
#include <nlohmann/json.hpp>

#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keycode.h>
#include <functional>

namespace ofs {
class EventQueue;
struct EvalJob;              // forward declaration for EvalCompleteEvent
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

// Posted by OfsApp::onEvent from SDL_EVENT_KEY_DOWN. keyboardCaptured is set from
// ImGui::GetIO().WantCaptureKeyboard at the time the SDL event fires (previous frame's ImGui
// state). Processed during drain(); BindingSystem is the sole subscriber.
struct KeyDownEvent {
    SDL_Keycode key;
    SDL_Keymod modifiers;
    bool repeat;
    bool keyboardCaptured = false;
};

// Posted by OfsApp::onEvent from SDL_EVENT_KEY_UP, mirroring KeyDownEvent. No keyboardCaptured field:
// a key-up must always be delivered so an in-flight Hold binding cannot get stuck when ImGui capture
// changes mid-press. BindingSystem is the sole subscriber (ends the matching active hold).
struct KeyUpEvent {
    SDL_Keycode key;
    SDL_Keymod modifiers;
};

struct PlayPauseEvent {};

// Posted by OfsApp::onEvent from SDL_EVENT_GAMEPAD_BUTTON_DOWN. gamepadCaptured mirrors
// ImGui::GetIO().WantCaptureGamepad at event time. BindingSystem is the sole subscriber.
struct GamepadButtonEvent {
    SDL_JoystickID which;
    SDL_GamepadButton button;
    bool gamepadCaptured = false;
};

// Posted by OfsApp::onEvent from SDL_EVENT_GAMEPAD_BUTTON_UP, mirroring KeyUpEvent. Always delivered
// (no capture gate) so an in-flight Hold binding on a gamepad button cannot get stuck. BindingSystem
// is the sole subscriber (ends the matching active hold).
struct GamepadButtonUpEvent {
    SDL_JoystickID which;
    SDL_GamepadButton button;
};

// Requests the title-bar command palette to open this frame. Pushed by the rebindable "open command
// palette" shortcut; OfsApp latches it and forwards it to renderTitleBar.
struct OpenCommandPaletteEvent {};

// Pushes a user-facing message onto the footer notification center (bell + popup). Any thread may
// push it; the OfsApp handler appends it to the NotificationState on the main thread during drain().
struct NotifyEvent {
    NotifyLevel level = NotifyLevel::Info;
    std::string message;
};

struct LoadVideoEvent {
    std::string path;
};

struct CloseVideoEvent {};

struct VolumeChangedEvent {
    float volume;
};

struct SeekEvent {
    double time;
};

// Hover-scrub request for the timeline frame preview. Pushed every hover frame by the seek-bar UI;
// VideoPreview coalesces (latest-wins, settle-gated) so fast flicks drop intermediate targets.
struct PreviewSeekRequestEvent {
    double time;
};

// Enable/disable the hover frame preview. Pushed by OfsApp when AppSettings.showTimelinePreview
// changes (and once at startup). VideoPreview lazily creates its mpv engine on enable and tears it
// down on disable; handled on the main thread during drain (mpv create/destroy is main-thread).
struct SetPreviewEnabledEvent {
    bool enabled;
};

struct PlaybackSpeedEvent {
    float speed;
};

struct AxisSelectedEvent {
    StandardAxis role;
};

// Set the multi-axis editing group (timeline strip Ctrl-click / drag). `roles` is the desired group;
// an empty/single-bit `roles` dissolves it back to single-axis editing. The handler keeps activeAxis
// (the lead) coherent with the group. Not undoable — grouping is transient view state.
struct SetAxisGroupingEvent {
    AxisRoles roles;
    StandardAxis lead; // becomes activeAxis
};

struct AxisModifiedEvent {
    StandardAxis role;
};

// Manual recompute request (processing panel "Recompute" button). Forces re-evaluation of the axis
// even while auto-eval is halted; unlike AxisModifiedEvent it carries no document mutation.
struct RequestAxisEvalEvent {
    StandardAxis role;
};

// Toggle automatic re-evaluation on axis edits. Transient session state (ScriptProject::autoEvalEnabled).
struct SetAutoEvalEnabledEvent {
    bool enabled;
};

struct SelectionChangedEvent {
    StandardAxis role;
};

struct UndoEvent {};
struct RedoEvent {};

struct RemoveSelectedActionsEvent {
    StandardAxis axis;
    bool fanToGroup = true; // see AddActionAtTimeEvent::fanToGroup
    bool snapshot = true;   // see AddActionAtTimeEvent::snapshot — false on the 2nd..Nth mutation of a Replace batch
};

struct AddActionAtTimeEvent {
    StandardAxis axis;
    double time;
    int pos;
    // Default true ⇒ a normal single add captures its own pre-edit undo snapshot. When an edit mode
    // resolves one gesture into several adds (e.g. Ofs.Core's Shaped Approach), the router sets this
    // true only on the first and false on the rest, so UndoSystem captures once and the whole batch
    // coalesces into a single undo step instead of one per injected point.
    bool snapshot = true;
    // Default true ⇒ an active-axis mutation projects across the active edit group below the seam
    // (ProjectManager fans it via editTargets/effectiveEditSet). The EditIntentRouter sets it false only
    // for a ReplacePerAxis resolution, where it has already consulted the mode once per editable axis and
    // each axis's intents must apply verbatim to that axis alone — no mechanical projection.
    bool fanToGroup = true;
};

struct OverlaySettingsChangedEvent {
    OverlayState state;
};

struct PlayStateChangedEvent {
    bool playing;
};

struct SpeedChangedEvent {
    float speed;
};

struct MediaChangedEvent {
    std::string path;
};

struct DurationChangedEvent {
    double duration;
};

struct MoveSelectionPositionEvent {
    StandardAxis axis;
    int delta;
    // A held-repeat burst coalesces into one event; only the first fire of the hold snapshots undo, so
    // the whole hold is a single undo step. The caller pre-scales `delta` by the burst's repeat count
    // (delta is already a signed magnitude). Default true ⇒ every other caller (single tap, menu) snapshots.
    bool snapshot = true;
    bool fanToGroup = true; // see AddActionAtTimeEvent::fanToGroup
};

struct MoveSelectionTimeEvent {
    StandardAxis axis;
    StepDirection direction; // Forward / Backward
    bool seekAfter;
    // Number of step units to move in one event (a held-repeat burst collapses N fires into reps=N).
    // delta = direction * reps * stepTime — one mutate and one collision check at the final position.
    int reps = 1;
    bool snapshot = true;   // see MoveSelectionPositionEvent::snapshot
    bool fanToGroup = true; // see AddActionAtTimeEvent::fanToGroup
};

struct MoveActionToCurrentTimeEvent {
    StandardAxis axis;
    bool fanToGroup = true; // see AddActionAtTimeEvent::fanToGroup
};

// Copy the current selection of every axis in the active group into ProjectManager's clipboard. No
// mutation (not undoable). Cut = this followed by RemoveSelectedActionsEvent.
struct CopySelectionEvent {};

// Paste ProjectManager's clipboard. A one-axis clipboard is broadcast across the active group; a
// multi-axis clipboard pastes each clip back onto its originating role. exact = at original times.
struct PasteActionsEvent {
    double pasteTime;
    bool exact;
    bool snapshot = true; // see AddActionAtTimeEvent::snapshot — false on the 2nd..Nth mutation of a Replace batch
};

struct SimulatorPositionChangedEvent {
    SimulatorState state;
};

struct CreateRegionEvent {
    StandardAxis axisRole; // lead — drives the present-check and is always part of the region
    AxisRoles axisRoles;   // full spanned set; empty default ⇒ just {axisRole}
    double startTime;
    double endTime;
    // End of the timeline (video/dummy duration). Bounds where a region may be placed so that
    // snapping a region forward past an occupied anchor can't run it off the end of the media.
    // Defaults to unbounded for programmatic callers that have no timeline length to enforce.
    double timelineDuration = std::numeric_limits<double>::max();
};

struct DeleteRegionEvent {
    int regionId;
};

struct ModifyRegionEvent {
    int regionId;
    ProcessingRegion updatedRegion;
    bool snapshot = true;
};

struct MoveRegionNodesEvent {
    int regionId;
    ProcessingRegion updatedRegion; // only nodeGraph.nodes[*].posX/posY are read
};

struct BakeRegionEvent {
    int regionId;
};

struct AssignAxisToRegionEvent {
    int regionId;
    StandardAxis axis;
    bool assign; // true = add this axis to the region, false = remove
};

struct RemoveActionAtTimeEvent {
    StandardAxis axis;
    double time;
    bool fanToGroup = true; // see AddActionAtTimeEvent::fanToGroup
    bool snapshot = true;   // see AddActionAtTimeEvent::snapshot — false on the 2nd..Nth mutation of a Replace batch
};

struct MoveActionEvent {
    StandardAxis axis;
    double fromAt;
    double toAt;
    int toPos;
    bool snapshot = true;
    bool fanToGroup = true; // see AddActionAtTimeEvent::fanToGroup
};

// Axis management
struct AddScratchAxisEvent {};

struct RemoveAxisEvent {
    StandardAxis axisRole;
};

struct ToggleAxisVisibilityEvent {
    StandardAxis axisRole;
    bool visible;
};

struct ToggleAxisLockEvent {
    StandardAxis axisRole;
    bool locked;
};

struct ToggleAxisPanelVisibilityEvent {
    StandardAxis axisRole;
    bool inPanel;
};

// QOL: make the six main TCode axes (L0–R2) present in one step, so a project that started with only L0
// is ready for multi-axis editing. Not-yet-present axes are created with default flags; already-present
// ones are left untouched. Pushed by the Axes menu "Multi-Axis" item.
struct ShowMultiAxisEvent {};

// QOL counterpart to ShowMultiAxisEvent: collapse the strip back to just L0, hiding every other axis
// from the panel (and dropping them from any edit group). L0 itself is forced visible. Data is never
// cleared: a scratch axis that holds actions still exists() and stays in the Axes menu (re-show via
// "Show in Panel"); an empty scratch axis simply ceases to exist. Pushed by the Axes menu "Show L0
// Only" item.
struct ShowL0OnlyEvent {};

struct VideoModeChangedEvent {
    VideoMode mode;
};

struct VideoResolutionChangedEvent {
    float scale;
};

// Render-target size the video window wants this frame (derived from its content region). The
// player handles it idempotently, so the window pushes only when the requested size changes.
struct SetRenderSizeEvent {
    int width;
    int height;
};

struct ClearRegionSelectionEvent {};

struct SelectRegionEvent {
    int regionId;
};

struct UpdateTimelineViewEvent {
    double visibleTime;
    double offsetTime;
};

struct SetTimelineShowPointsEvent {
    bool show;
};

struct SetTimelineShowWaveformEvent {
    bool show;
};

struct EvalCompleteEvent {
    StandardAxis role;
    std::shared_ptr<EvalJob> job; // compared against AxisState::pendingEval for staleness
    VectorSet<ScriptAxisAction> resolvedActions;
    double evalMs = 0.0;
    bool hasResult = false; // false means no regions; resolvedActions is empty
};

struct UnregisterPluginShortcutsEvent {
    std::string group;
};

struct LoadShortcutBindingsEvent {};

// Metadata, simulator settings, and video-player settings are edited via ModifyEvent<FunscriptMetadata>
// / ModifyEvent<SimulatorState> / ModifyEvent<VideoPlayerState> (see the ModifyEvent<T> template above).
// Bookmarks/chapters are undoable, so they use the dedicated ModifyBookmarkChapterEvent instead.

struct ChangeDummyDurationEvent {
    double durationSeconds;
};

struct ChangeMediaPathEvent {
    std::string path; // empty = unload video
};

struct CommitAxisActionsEvent {
    StandardAxis axis;
    VectorSet<ScriptAxisAction> actions;
};

// Replace an axis's selection wholesale (empty = clear). Deferred like CommitAxisActionsEvent so that a
// plugin's buffered AxisEdit applies atomically: the actions commit drains first, then this selects
// against the freshly-committed set — otherwise a selection of just-created points would be filtered out
// (setSelection drops times not yet in actions).
struct SetAxisSelectionEvent {
    StandardAxis axis;
    VectorSet<ScriptAxisAction> selection;
};

// ── Localization events (UI pushes; handled in the OfsApp composition root) ────
struct SetLanguageEvent {
    std::string languageId; // "" / "en" = built-in English
};

struct ExportCatalogEvent {
    std::string path; // destination .toml for a fresh annotated catalog
};

struct RefreshTranslationEvent {
    std::string path; // existing translation .toml to merge against the current source
};

} // namespace ofs
