#pragma once

#include <cstddef> // offsetof

namespace ofs {

// ── ABI version ───────────────────────────────────────────────────────────────
// Guards the *native ↔ managed* boundary: the raw memory layout of HostApi and
// PluginApi below must match their C# mirrors in Ofs.Api/PluginAbi.cs. The native
// exe, Ofs.Api.dll and Ofs.PluginHost.dll always ship together, so a mismatch here
// means a corrupted/partial install (e.g. a stale managed/ folder against a freshly
// built exe) — caught early instead of as silent memory corruption.
//
// This is a SEPARATE concern from plugin compatibility: whether a *plugin* was built
// against a compatible Ofs.Api is guarded by the assembly version of Ofs.Api itself,
// checked in PluginBootstrapper. Keep this constant in step with the major component
// of Ofs.Api's AssemblyVersion. Bump it whenever either struct's layout changes.
constexpr int OFS_ABI_VERSION = 1;

struct PluginAction {
    double at;
    int pos;
};

// ── Plugin node C ABI ─────────────────────────────────────────────────────────

// Opaque handles — concrete definitions in PluginNodeIO.h.
// Valid only for the duration of the evaluation callback (worker thread).
struct OfsDiscreteInput;
struct OfsDiscreteOutput;

// Float-backed param kinds for script-node header params: Float/Int store the
// value directly (host drag/slider), Bool stores 0/1 (host checkbox), Enum stores the (float)index (host
// combo over declared labels). Plugin nodes have no scalar params — their state is a typed TState.
typedef enum { OfsParamFloat = 0, OfsParamInt = 1, OfsParamBool = 2, OfsParamEnum = 3 } OfsParamType;

// Pure-value evaluation context — no pointers to live project state; safe on any thread
typedef struct {
    double regionStart;
    double regionEnd;
    const float *params; // length = paramCount; int params stored as (float)intVal
    int paramCount;
    int stateHandle; // ≥0 → index into the managed TState capture set for this eval; -1 = none
                     // (native effect / script node). Set on the main thread at snapshot build,
                     // read by the managed eval trampoline to find the node's captured TState.

    // Cooperative cancellation, safe to read from the worker thread. Points at the eval's cancel flag:
    // 0 = run, nonzero = this eval was superseded (a newer edit). A long discrete generator/modifier
    // SHOULD poll it at loop boundaries and return early — the host discards a cancelled eval's output,
    // so bailing only frees the worker sooner. The flag is monotonic (set once, never cleared), so a stale
    // read costs at most one extra iteration. null for evals with no owning job (e.g. a single functional
    // sample).
    const volatile int *cancelFlag;
} OfsEvalCtx;

typedef enum { OfsSignalDiscrete = 0, OfsSignalFunctional = 1 } OfsSignalKind;

// Funscript on-disk format selected by getFunscriptJson. 1.0 is single-axis (root "actions" only); 1.1
// carries extra axes in an "axes" array; 2.0 in a "channels" object. Mirrored as FunscriptVersion in
// Ofs.Api (Project.cs).
typedef enum { OfsFunscript10 = 0, OfsFunscript11 = 1, OfsFunscript20 = 2 } OfsFunscriptVersion;

// A node declares N input pins and M output pins (see OfsNodeDef). Both directions are capped at 16 so
// every imnodes pin id fits the 6-bit slot field of the host's GraphId encoding (see ProcessingRegion.h).
#define OFS_MAX_NODE_PINS 16

// Evaluation callbacks — called from a worker thread. Both take input/output arrays so one signature
// serves any (inputCount, outputCount) shape. Must not call any HostApi function except
// nodeInputCount/nodeInputTime/nodeInputPosition/nodeAddAction.
//
// Discrete: read `inCount` input action lists, write `outCount` output action lists. Once per region.
typedef void (*OfsDiscreteNodeFn)(const OfsDiscreteInput *const *ins, int inCount, const OfsEvalCtx *ctx,
                                  OfsDiscreteOutput *const *outs, int outCount, void *ud);
// Functional: given t, read `inCount` input values, write `outCount` output values (0..100). Sampled
// pointwise.
typedef void (*OfsFunctionalNodeFn)(double t, const float *ins, int inCount, const OfsEvalCtx *ctx, float *outs,
                                    int outCount, void *ud);

// Type-erased eval-callback pointer stored in OfsNodeDef.fn; cast to OfsDiscreteNodeFn /
// OfsFunctionalNodeFn by the def's `signal`. A function pointer (not void*) so the cast stays
// function-pointer-to-function-pointer (warning-clean under /WX).
typedef void (*OfsNodeEvalFn)(void);

// Custom node-body UI callback — main thread, called every frame while the node is visible. The
// wrapper draws the node's widgets and reports whether the state changed this frame (1 = changed →
// the host persists it and re-evaluates). null for a plain scalar node with no custom UI.
typedef int (*OfsNodeUiFn)(void *ctx, void *userData);

// The icon a node shows in the add-node palette and on its title bar — a CURATED, closed subset of the
// host's glyph set, exposed by value so a plugin never needs the host's icon-font codepoints (and so the
// host can re-skin a glyph without breaking plugins). Default → the host picks by arity (Generate /
// Modify / Combine). Mirrored as the public `NodeIcon` enum in Ofs.Api (Nodes.cs); the host maps each
// value to a glyph in the UI layer. Append-only: a host that doesn't know a newer value falls back to
// Default, so a plugin built against a newer enum degrades gracefully on an older host.
typedef enum {
    OfsNodeIconDefault = 0, // host picks by arity
    OfsNodeIconWaveform = 1,
    OfsNodeIconSliders = 2,
    OfsNodeIconFilter = 3,
    OfsNodeIconCurve = 4,
    OfsNodeIconActivity = 5,
    OfsNodeIconGauge = 6,
    OfsNodeIconMath = 7,
    OfsNodeIconFunction = 8,
    OfsNodeIconMerge = 9,
    OfsNodeIconSplit = 10,
    OfsNodeIconBlend = 11,
    OfsNodeIconCombine = 12,
    OfsNodeIconScale = 13,
    OfsNodeIconRuler = 14,
    OfsNodeIconMagnet = 15,
    OfsNodeIconPercent = 16,
    OfsNodeIconRepeat = 17,
    OfsNodeIconShuffle = 18,
    OfsNodeIconRandom = 19,
    OfsNodeIconWand = 20,
    OfsNodeIconTrend = 21,
    OfsNodeIconZap = 22,
    OfsNodeIconBars = 23,
    OfsNodeIconMove = 24,
} OfsNodeIcon;

typedef struct {
    const char *id;          // local id; host prepends plugin name → "pluginname.id"
    const char *displayName; // rendered verbatim (not catalog-localized; see the conventions block)
    // Per-pin names — `inputNames[0..inputCount)` and `outputNames[0..outputCount)`. Required (every pin
    // is named); rendered verbatim as pin labels. Copied at registration; NOT serialized (a disabled
    // plugin's node falls back to index labels from the persisted pin counts). inputNames may be null
    // only when inputCount == 0.
    const char *const *inputNames;
    const char *const *outputNames;
    OfsNodeEvalFn fn; // cast to OfsDiscreteNodeFn / OfsFunctionalNodeFn by `signal`
    void *userData;
    OfsNodeUiFn onNodeUi; // null = no body UI (a TState node that supplied no `ui` callback)
    int signal;           // OfsSignalKind: Discrete | Functional
    int inputCount;       // 0..OFS_MAX_NODE_PINS input pins
    int outputCount;      // 1..OFS_MAX_NODE_PINS output pins
    int hasState;         // 1 → TState-backed registration: the managed wrapper owns capture/encode/decode,
                          // and the host carries the node's state as JSON in ProcessingGraphNode::nodeState
    // Palette presentation (both optional).
    const char *group;       // add-node menu group header (rendered verbatim like displayName); null/"" → defaults
                             // to the plugin name. Lets one plugin split its nodes across groups, or several
                             // plugins share a group.
    OfsNodeIcon icon;        // leaf glyph in the palette and on the node title bar; OfsNodeIconDefault → the
                             // arity-bucket icon (Generate/Modify/Combine).
    const char *description; // add-node menu hover tooltip (rendered verbatim like displayName); null/"" → none.
} OfsNodeDef;

// Command registration — main thread only, call from onLoad.
// `id` is a local id; the host prepends the plugin name → "<plugin>.<id>".
// `inRebindList = 0` (the C# default) means palette-only — not offered in the rebind UI.
// `inPalette` and `inRebindList` must not both be 0 — such a command appears on no surface, so the
// user could never discover it to bind or invoke; the host rejects the definition.
// Struct layout is exact-size-asserted (see bottom of file); appending a field is a coordinated
// ABI break across both sides, not a silently-additive change. Grow the surface via a new HostApi
// function pointer or a trailing enum value instead.
typedef struct {
    const char *id;
    const char *group; // palette category; if null, defaults to plugin name
    const char *title;
    int inRebindList; // 0 = not listed in the rebind UI by default; 1 = offered for binding there
    int inPalette;    // 0 = hidden from the command palette; 1 = searchable in the palette
} OfsCommandDef;

// registerCommand result codes (returned as int). 0 = success; each failure has a distinct code so the
// managed wrapper can raise a specific, accurate exception instead of one blanket "id already taken".
// Mirrored in Ofs.Api/Commands.cs (RegisterResult) — keep the two in sync.
typedef enum {
    OfsRegisterCommandOk = 0,
    OfsRegisterCommandErrWrongThread = 1,  // not called on the main thread
    OfsRegisterCommandErrNotOnLoad = 2,    // called at runtime, not from onLoad
    OfsRegisterCommandErrInvalidArg = 3,   // null/empty def, id, or title
    OfsRegisterCommandErrNotInvokable = 4, // listed on no surface (not in rebind UI nor palette) → undiscoverable
    OfsRegisterCommandErrDuplicateId = 5,  // a command with this id is already registered
} OfsRegisterCommandResult;

// Async native-dialog result callbacks. The host invokes these exactly once, on the main thread, on a
// LATER frame once the user has answered — so a plugin's dialog never blocks the frame loop. `userData`
// is the pointer the plugin passed alongside the callback. For the file/folder dialogs `path` is null
// when the user cancelled. The C# wrapper bridges these into an awaitable Task.
typedef void (*OfsFileResultFn)(void *userData, const char *path);
typedef void (*OfsConfirmResultFn)(void *userData, int result); // result: 1 = OK/Yes, 0 = Cancel/No
// Multi-select open result: `paths` is an array of `count` UTF-8 paths; `count` is 0 (and `paths` may be
// null) when the user cancelled or chose nothing. Both array and strings are valid only for the call.
typedef void (*OfsFilesResultFn)(void *userData, const char *const *paths, int count);

// ── Interaction intents (edit modes + navigators) ───────────────────────────────
// The stable vocabulary a plugin's active edit mode / navigator overrides. These mirror the C++ seam
// structs in src/Core/IntentEvents.h (EditIntent / StepRequestEvent) and the C# mirror in
// Ofs.Api/PluginAbi.cs. Activation is host-owned (the footer selectors) — there is no plugin-callable
// setter, so registering a mode/navigator only *publishes* it.

// Edit-intent kind — order MUST match EditIntentKind in IntentEvents.h (the router maps by value).
// A single-action MoveSelection nudge is delivered to the mode as OfsEditMovePoint, so a mode that only
// handles MovePoint governs single-action keyboard moves too; only a multi-action nudge arrives as
// OfsEditMoveSelection.
typedef enum {
    OfsEditAddPoint = 0,
    OfsEditAddPointAtPlayhead,
    OfsEditMovePoint,
    OfsEditRemovePoint,
    OfsEditRemoveSelected,
    OfsEditMoveSelection,
    OfsEditPaste,
} OfsEditIntentKind;

// One edit intent — a flat tagged record mirroring the host EditIntent (src/Core/IntentEvents.h)
// field-for-field, so marshaling across the seam is a plain copy. Only the fields named for `kind` are
// meaningful; the rest stay zero. `snapshot` is deliberately NOT here — gesture phase rides the
// host-side EditRequestEvent envelope and the host stamps the snapshot flag, so a plugin can't break
// undo coalescing.
typedef struct {
    int kind;        // OfsEditIntentKind
    int axis;        // StandardAxis
    double time;     // AddPoint/RemovePoint: target; MovePoint: toAt; Paste: pasteTime
    double fromTime; // MovePoint: fromAt
    int pos;         // AddPoint/AddPointAtPlayhead: pos; MovePoint: toPos; MoveSelection: position delta
    int direction;   // MoveSelection time nudge: +1 forward / -1 backward; 0 ⇒ a position nudge (pos = delta)
    int reps;        // MoveSelection time nudge: held-repeat burst count. Contract: always ≥ 1 — the host
                     // clamps a 0/negative to 1 on the way across the seam (both directions), so reps is
                     // never an "off" switch.
    int exact;       // Paste: 1 = paste at the clipboard's original times
    int seekAfter;   // MoveSelection time nudge: 1 = seek to the moved selection afterward
} OfsEditIntent;

// Step channel — which step the request targets. MUST match StepGranularity in IntentEvents.h.
//   Frame         : the overlay grid (frame interval / tempo beat)   — ← / →
//   Action        : the adjacent action on the active axis           — ↓ / ↑
//   ActionAllAxes : the nearest adjacent action across all axes      — Ctrl+↓ / Ctrl+↑
typedef enum { OfsNavGranFrame = 0, OfsNavGranAction = 1, OfsNavGranActionAllAxes = 2 } OfsNavGranularity;

// Dual-purpose: the `step` argument carries the request (direction/reps/granularity), the `out`
// argument carries the result (time + optional axis activation). Direction across the seam is fixed by
// which argument it is — onStep always receives a Step and writes a Seek — so no tag field is needed.
typedef struct {
    int direction;   // Step: -1 prev / +1 next
    int reps;        // Step: held-repeat burst count. Contract: always ≥ 1 (the host clamps 0/negative to 1).
    double time;     // Seek: resolved absolute target time
    int granularity; // Step: OfsNavGranularity — which channel the user asked to step
    int axis;        // Seek: StandardAxis to activate alongside the seek. The host pre-seeds it with the
                     // currently-active axis, so a navigator that only seeks leaves the active axis as-is
                     // without touching this field; overwrite it to *switch* axes, reproducing the native
                     // ActionAllAxes step (land on a multi-axis action *and* make its axis active) that a
                     // time-only Seek can't express. Unused on the Step (`in`) side.
} OfsNavIntent;

// Edit-mode onIntent disposition (the callback's return value). ReplacePerAxis is Replace's per-axis
// sibling: instead of projecting the lead's emitted intents across the active group (the mechanical
// offset Replace/Pass use below the seam), the host applies the lead's intents to the lead alone, then
// re-consults onEditIntent once per remaining editable axis (request retargeted) and applies each
// verbatim — so each axis's edit is computed from that axis's own data.
typedef enum { OfsEditPass = 0, OfsEditDrop = 1, OfsEditReplace = 2, OfsEditReplacePerAxis = 3 } OfsEditDisposition;

// Host-owned sink an edit mode emits Replace intents into; the host copies *out during the call, so
// nothing the plugin allocates crosses the boundary. Call once per replacement (0 calls ≡ Drop).
typedef void (*OfsEmitEdit)(void *sink, const OfsEditIntent *out);

// Active-edit-mode resolver. `ud` is the def's userData, passed back verbatim — runs on the main thread.
// Returns an OfsEditDisposition: Pass (emit nothing, host resolves `in` natively), Drop (emit nothing,
// nothing applied), Replace / ReplacePerAxis (call emit per replacement; see the enum).
typedef int (*OfsEditIntentFn)(void *ud, const OfsEditIntent *in, OfsEmitEdit emit, void *sink);

// Activation lifecycle for an interaction mode (edit mode, navigator, or selection mode). onEnter/onExit
// fire when the user activates/deactivates the mode from the footer — never on the plugin's initiative —
// so a mode that needs to precompute or tear down per-activation state has a hook for it. Either may be
// null (a stateless resolver needs neither). `ud` is the def's userData; runs on the main thread.
typedef void (*OfsIntentLifecycleFn)(void *ud);

// Per-intent options UI — the widgets that configure an interaction intent (a brush size, a smoothing
// strength, a selection threshold). `ud` is the def's userData, passed back verbatim. Main thread,
// frame-budget bound. The host calls it ONLY while this intent is the active selection, in the docked
// "Tool Options" panel; it is never called for an inactive or fallen-back mode, so the plugin no longer
// hand-gates its options on isEditModeActive &c. Draw with the same host ui* builder as onBuildUI — no
// pixel sizes (conventions below). A null onUi means the intent has no options (no panel section for it).
typedef void (*OfsIntentUiFn)(void *ud);

// Active-navigator resolver disposition (the onStep return value). Seek writes *out (kind=Seek,
// time=target); None swallows the step; Pass defers this step's granularity to the native resolution
// (so a navigator can redefine one channel — e.g. Action — and leave Frame stepping native).
typedef enum { OfsNavResultNone = 0, OfsNavResultSeek = 1, OfsNavResultPass = 2 } OfsNavResult;

// Active-navigator resolver (main thread). Returns an OfsNavResult; on Seek it writes *out (kind=Seek,
// time=target). `step->granularity` says which channel the user stepped; return Pass for the channels
// this navigator does not redefine.
typedef int (*OfsNavStepFn)(void *ud, const OfsNavIntent *step, OfsNavIntent *out);

// Edit-mode registration — main thread only, call from onLoad. `id` is a local id; the host prepends
// the plugin name → "<plugin>.<id>". Publishing only — the user activates it from the footer.
typedef struct {
    const char *id;
    const char *displayName;
    OfsEditIntentFn onEditIntent; // null → behaves as native (always Pass)
    OfsIntentLifecycleFn onEnter; // may be null (see OfsIntentLifecycleFn)
    OfsIntentLifecycleFn onExit;  // may be null
    OfsIntentUiFn onUi;           // may be null → no options affordance (see OfsIntentUiFn)
    void *userData;               // opaque; passed back as the first arg of the callbacks above
} OfsEditModeDef;

// Navigator registration — main thread only, call from onLoad. Shape mirrors OfsEditModeDef.
typedef struct {
    const char *id;
    const char *displayName;
    OfsNavStepFn onStep;          // null → behaves as native follow-overlay
    OfsIntentLifecycleFn onEnter; // may be null (see OfsIntentLifecycleFn)
    OfsIntentLifecycleFn onExit;  // may be null
    OfsIntentUiFn onUi;           // may be null → no options affordance (see OfsIntentUiFn)
    void *userData;
} OfsNavigatorDef;

// Selection gesture — which selection-authoring gesture a request carries. MUST match SelectGesture in
// IntentEvents.h (and SelectGesture in Ofs.Api/Selection.cs).
//   Box   : marquee / time-range — every action with time in [startTime,endTime] (native ignores pos)
//   All   : select-all (Ctrl+A) — every action on the axis
//   Point : click / Ctrl-click   — the action at {startTime,pos} (startTime == endTime)
typedef enum { OfsSelectBox = 0, OfsSelectAll = 1, OfsSelectPoint = 2 } OfsSelectGesture;

// A selection request handed to an active selection mode, once per editable axis (the host fans out
// per-axis below the seam). The mode reads the region + axis, enumerates candidates itself through the
// host read API, and emits the action times it keeps. The replace-vs-toggle combine (Ctrl held) is
// owned by the host below the seam and is deliberately NOT exposed here — a mode only decides *which*
// actions a gesture means, never how they merge into the current selection.
typedef struct {
    int gesture;      // OfsSelectGesture
    int axis;         // StandardAxis being resolved (lead, then each group follower)
    double startTime; // Box: range start; Point: the time
    double endTime;   // Box: range end (== startTime for Point)
    int pos;          // Point: the clicked pos
} OfsSelectRequest;

// Selection-mode onSelect disposition (the callback's return value), parallel to OfsEditDisposition.
typedef enum { OfsSelectPass = 0, OfsSelectDrop = 1, OfsSelectReplace = 2 } OfsSelectDisposition;

// Host-owned sink the mode emits kept actions into; the host copies each {at,pos} during the call and
// resolves it back to the real action on the axis. An action that names no point on the axis is
// silently ignored (a mode can only select existing actions, never invent them).
typedef void (*OfsEmitSelect)(void *sink, PluginAction action);

// Active-selection-mode resolver (main thread). Returns an OfsSelectDisposition:
//   Pass    → return OfsSelectPass,    emit nothing (host selects all native candidates for the gesture)
//   Drop    → return OfsSelectDrop,    emit nothing (host selects nothing)
//   Replace → return OfsSelectReplace, call emit(sink, t) per kept action time (0 emits ≡ Drop)
typedef int (*OfsSelectFn)(void *ud, const OfsSelectRequest *in, OfsEmitSelect emit, void *sink);

// Selection-mode registration — main thread only, call from onLoad. Shape mirrors OfsNavigatorDef.
typedef struct {
    const char *id;
    const char *displayName;
    OfsSelectFn onSelect;         // null → native (always Pass)
    OfsIntentLifecycleFn onEnter; // may be null (see OfsIntentLifecycleFn)
    OfsIntentLifecycleFn onExit;  // may be null
    OfsIntentUiFn onUi;           // may be null → no options affordance (see OfsIntentUiFn)
    void *userData;
} OfsSelectModeDef;

// ── Marshaled-layout assertions ──────────────────────────────────────────────
// Every struct that crosses the native↔managed seam by raw memory copy is mirrored field-for-field in
// Ofs.Api/PluginAbi.cs. The two sides compile independently, so nothing checks them against each other
// at build time; these pin the C++ side's size and field offsets to fixed values, so reordering or
// resizing a marshaled field fails the native build and forces the matching edit on the C# mirror.
// A layout change here is NOT an ABI version bump (see OFS_ABI_VERSION) — it is a mismatch to fix, not
// a new contract. The whole plugin ABI marshals raw pointers, so it is 64-bit only:
static_assert(sizeof(void *) == 8, "plugin ABI assumes a 64-bit (8-byte pointer) build");

static_assert(sizeof(PluginAction) == 16, "PluginAction layout drift (mirror: ScriptAction)");
static_assert(offsetof(PluginAction, at) == 0 && offsetof(PluginAction, pos) == 8, "PluginAction field drift");

static_assert(sizeof(OfsEvalCtx) == 40, "OfsEvalCtx layout drift");
static_assert(offsetof(OfsEvalCtx, regionStart) == 0 && offsetof(OfsEvalCtx, regionEnd) == 8 &&
                  offsetof(OfsEvalCtx, params) == 16 && offsetof(OfsEvalCtx, paramCount) == 24 &&
                  offsetof(OfsEvalCtx, stateHandle) == 28 && offsetof(OfsEvalCtx, cancelFlag) == 32,
              "OfsEvalCtx field drift");

static_assert(sizeof(OfsCommandDef) == 32, "OfsCommandDef layout drift");
static_assert(offsetof(OfsCommandDef, id) == 0 && offsetof(OfsCommandDef, group) == 8 &&
                  offsetof(OfsCommandDef, title) == 16 && offsetof(OfsCommandDef, inRebindList) == 24 &&
                  offsetof(OfsCommandDef, inPalette) == 28,
              "OfsCommandDef field drift");

static_assert(sizeof(OfsNodeDef) == 96, "OfsNodeDef layout drift");
static_assert(offsetof(OfsNodeDef, id) == 0 && offsetof(OfsNodeDef, displayName) == 8 &&
                  offsetof(OfsNodeDef, inputNames) == 16 && offsetof(OfsNodeDef, outputNames) == 24 &&
                  offsetof(OfsNodeDef, fn) == 32 && offsetof(OfsNodeDef, userData) == 40 &&
                  offsetof(OfsNodeDef, onNodeUi) == 48 && offsetof(OfsNodeDef, signal) == 56 &&
                  offsetof(OfsNodeDef, inputCount) == 60 && offsetof(OfsNodeDef, outputCount) == 64 &&
                  offsetof(OfsNodeDef, hasState) == 68 && offsetof(OfsNodeDef, group) == 72 &&
                  offsetof(OfsNodeDef, icon) == 80 && offsetof(OfsNodeDef, description) == 88,
              "OfsNodeDef field drift");

static_assert(sizeof(OfsEditIntent) == 48, "OfsEditIntent layout drift");
static_assert(offsetof(OfsEditIntent, kind) == 0 && offsetof(OfsEditIntent, axis) == 4 &&
                  offsetof(OfsEditIntent, time) == 8 && offsetof(OfsEditIntent, fromTime) == 16 &&
                  offsetof(OfsEditIntent, pos) == 24 && offsetof(OfsEditIntent, direction) == 28 &&
                  offsetof(OfsEditIntent, reps) == 32 && offsetof(OfsEditIntent, exact) == 36 &&
                  offsetof(OfsEditIntent, seekAfter) == 40,
              "OfsEditIntent field drift");

static_assert(sizeof(OfsNavIntent) == 24, "OfsNavIntent layout drift");
static_assert(offsetof(OfsNavIntent, direction) == 0 && offsetof(OfsNavIntent, reps) == 4 &&
                  offsetof(OfsNavIntent, time) == 8 && offsetof(OfsNavIntent, granularity) == 16 &&
                  offsetof(OfsNavIntent, axis) == 20,
              "OfsNavIntent field drift");

static_assert(sizeof(OfsEditModeDef) == 56, "OfsEditModeDef layout drift");
static_assert(offsetof(OfsEditModeDef, id) == 0 && offsetof(OfsEditModeDef, displayName) == 8 &&
                  offsetof(OfsEditModeDef, onEditIntent) == 16 && offsetof(OfsEditModeDef, onEnter) == 24 &&
                  offsetof(OfsEditModeDef, onExit) == 32 && offsetof(OfsEditModeDef, onUi) == 40 &&
                  offsetof(OfsEditModeDef, userData) == 48,
              "OfsEditModeDef field drift");

static_assert(sizeof(OfsNavigatorDef) == 56, "OfsNavigatorDef layout drift");
static_assert(offsetof(OfsNavigatorDef, id) == 0 && offsetof(OfsNavigatorDef, displayName) == 8 &&
                  offsetof(OfsNavigatorDef, onStep) == 16 && offsetof(OfsNavigatorDef, onEnter) == 24 &&
                  offsetof(OfsNavigatorDef, onExit) == 32 && offsetof(OfsNavigatorDef, onUi) == 40 &&
                  offsetof(OfsNavigatorDef, userData) == 48,
              "OfsNavigatorDef field drift");

static_assert(sizeof(OfsSelectRequest) == 32, "OfsSelectRequest layout drift");
static_assert(offsetof(OfsSelectRequest, gesture) == 0 && offsetof(OfsSelectRequest, axis) == 4 &&
                  offsetof(OfsSelectRequest, startTime) == 8 && offsetof(OfsSelectRequest, endTime) == 16 &&
                  offsetof(OfsSelectRequest, pos) == 24,
              "OfsSelectRequest field drift");

static_assert(sizeof(OfsSelectModeDef) == 56, "OfsSelectModeDef layout drift");
static_assert(offsetof(OfsSelectModeDef, id) == 0 && offsetof(OfsSelectModeDef, displayName) == 8 &&
                  offsetof(OfsSelectModeDef, onSelect) == 16 && offsetof(OfsSelectModeDef, onEnter) == 24 &&
                  offsetof(OfsSelectModeDef, onExit) == 32 && offsetof(OfsSelectModeDef, onUi) == 40 &&
                  offsetof(OfsSelectModeDef, userData) == 48,
              "OfsSelectModeDef field drift");

// ─────────────────────────────────────────────────────────────────────────────
// Conventions for plugin authors (these hold for the whole HostApi; documenting them changes no
// layout, so it is NOT an ABI change — OFS_ABI_VERSION stays put):
//
//  • Strings are UTF-8. Every `const char *` in and `char *buf` out is UTF-8, NUL-terminated.
//
//  • `char *buf, int bufSize` counts BYTES, not characters. UTF-8 is multibyte — a CJK glyph is
//    3 bytes, an emoji 4 — so a 32-byte buffer holds only ~10 CJK characters. The host always writes a
//    NUL within bufSize and never splits a codepoint: an over-long value is truncated on a valid char
//    boundary (it does not corrupt). This applies to edit buffers (uiInputText / uiInputTextMultiline)
//    and to every read-out buffer. The C# Ofs.Api wrapper surfaces edit-buffer caps as `maxBytes`.
//
//  • Every read-out getter REPORTS THE REQUIRED LENGTH so the caller can grow and never silently clip.
//    The single-string getters (getMediaPath, getAxisName, getProjectMetadata)
//    return the full source byte length excluding the NUL; the composite getters
//    (getChapter/getBookmark/getRegion) report their name's required length through the `nameReqOut`
//    out-param. In every case `required >= bufSize` means the value was truncated — re-call with a
//    buffer of `required + 1` bytes and it will fit. The buffer is always NUL-terminated even on an
//    error/empty return, so the caller may read it unconditionally. (The C# Ofs.Api `GrowAndRead`
//    helper does exactly this — one deterministic retry.)
//
//  • The UI takes NO pixel sizes. Widget width is host-owned: a widget fills the available width, and
//    widgets inside a uiPushRow split it evenly. There is intentionally no width parameter to pass, so
//    plugin UI is automatically font-, DPI- and translation-safe — use uiPushSection / uiPushRow for
//    layout, never try to reach for a pixel width. (If a width parameter is ever added, the convention
//    is -1 = "fill available", mirroring the host's own `-FLT_MIN` usage.)
//
//  • Plugin-supplied display strings (widget labels, section/row titles, command titles, node display
//    names, the plugin's getName() window title) are rendered VERBATIM. They do NOT pass through the
//    host's string catalog (localization/strings.toml) — localizing them is the plugin's own job.
//    A plugin reads getActiveLanguage at onLoad and registers its strings in that language; on a later
//    UI-language switch the host unloads and reloads every plugin, so each re-registers from its onLoad
//    in the new language. There is no live re-supply path. (The Ofs.Api C# wrapper handles this for you.)
// ─────────────────────────────────────────────────────────────────────────────

struct HostApi {
    int version; // must equal OFS_ABI_VERSION
    void *ctx;   // host-supplied context; pass as first argument to every function pointer below
    // Player queries
    double (*getTime)(void *ctx);
    double (*getDuration)(void *ctx);
    int (*isPlaying)(void *ctx);
    float (*getSpeed)(void *ctx);
    int (*getMediaPath)(void *ctx, char *buf, int bufSize); // returns required byte length (excl NUL)
    // Volume is 0..1; getters return 0 when no media is loaded
    float (*getVolume)(void *ctx);
    void (*setVolume)(void *ctx, float volume);
    double (*getFps)(void *ctx);
    int (*getVideoWidth)(void *ctx);
    int (*getVideoHeight)(void *ctx);
    // Seconds one action move-step travels under the active overlay (frame interval or tempo beat) — the
    // unit a time nudge advances by. The host already applies it when delivering a single-action nudge as a
    // MovePoint; a mode handling a multi-action MoveSelection multiplies it by direction × reps itself.
    double (*getMoveStepTime)(void *ctx);
    // Axis enumeration — returns total count; copies present roles (as int) into rolesBuf when non-null
    int (*getAxisRoles)(void *ctx, int *rolesBuf, int bufSize);
    // Role of the active axis as int. On the main thread always a valid role: there is always an active
    // axis (defaults to L0, which can't be removed or hidden), so it never reports "none". -1 only from the
    // off-main-thread guard (misuse; the managed wrapper asserts the main thread before calling).
    int (*getActiveAxisRole)(void *ctx);
    // Axis read — returns total count; copies min(count,bufSize) into buf; buf may be nullptr
    int (*getAxisActions)(void *ctx, int role, PluginAction *buf, int bufSize);
    // Returns action count without copying data
    int (*getAxisActionCount)(void *ctx, int role);
    // Axis write — replaces all actions, triggers undo snapshot + AxisModifiedEvent
    void (*commitAxisActions)(void *ctx, int role, const PluginAction *actions, int count);
    // Selection read
    int (*getAxisSelection)(void *ctx, int role, PluginAction *buf, int bufSize);
    int (*getAxisSelectionCount)(void *ctx, int role);
    // Selection write — selection is ephemeral, no undo
    void (*setAxisSelection)(void *ctx, int role, const PluginAction *actions, int count);
    void (*clearAxisSelection)(void *ctx, int role);
    // Player commands
    void (*seekTo)(void *ctx, double time);
    void (*setPlaying)(void *ctx, int playing);
    void (*setSpeed)(void *ctx, float speed);
    // Register a command in the palette. Main thread only, call from onLoad.
    // Returns an OfsRegisterCommandResult: 0 on success, else a distinct nonzero failure code.
    int (*registerCommand)(void *ctx, const OfsCommandDef *def);
    // Immediate-mode UI — call these from onBuildUI; host manages all ImGui state and IDs
    void (*uiLabel)(void *ctx, const char *text);
    int (*uiButton)(void *ctx, const char *label);               // 1 if clicked
    int (*uiCheckbox)(void *ctx, const char *label, int *value); // 1 if changed
    int (*uiSliderFloat)(void *ctx, const char *label, float *value, float min, float max,
                         int decimals);                                                          // 1 if changed
    int (*uiSliderInt)(void *ctx, const char *label, int *value, int min, int max);              // 1 if changed
    int (*uiCombo)(void *ctx, const char *label, int *index, const char *itemsSeparatedByZeros); // 1 if changed
    void (*uiSeparator)(void *ctx);
    void (*uiPushSection)(void *ctx, const char *title);
    void (*uiPopSection)(void *ctx);
    int (*uiRadioButton)(void *ctx, const char *label, int *value, int option); // 1 if clicked
    int (*uiInputInt)(void *ctx, const char *label, int *value, int step);      // 1 if changed
    // `decimals` (0..9) is the displayed fractional-digit count; the host turns it into a bounded
    // "%.Nf" — plugins never supply a raw printf format string (format-string safety).
    int (*uiInputFloat)(void *ctx, const char *label, float *value, float step, int decimals); // 1 if changed
    int (*uiDragInt)(void *ctx, const char *label, int *value, float speed, int min, int max); // 1 if changed
    int (*uiDragFloat)(void *ctx, const char *label, float *value, float speed, float min, float max,
                       int decimals); // 1 if changed
    // buf is a UTF-8 NUL-terminated edit buffer of bufSize *bytes* (see the buffer convention above).
    // `password` != 0 masks the text; `readOnly` != 0 disables editing.
    int (*uiInputText)(void *ctx, const char *label, char *buf, int bufSize, int password,
                       int readOnly); // 1 if changed
    // Scoped horizontal row: widgets between push/pop share one line; width-bearing widgets split the
    // available width evenly. Balanced + depth-guarded like uiPushSection/uiPopSection.
    void (*uiPushRow)(void *ctx, const char *label);
    void (*uiPopRow)(void *ctx);
    // Plugin node registration — main thread only, call from onLoad
    void (*registerNode)(void *ctx, const OfsNodeDef *def);
    // Discrete I/O accessors — worker-thread-safe; no ctx needed
    int (*nodeInputCount)(const OfsDiscreteInput *in);
    double (*nodeInputTime)(const OfsDiscreteInput *in, int i);
    int (*nodeInputPosition)(const OfsDiscreteInput *in, int i);
    void (*nodeAddAction)(OfsDiscreteOutput *out, double time, int position);
    // Host diagnostics — any thread. level matches LogLevel: 0=trace 1=debug 2=info 3=warning 4=error.
    void (*hostLog)(void *ctx, int level, const char *msg);
    // Throttled user-facing error toast for a swallowed plugin fault — any thread. `faultCtx` is the
    // entry point that threw (e.g. "OnUpdate", "node:gen"); the host names the plugin and coalesces.
    void (*hostReportFault)(void *ctx, const char *faultCtx);
    // Plugin-initiated user-facing toast — any thread. `level` matches NotifyLevel: 0=info 1=success
    // 2=warning 3=error. The host prefixes the plugin name and shows `msg` verbatim. Not throttled, so
    // the plugin must rate-limit itself (do not call every frame).
    void (*hostNotify)(void *ctx, int level, const char *msg);

