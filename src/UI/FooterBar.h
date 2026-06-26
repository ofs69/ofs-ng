#pragma once

#include "UI/Notifications.h"
#include <cstddef>
#include <cstdint>

namespace ofs {
class EventQueue;
enum class StandardAxis : uint8_t;
} // namespace ofs

namespace ofs::ui {

// One entry in a footer dropdown (edit mode / navigator). All strings must outlive the render call
// (frame-scratch ok): `id` is the stable registry id pushed back on selection; `label` is the
// already-resolved display text (a localized native label, or a plugin's display name); `source` is
// the owning plugin's name (shown dim beside the label so a plugin-provided entry is attributable),
// empty for a native/host entry.
struct FooterSelectOption {
    const char *id = "";
    const char *label = "";
    const char *source = "";
    // True when this mode supplied an onUi (has per-mode options). Drives the small "tool options"
    // affordance the selector grows beside the *active* mode's label — present only when there is
    // something to configure, so an option-less mode adds no clutter.
    bool hasUi = false;
};

// Read-only telemetry the footer renders each frame. Plain POD (no SDL / service / ScriptProject
// types) so the renderer stays dependency-free and unit-testable — mirrors the TitleBar split. The
// caller (OfsApp) gathers these from the player, ScriptProject and ProjectManager.
struct FooterBarInfo {
    // ── Idle (no-project) override ─────────────────────────────────────────────────────────────
    // Non-empty ⇒ the welcome screen is up: render this single status line instead of the transport /
    // axis / view / eval zones below. The bell still renders. Must outlive the call (frame-scratch ok).
    const char *idleMessage = "";

    // ── App render loop (right zone, always shown — including over the welcome screen) ───────────
    float appFps = 0.0f; // smoothed UI frame rate (ImGui io.Framerate)
    bool idle = false;   // true while the loop is throttled to the idle FPS floor (waitEvents sleeping)

    // ── Transport stats (left zone) ────────────────────────────────────────────────────────────
    double fps = 0.0;           // container fps (0 = unknown / no media; hides the whole transport zone)
    float playbackSpeed = 1.0f; // shown only when != 1.0x

    // Which media source is loaded: -1 none, 0 original, 1 optimized (intra) — renders a small badge so
    // the user can always see whether the fast-seeking copy or the original is active.
    int mediaSource = -1;

    // True when optimizing is currently possible (mirrors the optimize command's gate, minus the output
    // dir which is now auto-picked on demand). When set and mediaSource == 0, the original badge becomes
    // a click target that opens the optimize dialog.
    bool canOptimize = false;

    // ── Active axis + selection (center zone) ──────────────────────────────────────────────────
    StandardAxis activeAxis{};  // active axis; the renderer looks up its short tag, tint and (hover) full name
    int activeActionCount = 0;  // points in the active axis
    int selectionCount = 0;     // selected points in the active axis
    double selectionSpan = 0.0; // seconds between first/last selected point (0 if < 2 selected)

    // ── View window (center zone, between dividers) ────────────────────────────────────────────
    double visibleTime = 0.0; // timeline zoom window, seconds

    // ── Undo history (center zone) ─────────────────────────────────────────────────────────────
    // Stack depth (steps reachable via undo / redo) plus the packed+compressed bytes the history holds
    // and its byte budget. The zone is hidden when undoMaxBytes == 0 (no undo system, e.g. before init).
    size_t undoSteps = 0;
    size_t redoSteps = 0;
    size_t undoUsedBytes = 0;
    size_t undoMaxBytes = 0;

    // ── Managed heap (right zone) ──────────────────────────────────────────────────────────────
    // Current .NET (GC) heap size in bytes. 0 ⇒ no CLR loaded (no .NET runtime) → the zone is hidden.
    long long managedHeapBytes = 0;

    // ── Background eval workers (right zone, between the heap and fps read-outs) ──────────────────
    // Eval jobs executing on a pool thread right now (0 hides the read-out), and the wall-clock age of the
    // longest-running one. When that age crosses kStuckWorkerSeconds a worker is very likely wedged (eval is
    // a frame-budget cooperative task; a minute means it is not coming back), so the read-out flags it.
    int runningWorkers = 0;
    double oldestWorkerSeconds = 0.0;

    // ── Background evaluation (center zone) ────────────────────────────────────────────────────
    // The async-job contract surfaces here instead of per-axis: how many axes currently hold a
    // pendingEval, and the first one's name for the label.
    int evaluatingCount = 0;
    const char *evaluatingAxisName = "";

    // ── Interaction extension points (center zone) ─────────────────────────────────────────────
    // The active edit mode and navigator selectors. Each option list is a frame-scratch array the
    // caller fills from the registries (native entries first); the renderer pushes a SetActive…Event
    // when the user picks a different id. Empty lists hide the respective selector.
    const FooterSelectOption *editModes = nullptr;
    int editModeCount = 0;
    const char *activeEditModeId = "";
    const FooterSelectOption *navigators = nullptr;
    int navigatorCount = 0;
    const char *activeNavigatorId = "";
    const FooterSelectOption *selectionModes = nullptr;
    int selectionModeCount = 0;
    const char *activeSelectionModeId = "";
};

// Renders a thin status strip pinned to the bottom of the main viewport (mirror of the title bar).
// `notifications` is caller-owned (like the title bar's CommandPaletteState): the footer renders the
// bell + unread badge + popup and may mutate it (clear items, reset unread).
// Returns the bar height in logical points (0 if it could not open). Performs no window/platform
// calls. Must be submitted before the dockspace each frame so it reserves work area (like the title bar).
// `eq` is the write path: the edit-mode / navigator selectors push SetActive…Event on a user pick, and
// a selector's tool-options affordance pushes OpenToolOptionsEvent (the host opens the options modal).
float renderFooterBar(const FooterBarInfo &info, NotificationState &notifications, EventQueue &eq);

// Renders the transient toast stack above the bell, growing upward (newest nearest the bell). Call
// once near the END of the frame (after all docked windows) so the toasts float on top of everything.
// Reads the bell anchor that renderFooterBar published into `notifications` this frame; mutates each
// shown item's transient toast timer (NotificationItem::shownAt) but never the log itself.
void renderToasts(NotificationState &state);

// Renders the persistent background-task stack just above the bell (below the toast stack), one panel
// per running task: label, progress bar, optional detail line, and an abort button (→ CancelTaskEvent).
// Call once near the END of the frame, BEFORE renderToasts — it publishes state.taskStackHeight so the
// toast stack can float above it. Reads the bell anchor renderFooterBar published this frame.
void renderTasks(NotificationState &state, EventQueue &eq);

} // namespace ofs::ui
