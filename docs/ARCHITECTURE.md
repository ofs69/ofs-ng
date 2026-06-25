# Architecture

This is the **prose companion** to the rules in [`../CLAUDE.md`](../CLAUDE.md). CLAUDE.md is the
normative contract — the MUST/MUST NOT list any change has to respect, and the field-by-field source of
truth (which in turn defers to `src/Core/ScriptProject.h`). This document explains *why* the design is
shaped that way and walks the data flows that the rules exist to protect. It deliberately holds **no code
listings** — the headers are the source of truth and rot if duplicated; where a detail matters, this file
names the type or file to read. Where the two documents overlap, CLAUDE.md wins; if you find them
disagreeing, CLAUDE.md is right and this file has drifted.

## Overview

The application is structured around three primitives — no ECS framework, no entity IDs, no component
pools:

| Primitive | Role |
|-----------|------|
| **ScriptProject** | Single source of truth for all project state |
| **EventQueue** | Only channel for cross-system communication |
| **Services** | Behavior units that read ScriptProject and push events |

`OfsApp` owns all three and composes them. No framework governs the structure.

## ScriptProject

`ScriptProject` (`src/Core/ScriptProject.h`) owns all project state as a plain C++ struct hierarchy:
`AxisState axes[kStandardAxisCount]` indexed by the `StandardAxis` enum, plus the top-level state structs
(`state`, `overlay`, `simulator`, …), the always-sorted `regions`, the opaque `pluginData` store, and the
`active*`/`stored*` interaction-selection ids. The header is the source of truth for the full field list;
CLAUDE.md → *ScriptProject* carries the mutation rules. Each `AxisState` holds its `role`, the visibility
flags (`isVisible`, `showInStrip`, `isLocked`), a `dirty` flag, the `actions` and `selection` sorted sets,
and two transient processing fields (`resolved`, `pendingEval`) managed by `ProcessingSystem`. Two
invariants are worth seeing in prose:

**Existence is derived, not flagged.** All 20 axes always occupy the array; there is no `present` flag.
"Is this axis part of the project" is the `exists()` predicate — `showInStrip || !actions.empty() ||
isLocked` — so an axis is real once it is surfaced in the strip, carries data, or is locked (exactly the
set that survives a save). `showInStrip` is the narrower UI concern of whether a strip row is drawn (and
so whether the axis can be activated or grouped). The two diverge only for a standard axis hidden with
data: it still `exists()` and still round-trips, it just draws no row until re-shown. Persistence and
plugin enumeration key off `exists()`; strip render, activation, and grouping key off `showInStrip`.

**`mutate()` is the only door to axis fields, and it doesn't snapshot.** `ScriptProject::mutate(role, fn, eq)`
applies `fn`, syncs the selection, sets `dirty`, and pushes `AxisModifiedEvent` — but it deliberately does
*not* touch undo. `UndoSystem` auto-snapshots by registering its handlers for every undoable event type
*before* `ProjectManager` registers its mutating ones; because `EventQueue` fires handlers in registration
order, the snapshot always lands before the mutation. The one intentional exception is
`UndoSystem::restoreSnapshot()`, which calls `project.restoreAxis()` directly: undo/redo *is* the mutation
authority, and routing it back through `mutate()` would emit spurious `AxisModifiedEvent`s mid-restore. The
six interaction-selection ids are a *tool* selection, not a content edit, so they are excluded from the undo
snapshot.

## EventQueue

A typed, deferred, thread-safe bus (`src/Core/EventQueue.h`) — the only channel between systems, backed by
`moodycamel::ConcurrentQueue`. Handlers are registered with `on<E>()` at startup only; `push<E>()` is
thread-safe and callable from workers or the main thread; `drain()` runs on the main thread and invokes
handlers in push order.

The contract that makes this safe — register all handlers before any worker exists, `freeze()` before
`jobSystem.start()`, `drain()` exactly once per frame as the first thing in `onUpdate()`, push (never call)
across system boundaries — lives in CLAUDE.md → *EventQueue*. The reason it works without locks: handlers
are registered once at startup, so the handler table is effectively read-only during operation, and `push()`
from a worker only enqueues — the handler still runs on the main thread at the next `drain()`.

## Services

Services own behavior; they do not own project state. The full table and the no-direct-calls rule are in
CLAUDE.md → *Services*. The piece that needs prose is the **request/apply split** for interaction, because
it is easy to mistake for a rule violation:

`EditIntentRouter`, `NavigatorRouter`, and `SelectIntentRouter` are each the *sole* subscriber to one
request event (`EditRequestEvent`, `StepRequestEvent`, `SelectRequestEvent`, in `src/Core/IntentEvents.h`).
An *intent* is a request with a single privileged consumer — the router — kept distinct from the
*mutation/seek* events everyone else still broadcast-consumes. A plugin edit mode, navigator, or selection
mode sits on the request; the router resolves it (through the active mode) into ordinary mutation/seek
events, and `UndoSystem` / `ProjectManager` / `ProcessingSystem` stay on those application events, unchanged.
That is why a plugin can rewrite what a drag *means* without any of the downstream services knowing a plugin
exists. The extension points that ride this seam — and how they keep a plugin from ever touching the
document directly — are in *Interaction extension points* below.

## Interaction extension points

The request/apply split exists to carry three **interaction extension points** — *edit modes*, *navigators*,
and *selection modes* — through which a plugin overrides what timeline interaction *does*. They are
independent (an edit mode and a navigator are separate settings that don't influence each other) and compose
(a plugin wanting custom editing and custom stepping registers both). The request events and routers live in
`src/Core/IntentEvents.h` and the `*IntentRouter` services.

**Why an intent seam and not a command hook.** Add/edit-action is overwhelmingly mouse-driven — a shift-click
pushes `AddActionAtTimeEvent`, a drag pushes `MoveActionEvent`, straight into the queue, never through
`CommandRegistry`. A hook at command dispatch is structurally blind to that primary path, carries no payload
to *transform* an edit (it can only swallow or pass), and arbitrates by load order — the wrong shape for
mutually-exclusive, user-selected modes. So each extension point instead owns a *request* event with a single
privileged subscriber, its router.

**Register, don't activate.** A plugin only *registers* a mode (id, display name, callbacks); the **user**
activates it from a footer selector. There is **no plugin-callable setter** for any active selection — the
one rule that stops two plugins fighting over the mode. Each may supply optional `onEnter`/`onExit` (fired on
a user switch) and an `onUi` callback the host draws only while that mode is active, in the docked Tool
Options panel — so a mode never hand-gates its options.

**Dispositions, and why a plugin can't corrupt the document.** A mode answers each request with a bounded
disposition — an edit mode returns Pass (resolve natively), Drop (nothing), Replace (apply these intents
instead), or ReplacePerAxis; a navigator returns Seek/None/Pass; a selection mode returns Pass/Drop/Replace.
A replacement is *emitted into a host-owned sink* (the plugin never allocates an array the host must free) and
flows back through the **normal** mutation/seek path — so undo coalescing, the dirty flag, selection sync, and
group fan-out keep working and the plugin never touches `ScriptProject`. An edit mode emits only point intents
(never a wholesale commit); a selection mode emits `{at,pos}` pairs the host resolves to existing actions by
`at`, so it can only *select* points, never invent or mutate them. A `Replace` is never re-fed to the mode, so
it cannot loop.

**Group fan-out is host-owned, below the mode.** An edit intent names one *lead axis*; the host walks the
active edit group. `Replace` (the default) consults the mode once for the lead and derives each follower
mechanically as a position-delta offset — right for drag/nudge edits. `ReplacePerAxis` instead re-consults the
mode once per editable axis with the request retargeted, applying each result verbatim — right when the
gesture is computed from each axis's own data (per-axis grids); it is one level deep (a `ReplacePerAxis`
returned during a follower call degrades to a plain `Replace`). Selection modes are *always* per-axis — a
selection has no meaningful lead-projection — so they offer no such choice. The single↔multi-action split also
lives below the seam: a single-action keyboard nudge is presented to a mode as a `MovePoint`, so a mode
implementing only `MovePoint` governs mouse drags and single keyboard nudges alike; only a genuine
multi-action nudge arrives as `MoveSelection`. And the per-gesture snapshot latch that coalesces a whole drag
into one undo step is host-stamped on the emitted mutations (see *Axis mutation* above) — never the mode's
concern.