    // ── Axis & project state (read + a few writes) ────────────────────────────
    // Make `role` (StandardAxis as int) the active axis. No-op if that axis isn't present. Main thread.
    void (*setActiveAxis)(void *ctx, int role);
    // Axis flags — return -1 if the axis isn't present, else 0/1.
    int (*isAxisVisible)(void *ctx, int role);
    int (*isAxisLocked)(void *ctx, int role);
    // Display name (e.g. "L0 (Stroke)") into buf as UTF-8, NUL-terminated. Returns required length.
    int (*getAxisName)(void *ctx, int role, char *buf, int bufSize);
    // 1 if the project has unsaved changes.
    int (*isProjectDirty)(void *ctx);
    // Full project metadata as a UTF-8 JSON object into buf (same schema as the .funscript metadata
    // block: title, creator, scriptUrl, videoUrl, description, notes, tags, performers, license, plus
    // any custom fields written inline). Empty string when no project is loaded. Returns required length.
    int (*getProjectMetadata)(void *ctx, char *buf, int bufSize);
    // Chapters, sorted by start time. getChapter fills the out params (any may be null) and returns 1, or
    // 0 for an out-of-range index. nameBuf receives the chapter name as UTF-8; *nameReqOut (may be null)
    // reports the name's required byte length (excl NUL) so a clipped name can be re-read into a bigger buf.
    int (*getChapterCount)(void *ctx);
    int (*getChapter)(void *ctx, int index, double *startOut, double *endOut, unsigned int *colorOut, char *nameBuf,
                      int nameBufSize, int *nameReqOut);
    // Bookmarks, sorted by time. *nameReqOut as in getChapter.
    int (*getBookmarkCount)(void *ctx);
    int (*getBookmark)(void *ctx, int index, double *timeOut, char *nameBuf, int nameBufSize, int *nameReqOut);
    // Processing regions, sorted by start time. *nameReqOut as in getChapter.
    int (*getRegionCount)(void *ctx);
    int (*getRegion)(void *ctx, int index, double *startOut, double *endOut, char *nameBuf, int nameBufSize,
                     int *nameReqOut);

