#pragma once

#include "Core/StandardAxis.h"

#include <string>

namespace ofs {

// Interaction-intent request events: the stable vocabulary a plugin can override, kept in its own
// header (not the already-large Events.h) per the domain-grouped split convention. An intent is a
// *request*, deliberately distinct from the mutation/seek event that applies it — those resolved
// outputs (SeekEvent, AddActionAtTimeEvent, …) stay in Events.h.

// What a step request targets — the channel the active navigator resolves. The native navigator gives
// each its own built-in resolution (overlay grid, or adjacent action); a plugin navigator receives the
// granularity and may redefine one channel while Passing the others through to native. Values mirror
// NavGranularity in Ofs.Api/Navigation.cs and the granularity field on OfsNavIntent — keep the order
// in lockstep.
enum class StepGranularity {
    Frame,         // ← / →        : step the overlay grid (frame interval, or tempo beat)
    Action,        // ↓ / ↑        : step to the adjacent action on the active axis
    ActionAllAxes, // Ctrl+↓/Ctrl+↑ : step to the nearest adjacent action across all axes
};

// A step / nudge direction. The underlying values are the wire contract: ±1 is the step sign (used
// directly as `direction × reps × stepUnit`), and None (0) is EditIntent's position-nudge sentinel
// (no time nudge — `pos` carries the delta). The genuinely-binary sites (StepRequestEvent,
// MoveSelectionTimeEvent, CustomCommand) only ever carry Backward/Forward; None is an EditIntent-only
// state. Cast to `int` for arithmetic, exactly as the value is the sign.
enum class StepDirection : int {
    Backward = -1,
    None = 0, // EditIntent: a position nudge, not a time nudge
    Forward = 1,
};

// Navigation intent: the user asked to step the playhead (the prev/next-step keys). The active
// navigator resolves a Step into an absolute target and the host applies it as a SeekEvent; the
// NavigatorRouter is its sole subscriber (the navigation seam). Named ...RequestEvent — parallel to
// EditRequestEvent — so its role as an intent request (distinct from the SeekEvent that applies it)
// is clear at every call site.
struct StepRequestEvent {
    StepDirection direction; // Forward (next) / Backward (prev)
    // Steps to advance in one event; a held-repeat burst collapses N fires into reps=N (one SeekEvent).
    int reps = 1;
    // Which step channel this request targets (frame grid vs adjacent action). The navigator resolves
    // each channel; ActionAllAxes ignores reps (a single nearest-across-axes step).
    StepGranularity granularity = StepGranularity::Frame;
};

// ── Edit intents ────────────────────────────────────────────────────────────────────────────────
// A request to mutate actions, owned by the active edit mode. EditIntentRouter is the sole subscriber
// to EditRequestEvent (the editing seam): it consults the active mode, then emits the existing
// mutation event(s) the intent resolves to (AddActionAtTimeEvent, MoveActionEvent, …) which
// ProjectManager and UndoSystem still consume unchanged. The intent vocabulary mirrors the future ABI
// struct OfsEditIntent (a flat tagged record) so the C++ seam and the plugin boundary stay aligned.

enum class EditIntentKind {
    AddPoint,           // axis, time, pos           → AddActionAtTimeEvent
    AddPointAtPlayhead, // pos (axis = active)       → AddActionAtTimeEvent at the cursor (resolve sugar)
    MovePoint,          // axis, fromTime, time, pos → MoveActionEvent
    RemovePoint,        // axis, time                → RemoveActionAtTimeEvent
    RemoveSelected,     // axis                      → RemoveSelectedActionsEvent
    // A keyboard nudge of the selection: a time nudge when direction != 0 (carries direction/reps), else a
    // position nudge (pos = delta). Native resolution → MoveSelectionTimeEvent / MoveSelectionPositionEvent.
    // The router presents a *single-action* nudge to the active mode as a MovePoint, so a mode implementing
    // only MovePoint governs single-action keyboard moves too (see EditIntentRouter::onEditRequest).
    MoveSelection,
    Paste, // time (= pasteTime), exact → PasteActionsEvent
    // (No MovePointToPlayhead: "move action to the playhead" is a plain command that pushes
    //  MoveActionToCurrentTimeEvent directly, never routed through a mode.)
};

// Gesture phase on the request envelope, set by the source that owns the gesture boundary. The router
// keeps a per-gesture snapshot latch off this: the first mutation it emits for a Begin/OneShot gesture
// carries snapshot=true (a fresh undo step), every later mutation of the same gesture carries
// snapshot=false (coalesced). The snapshot flag is therefore host-stamped here, never part of the
// intent itself — a plugin mode cannot break undo coalescing by mishandling a flag it never holds.
enum class GesturePhase {
    OneShot,  // a discrete, self-contained edit (tap, menu, keypress) — its own one-mutation undo step
    Begin,    // first frame of a continuous gesture (drag, hold) — opens a coalesced undo step
    Continue, // a later frame of the gesture already opened by Begin — folds into that step
};

// One edit intent: a flat tagged record. Only the fields named for `kind` (see EditIntentKind) are
// meaningful; the rest stay at their defaults. Matches OfsEditIntent's shape so resolving it native
// and marshaling it to a plugin mode read the same fields.
struct EditIntent {
    EditIntentKind kind;
    StandardAxis axis = StandardAxis::L0;
    double time = 0.0;     // AddPoint/RemovePoint: target; MovePoint: toAt; Paste: pasteTime
    double fromTime = 0.0; // MovePoint: fromAt
    int pos = 0;           // AddPoint/AddPointAtPlayhead: pos; MovePoint: toPos; MoveSelection: pos delta
    StepDirection direction =
        StepDirection::None; // MoveSelection: Forward/Backward time nudge; None ⇒ a position nudge
    int reps = 1;            // MoveSelection (time nudge): held-repeat burst count
    bool exact = false;      // Paste: paste at original times
    bool seekAfter = false;  // MoveSelection (time nudge): seek to the moved selection afterward
};

// The editing seam's request event. The sole subscriber is EditIntentRouter; every gesture source
// (timeline mouse, keyboard command, palette, plugin) pushes this instead of a mutation event so the
// active mode is consulted exactly once, source-agnostically.
struct EditRequestEvent {
    EditIntent intent;
    GesturePhase gesture = GesturePhase::OneShot;
};

// ── Selection intents ───────────────────────────────────────────────────────────────────────────
// A request to *author* a selection (which actions become selected), owned by the active selection
// mode. SelectIntentRouter is the sole subscriber to SelectRequestEvent (the selection seam): it
// resolves the gesture per-axis — natively, or through the active mode — and applies the result via
// project.setSelection(); the per-axis fan-out and the additive combine live in the router. The
// wholesale per-axis SetAxisSelectionEvent (a plugin AxisEdit / a deselect-all) stays its own event —
// it sets a selection rather than authoring one from a gesture.

// Which selection-authoring gesture a request carries; the active mode reinterprets what it means,
// the native resolver reproduces today's behavior. Mirrors OfsSelectGesture in Ofs.Api.
enum class SelectGesture {
    Box,   // marquee drag / programmatic time-range: every action with time in [startTime,endTime]
    All,   // select-all (Ctrl+A): every action on the axis
    Point, // click / Ctrl-click: the action at {startTime,pos} (a degenerate single-time range)
};

// The selection seam's request event. Every gesture that authors a selection (marquee, Ctrl+A,
// click/Ctrl-click) pushes this instead of mutating the selection, so the active mode is consulted
// once per editable axis, source-agnostically. `additive` (Ctrl held) is the host-owned combine
// applied below the seam (toggle into the current selection vs. replace it); `pos` is carried for a
// plugin mode — the native resolver ignores it, exactly as box-select ignores its vertical extent today.
struct SelectRequestEvent {
    SelectGesture gesture;
    StandardAxis axis;      // lead axis the user gestured on; the router fans across its edit group
    double startTime = 0.0; // Box: range start; Point: the clicked time (== endTime)
    double endTime = 0.0;   // Box: range end
    int pos = 0;            // Point: the clicked pos
    bool additive = false;  // Ctrl held: toggle into the current selection instead of replacing it
};

// ── Extension-point selection ─────────────────────────────────────────────────────────────────────
// Host-owned activation of the active edit mode / navigator / selection mode, written ONLY by the
// footer selectors — there is no plugin-callable setter, so two plugins can never fight over the
// selection. Each is handled solely by its router, which validates the id against its registry and
// writes the ScriptProject field; the edit router also runs the outgoing/incoming onExit/onEnter. Not
// an intent — it configures which resolver the intent events above route through, rather than
// requesting a mutation.
struct SetActiveEditModeEvent {
    std::string id;
};
struct SetActiveNavigatorEvent {
    std::string id;
};
struct SetActiveSelectionModeEvent {
    std::string id;
};

// Which extension point's active-mode options a request targets. One per footer selector / per
// active mode — the tool-options modal is per-mode, so this names which one to open.
enum class ToolOptionTarget { Edit, Navigator, Selection };

// Open the active mode's options for one extension point as the click-away tool-options modal. Pushed
// by the footer selector's options affordance and by the dynamic "Tool Options" palette commands; the
// host (OfsApp) raises the modal. Not an intent — it surfaces a mode's options, routing no mutation.
struct OpenToolOptionsEvent {
    ToolOptionTarget target;
};

} // namespace ofs