**Lifecycle — the active selection is weak.** The three active ids (`activeEditMode`/`activeNavigator`/
`activeSelectionMode`, defaulting to `native`/`follow-overlay`/`native`) are project state, serialized with
the project and written directly by the routers (they are non-axis fields). As a *tool* selection rather than
a content edit they are excluded from the undo snapshot. They are held *weakly by id*: if the owning plugin is
absent (uninstalled, disabled, unloaded, crashed) the active selection falls back to native while the stored
id is preserved, so re-saving keeps it; a reload republishes the registration but does not silently
re-activate it. The native `native` mode and `follow-overlay` navigator are always present, so the fallback
target can never itself dangle.

## Threading

The allowed-operations split (main vs. worker) and the no-untracked-threads rule are in CLAUDE.md →
*Threading model*. The short version: workers only read an `AxisSnapshot` (a value copy) and `push()` a
result; they never touch `ScriptProject` or call a service. Two consequences of that model deserve prose
here, because they are the load-bearing parts of plugin safety:

**Plugin callbacks run on the main thread — except node evaluation.** Lifecycle, events, commands, and UI
all run on the main thread within the frame budget. The exception is node evaluation, whose callbacks run on
`JobSystem` worker threads; those receive only their inputs/params/state copy and must not touch `Host`.
Plugins are contractually required to return within the frame budget — there is no enforcement, so a
violation appears as dropped frames.