    // ── Localization ──────────────────────────────────────────────────────────
    // The ISO 639 code of ofs-ng's active UI language ("en" for built-in English, else the iso639 the
    // translation file declares, e.g. "ja"). Fills buf (NUL-terminated) and returns the required byte
    // length (excl NUL). A plugin reads this at onLoad and maps it to a .NET culture to drive its own
    // resource catalog; the host reloads every plugin on a language switch, so it is re-read each time.
    int (*getActiveLanguage)(void *ctx, char *buf, int bufSize);

    // ── Native dialogs (non-blocking) ─────────────────────────────────────────
    // Main thread only. These do NOT block: they queue the dialog and return immediately; when the user
    // answers (a later frame) the host invokes `onResult(userData, …)` exactly once on the main thread.
    // All strings are UTF-8. `filterPatterns` is a ';'-separated glob list (e.g. "*.png;*.jpg"), or
    // null/"" for all files. The file/folder result path is null when the user cancelled.
    void (*openFileDialog)(void *ctx, const char *title, const char *filterPatterns, const char *filterDesc,
                           OfsFileResultFn onResult, void *userData);
    // Multi-select sibling of openFileDialog: the user may pick several files; `onResult` receives the whole
    // list (count 0 == cancelled). Same arguments otherwise.
    void (*openFilesDialog)(void *ctx, const char *title, const char *filterPatterns, const char *filterDesc,
                            OfsFilesResultFn onResult, void *userData);
    void (*saveFileDialog)(void *ctx, const char *title, const char *defaultName, const char *filterPatterns,
                           const char *filterDesc, OfsFileResultFn onResult, void *userData);
    void (*pickFolder)(void *ctx, const char *title, OfsFileResultFn onResult, void *userData);
    // Message / confirmation box. kind: 0=ok, 1=okcancel, 2=yesno.
    void (*confirmDialog)(void *ctx, const char *title, const char *message, int kind, OfsConfirmResultFn onResult,
                          void *userData);

    // ── Immediate-mode UI (additions) — call from onBuildUI ───────────────────
    // rgba points to 4 floats in 0..1; returns 1 if edited this frame. `alpha` != 0 shows the alpha
    // channel (RGBA); 0 edits RGB only and leaves rgba[3] untouched.
    int (*uiColorEdit)(void *ctx, const char *label, float *rgba, int alpha);
    // Multi-line text box. buf is a UTF-8 NUL-terminated edit buffer of bufSize bytes. heightLines <= 0
    // uses a default height. `readOnly` != 0 disables editing. Returns 1 if edited this frame.
    int (*uiInputTextMultiline)(void *ctx, const char *label, char *buf, int bufSize, int heightLines, int readOnly);
    // Progress bar; fraction is clamped to 0..1; overlay text may be null.
    void (*uiProgressBar)(void *ctx, float fraction, const char *overlay);

    // ── Plugin-node custom UI state (main thread, only valid inside an onNodeUi call) ─────────
    // The host sets the "current node" before each onNodeUi call (like currentPluginName). The
    // managed wrapper reads the node's stored TState JSON via nodeUiGetState, draws its widgets,
    // and on a detected change reports the new JSON via nodeUiSetState; the host stores it into the
    // current node's nodeState and raises ModifyRegionEvent. "" outside an onNodeUi call.
    void (*nodeUiSetState)(void *ctx, const char *stateJsonUtf8);
    const char *(*nodeUiGetState)(void *ctx);
    // Stable identity of the current onNodeUi node, packed (regionId << 32) | (uint32)nodeId; -1 outside
    // an onNodeUi call. The managed Node<TState> handle carries this so a deferred Node.Update(mutator)
    // can enqueue against the right node and apply on its next UI pass.
    long long (*nodeUiCurrentKey)(void *ctx);