**Fault isolation.** Every native→managed entry point is wrapped on the managed side
(`Ofs.Api/PluginGuard.cs`): a plugin exception is caught, logged via the `hostLog` callback, and raised as a
throttled error toast via `hostReportFault` (coalesced per plugin in `PluginManager::notifyPluginFault`). It
returns a safe default and never propagates back into native — a plugin bug is a logged error, not a host
crash. This covers the `PluginApi` reverse-P/Invoke callbacks and the `[UnmanagedCallersOnly]` node
trampolines (a worker-thread throw would otherwise be fatal). Distinct from the fault path,
`hostNotify(ctx, level, msg)` lets a plugin raise its own user-facing toast at any `NotifyLevel` (surfaced to
C# as `IOfsHost.Notify`).

**Plugin lifetime.** Each plugin loads into its own collectible `AssemblyLoadContext`. The managed host
exposes a `CancellationToken` (`Host.UnloadToken`) cancelled on unload so a plugin can stop background
threads/timers/Tasks — a surviving thread roots the ALC, defeats the forced-GC unload, and locks the plugin
DLL (the bootstrapper logs/notifies a warning naming such a plugin). `PluginManager` **must outlive every
plugin** (the managed host holds a raw `HostApi*`/`PluginCtx*` into its members); `PluginManager::shutdown()`
tears every enabled plugin down — `onUnload` + ALC unload — while those members are still alive, invoked
from `OfsApp::~OfsApp` before the manager is destroyed.

## Frame loop and data flows

`drain()` is always first, so worker results from the previous frame are applied before the UI renders — a
one-frame latency for async results, intentional and acceptable. The four-step loop itself is in CLAUDE.md →
*Frame loop*. What that diagram doesn't show is how a single user gesture threads through the queue.

### Axis mutation — a drag gesture (coalesced undo)

On the first drag move the UI pushes a `MoveActionEvent` with `snapshot = true`; every subsequent move
pushes one with `snapshot = false`. At the next `drain()`, in registration order:

1. `UndoSystem`'s `MoveActionEvent` handler takes a snapshot **only** when `snapshot` is true — so the snapshot
   happens once, at gesture start.
2. `ProjectManager`'s handler calls `mutate(role, …)`, whose lambda erases the action at the old time and
   inserts the new one. `mutate()` then syncs the selection, sets `dirty`, and pushes `AxisModifiedEvent`.
3. `ProcessingSystem`'s `AxisModifiedEvent` handler clears `resolved` and returns if the axis has no regions;
   otherwise it cancels any in-flight `pendingEval` and submits a fresh job carrying an `AxisSnapshot` (all
   axes' actions, the region list, resolved node refs).

The `snapshot` flag is what coalesces a whole drag — however many `MoveActionEvent`s it emits — into one
undo step. A discrete action (paste, delete) carries no gesture, so its handler snapshots unconditionally.
Either way the snapshot precedes the mutation, guaranteed by registration order, *not* by any explicit
sequencing at the call site.

### Async evaluation — a worker finishes

The worker evaluates the graph on its `AxisSnapshot`, checking `job->isCancelled()` between regions, then
pushes an `EvalCompleteEvent` carrying the role, the job pointer, and the result. On the next frame's
`drain()`, `ProcessingSystem`'s handler compares `axes[role].pendingEval` against the event's job pointer: if
they differ the result is stale and silently discarded; otherwise it moves the result into
`axes[role].resolved` and resets `pendingEval`. The staleness guard is a pointer comparison against data
already on `AxisState` — no side-tables, no generation counters. A cancelled job's result is dropped here.

## Async job contract

An `EvalJob` (`src/Services/JobSystem.h`) carries two atomics: `currentNodeId`, which the worker writes to
the node it is on and the UI reads to highlight progress, and `cancelled`. `cancelled` is an `int` rather
than a `bool` so its address can be handed to plugin/script discrete nodes as a `const volatile int*`
(`OfsEvalCtx::cancelFlag`) that the managed side polls directly — it cannot read a `std::atomic`. The flag is
monotonic; C++ only ever touches it through `cancel()` / `isCancelled()`. The caller stores the
`shared_ptr<EvalJob>` in `AxisState::pendingEval`, the worker captures the same pointer, and UI reads
`pendingEval != nullptr` to show a spinner. The cancel-before-resubmit and check-`isCancelled()`-at-loop-
boundaries rules are in CLAUDE.md → *Async job contract*.

## Processing nodes

A region's processing graph holds two dynamic, user-authored node kinds beyond the host's native math/glue:
**script nodes** (file-backed Roslyn C# fragments with header-declared scalar params) and **plugin nodes**
(C# DLL nodes with a typed state object and a custom UI callback). Two ground truths shape the whole model:
**.NET is always present** (there is no no-managed build), and **both kinds resolve into one kind-neutral call
object and run through one evaluator** — a plugin node is a script node plus a state channel and a UI
callback.

**One evaluator, resolved on the main thread.** While building the `AxisSnapshot`, every dynamic node — script
and plugin alike — is resolved on the main thread into a single `NodeCallRef` (a function pointer, signal
kind, in/out pin counts, and a `stateHandle`) and copied into the snapshot. A script node is the
`stateHandle == -1` case; a plugin node is the same call-ref plus a value-captured state. The worker then
reads only its snapshot copy, so neither a concurrent recompile (scripts) nor a plugin unload (plugin nodes)
can invalidate an in-flight eval — slots and function pointers are process-lifetime stable, and the only nodes
resolved by live reference are the native effects (`rdp`/`smooth`), which never unload. That single
function-pointer seam is also the test boundary: a native fake `NodeCallRef` drives the full worker eval with
no managed runtime.

**Eval is a pure worker-side function; UI is main-thread only.** A node's compute is a pure function of
`{state, inputs, region bounds}` with no access to `ScriptProject` or any service — it runs on a `JobSystem`
worker off the value-copied snapshot. Node bodies render every frame on the main thread. Moving a node's
editable values from the thread that edits them to the thread that reads them — safely, without the author
thinking about threads — is the entire point of the state model below. Roslyn compilation (~100s of ms) runs
as a `JobSystem` task into a collectible load context, keyed by a content hash so identical sources share one
artifact; a functional node needing an expensive, non-serializable working artifact (a lookup table, an
interpolator) registers a *factory* that runs once per region eval and returns the per-sample closure, keeping
heavy data out of the persisted state.

**Plugin-node state: a value type, copied, never shared.** A plugin node is parameterised by an
author-defined value-type `TState` whose fields *are* the node's parameters (persisted, seen by eval,
undoable) — there is no separate parameter object, and the host owns one instance per node. Value semantics
make the cross-thread handoff a *copy*, not a share: at snapshot build the host value-copies the struct into
the eval's capture set, frozen against later UI edits. The one contract this asks of the author is **replace,
don't mutate** a reference field (assign a new array/list, never mutate it in place) — which keeps a captured
reference immutable from the writer's side and is the same shallow compare that drives change detection (a
difference after the UI callback marks the node dirty and re-evaluates). Crucially, **no state bytes cross the
C ABI**: the native snapshot carries only the integer handle naming the managed capture; the managed
value-copy *is* the data.

**Script nodes have no custom UI, by design.** A script node is pure worker-side compute authored as a
fragment; letting it also draw main-thread UI would run user-typed code in the render loop every frame,
collapsing the line between "a formula in a region" and "a plugin." Its whole surface is its header-declared
scalar params (float/int/bool/enum — never string, path, or color, which are not scalars) plus its body; that
non-scalar/UI capability is exactly what distinguishes a plugin node. The native set is culled on the same
principle: only `rdp` and `smooth` (genuine algorithms) and the zero-param math/structural glue stay native,
while the trivial effects ship as library scripts packed into `data.pak`.

**Persistence and trust.** A plugin node's state is canonically the JSON stored as a nested child of the C++
node; the worker never touches it (it reads the managed value-copy named by the handle), and because C++ never
interprets it, a project authored with a plugin that isn't loaded round-trips that state losslessly. Undo
covers it for free — the JSON rides the region struct the undo system already snapshots. Compiled scripts and
plugins run with full CLR trust and no sandbox, so code that *travels* is gated: a graph preset embeds its
scripts' source and raises a trust prompt before compiling or materializing, plugin DLLs are gated by a
SHA-256 acknowledgement plus a first-party allowlist, and shipped library scripts are auto-trusted by hashing
to a known-shipped set. A plain project only references scripts by name, so it can never ship a body.

## Anti-patterns

| Pattern | Why it's wrong | Correct approach |
|---------|----------------|------------------|
| `ScriptProject*` parameter | Implies nullable ownership | Use `ScriptProject&` |
| Writing axis fields outside `mutate()` | Bypasses dirty flag, selection sync, `AxisModifiedEvent` | `mutate()` for axis fields; write non-axis fields directly inside service handlers |
| Service calling another service's method | Hidden coupling, bypasses the event log | Push an event (registries are the documented exception) |
| UI pushing several events to fake one mutation | UI must not know mutation internals | Define a purpose-specific event |
| Long work inside an event handler | Blocks the frame | Submit to `JobSystem` |
| Passing `ScriptProject&` to a worker | Not thread-safe | Copy to `AxisSnapshot` |
| Storing a pointer into `AxisState` across frames | Array elements are stable but fields change | Re-read each frame |
| Global state or singletons | Hidden dependencies | Explicit construction in `OfsApp` |
| Worker calling a service method | Services are main-thread-only | `push()` the result |

## SceneGraph

The simulator scene graph (`src/Scenegraph/`) is a plain node tree — no ECS. A `SceneNode` carries a local
transform, a cached world transform, an optional `Mesh*` (null = no geometry), a color, and parent/children
links; `SceneGraph` owns node creation/destruction, `updateTransforms()`, and `render()`. See the header for
the exact members.

It is owned by `ScriptSimulator`, not shared. `SceneNode*` pointers are stable for the graph's lifetime;
`updateTransforms()`/`render()` are main-thread-only; and it never touches `ScriptProject` or `EventQueue` —
the owning system reads `ScriptProject::simulator` and updates node transforms each frame. See CLAUDE.md →
*SceneGraph*.

## Plugin API surface

The C# plugin surface (`Ofs.Api`) is its own documentation: the public types carry XML doc comments, and the
StarterPlugin shows a first build. The architectural choices behind that surface, not visible in any one
signature:

- **A plugin author never sees the C ABI.** `unsafe`, `IntPtr`, `delegate* unmanaged`, UTF-8 marshaling, and
  the main-thread queue all live behind `internal` types; a plugin sees ordinary C#. The ABI is internal to
  one shipped unit (native host + its `Ofs.Api`, always rebuilt together) — see CLAUDE.md → *Plugin system*
  for why breaking the ABI does not break plugins and only the `Ofs.Api` assembly version gates compatibility.
- **Axis edits are buffered and committed as one undo step.** A plugin mutates an axis through a buffered edit
  and an explicit `Commit()`; everything between is one atomic undo step (an uncommitted edit is a clean
  no-op, never a partial write). This is the plugin-facing mirror of the snapshot-coalescing invariant above —
  one logical action is exactly one undo state, never per-change.
- **Per-frame and per-call views throw on stale access.** An `Axis` view is valid only for the frame it was
  read in, and node-eval views (`NodeContext`, `DiscreteReader`) only for the callback's duration; both carry
  a generation stamp and throw on stale access rather than returning corrupt data — the same lifetime
  discipline the *Storing a pointer across frames* anti-pattern enforces on the native side.

## Where things live

The plugin system (C ABI, `PluginCtx`, the ABI-version guard) is documented in CLAUDE.md → *Plugin system*
and the C# surface in the `Ofs.Api` source; the interaction extension points and the processing-node model are
in the sections above. The directory-by-directory layering contract is in CLAUDE.md → *Target file layout* —
browse `src/` for the current files rather than relying on a tree duplicated here.