    // ── Per-plugin project data (stored in the .ofp, round-trips with save/load) ───────────────
    // A plugin's own key→JSON store, namespaced by the calling plugin (currentPluginName) so it can
    // only ever see its own data. Main thread only; NOT undoable.
    // getProjectData fills buf with this plugin's JSON value for `key` (UTF-8, "" if none) and returns
    // the required byte length excl NUL (GrowAndRead contract). setProjectData replaces it; an empty or
    // null jsonUtf8 erases the key. `jsonUtf8` must be a valid UTF-8 JSON value — the host parses and
    // stores it structurally (so it round-trips like nodeState); malformed JSON is logged and ignored.
    // setProjectData marks the project dirty.
    int (*getProjectData)(void *ctx, const char *key, char *buf, int bufSize);
    void (*setProjectData)(void *ctx, const char *key, const char *jsonUtf8);

    // ── Per-plugin app data (global app settings, stored in <pref>/plugin_settings/<plugin>.json) ──
    // A plugin's own key→JSON store, namespaced by the calling plugin (currentPluginName) and persisted
    // in one file per plugin under the pref dir — unlike per-plugin PROJECT data (above), this is global
    // to all projects and survives project switches. Main thread only; NOT undoable. The host caches the
    // store in memory and writes it back debounced once per frame and again on app close.
    // getAppData fills buf with this plugin's JSON value for `key` (UTF-8, "" if none) and returns the
    // required byte length excl NUL (GrowAndRead contract). setAppData replaces it; an empty or null
    // jsonUtf8 erases the key. `jsonUtf8` must be a valid UTF-8 JSON value (parsed and stored
    // structurally, like setProjectData); malformed JSON is logged and ignored.
    int (*getAppData)(void *ctx, const char *key, char *buf, int bufSize);
    void (*setAppData)(void *ctx, const char *key, const char *jsonUtf8);

    // ── Interaction extension points (main thread, call from onLoad) ───────────
    // Publish an edit mode / navigator the user can pick in the footer. The host prepends the plugin
    // name to def->id. No activation here — there is no plugin-callable setter.
    void (*registerEditMode)(void *ctx, const OfsEditModeDef *def);
    void (*registerNavigator)(void *ctx, const OfsNavigatorDef *def);
    // 1 if the calling plugin's edit mode / navigator `localId` (host prepends the plugin name, as in
    // registerEditMode / registerNavigator) is the active one. Its original use — gating a mode's options
    // UI in onBuildUI — is superseded by the per-mode onUi callback (OfsIntentUiFn),
    // which the host draws only while the mode is active. Kept for any non-UI activeness query. Main thread.
    int (*isEditModeActive)(void *ctx, const char *localId);
    int (*isNavigatorActive)(void *ctx, const char *localId);
    // Publish a selection mode the user can pick in the footer's Select selector; host prepends the plugin
    // name to def->id. No activation here (no plugin-callable setter). isSelectionModeActive mirrors
    // isEditModeActive — 1 if the calling plugin's selection mode `localId` is the active one. Main thread.
    void (*registerSelectionMode)(void *ctx, const OfsSelectModeDef *def);
    int (*isSelectionModeActive)(void *ctx, const char *localId);

    // ── Immediate-mode UI (additions) — call from onBuildUI ───────────────────
    // Word-wrapped paragraph text (wraps to the cell width inside a section, else the window content width).
    // Unlike uiLabel (a single unwrapped line), use this for sentences/disclaimers. Main thread.
    void (*uiTextWrapped)(void *ctx, const char *text);
    // Scoped disabled block: widgets drawn between push/pop are greyed and don't take input. Nestable; the
    // host depth-guards the pop so an unbalanced plugin can't unwind ImGui's stack. `disabled` == 0 pushes a
    // no-op level (push/pop stays balanced regardless of the condition). Main thread.
    void (*uiPushDisabled)(void *ctx, int disabled);
    void (*uiPopDisabled)(void *ctx);
    // Hover tooltip for the most recently drawn widget. Call right after the widget; a no-op if none
    // precedes it. Does not fire on a disabled widget (use the disabled-tooltip scope for that). Main thread.
    void (*uiTooltip)(void *ctx, const char *text);
    // Like uiPushDisabled, but while `disabled`, hovering any greyed widget drawn inside the scope shows
    // `tooltip` (explaining why it's disabled). Nestable and depth-guarded like uiPushDisabled; the host
    // copies `tooltip`, so the caller need not keep it alive past the call. Main thread.
    void (*uiPushDisabledTooltip)(void *ctx, int disabled, const char *tooltip);
    void (*uiPopDisabledTooltip)(void *ctx);

    // ── Funscript export (read) ───────────────────────────────────────────────
    // Build a funscript document for one or more axes and write it as UTF-8 JSON into buf. `roles` points
    // to `count` StandardAxis values (as int); `version` is an OfsFunscriptVersion selecting the format:
    // 1.0 serializes a SINGLE axis (the first valid role; extra roles ignored) as {actions, metadata};
    // 1.1 carries the primary axis's actions at top level and the rest under "axes":[{id,actions},…];
    // 2.0 is the same shape but under a "channels" object. Absent, empty, scratch (S0–S9, which have no
    // funscript tag), and duplicate roles are skipped; an empty result (no exportable axis) writes "" and
    // returns 0. The document carries the project metadata block (with its version field set to match) and
    // action times are milliseconds, matching the host's own export. Returns the required byte length
    // (excl NUL); GrowAndRead contract. Main thread.
    int (*getFunscriptJson)(void *ctx, const int *roles, int count, int version, char *buf, int bufSize);
};

struct PluginApi {
    int version; // must equal OFS_ABI_VERSION
    void (*onLoad)();
    // Host calls this each frame; plugin calls back via HostApi ui* functions to draw widgets
    void (*onBuildUI)();
    // Optional event callbacks — nullptr means not implemented
    void (*onUpdate)(float delta);
    void (*onTimeChange)(double time);
    void (*onPlayChange)(int playing);
    void (*onSpeedChange)(float speed);
    void (*onMediaChange)(const char *path);
    void (*onProjectChange)();
    // role is a StandardAxis value cast to int
    void (*onAxisModified)(int role);
    // Fired when the active axis changes; role is -1 if no axis is active
    void (*onActiveAxisChanged)(int role);
    // Fired when a command registered via registerCommand is invoked (palette, key, or gamepad)
    void (*onCommand)(const char *id);
    // Called before the plugin is unloaded so it can release resources
    void (*onUnload)();
    // Returns the plugin's display name. The pointer is valid until the next getName() call — the host
    // copies it immediately. A name derived from the plugin's own catalog (e.g. a Str.* getter) follows
    // the active UI language because the host reloads the plugin on a language switch and re-pulls this.
    const char *(*getName)();
    // Returns the plugin's version (assembly informational version); pointer must remain valid for
    // the plugin's lifetime. nullptr/empty if the plugin doesn't report one.
    const char *(*getVersion)();
};

// The two function-pointer tables are mirrored field-for-field in Ofs.Api/PluginAbi.cs the same way the
// data structs above are, but a table is the layout most likely to drift — every new HostApi function
// appends a pointer, and a managed mirror that adds it in the wrong place (or not at all) reads every
// later pointer at the wrong offset. These pin the native size so a table edit that forgets to update
// the count fails the native build; the C# AbiLayout.Verify() self-test pins the managed side.
static_assert(sizeof(HostApi) == 736, "HostApi layout drift (mirror: Ofs.Api/PluginAbi.cs HostApi)");
static_assert(sizeof(PluginApi) == 120, "PluginApi layout drift (mirror: Ofs.Api/PluginAbi.cs PluginApi)");

} // namespace ofs
