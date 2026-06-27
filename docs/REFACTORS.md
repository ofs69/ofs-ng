# Outstanding refactors

Deferred items from the code-smell audit. Each is a larger, regression-prone change that deserves its
own focused pass (build + `ctest` between steps), so they were left out of the broad cleanup batch. The
correctness fixes, localization, hot-path, naming/magic-number, and the smaller duplication findings are
already done.

Line numbers are approximate — several files moved during the cleanup. Locate by symbol/function.

## Long-function splits

- **`ScriptTimelineWindow::renderTimeline` (~650 lines)** — `src/UI/ScriptTimeline.cpp`.
  Strip layout, multi-axis edit-set resolution, stroke/dot machinery, the selection box, and the full
  context/overlay menu live in one scope. Split into `renderStrip` / `renderCurves` /
  `renderContextMenu` / `renderOverlayMenu` (mirror the `ProcessingPanel::render` split already done:
  extract methods, thread the frame-local state through params). Highest-risk of the three — heavy
  ImGui/draw-list ordering.

- **`openTranscodeOptionsModal` (~220 lines, ~170-line nested body lambda)** — `src/App/OfsApp.cpp`.
  Nested `start`/`guardedStart` lambdas plus the full scale/timing/codec/quality UI. Peer modals are
  already factored into methods — follow that pattern: render helpers + a `startTranscode` helper.

## Router / registry templating

- **Three C++ interaction routers duplicate mode-lifecycle handlers** —
  `src/Services/EditIntentRouter.cpp`, `NavigatorRouter.cpp`, `SelectIntentRouter.cpp`.
  `onSetActive* / onRegister* / onUnregister* / onProjectLoaded` are structurally identical. Introduce a
  templated base / free helper parameterized on (registry, native id, field accessors). Watch the
  per-router selection-id write ownership documented in CLAUDE.md.

- **Triplicated mode-registry infrastructure (C#)** — `plugins/Ofs.Api/Editing.cs`, `Navigation.cs`,
  `Selection.cs` (~150 lines each: slot record, lock/list, `GetSlot`/`ReleaseOwnedSlots`, `RegisterMode`
  preamble, trampolines). Extract a generic `ModeRegistry<TSlot>` base for the slot bookkeeping. Note:
  `[UnmanagedCallersOnly]` trampolines can't move into a generic — leave those per-registry.

- **`ReleaseOwnedSlots` slot-nulling loop duplicated in 4 registries (C#)** — `Nodes.cs`, `Editing.cs`,
  `Navigation.cs`, `Selection.cs`. A shared base / static `OwnedSlots` helper. Folds into the
  `ModeRegistry<TSlot>` work above.

- **`AppScoped<T>` and `ProjectScoped<T>` near-identical (C#)** — `plugins/Ofs.Api/AppScoped.cs`,
  `ProjectScoped.cs`. Differ only in the backing-store calls. Shared non-public base / strategy holding
  the load/save callbacks.

> C# changes need a managed rebuild and are higher-risk (the public `Ofs.Api` surface is the plugin
> stabilization contract — keep these internal/non-breaking).

## C# marshaling / loops

- **Per-widget UTF-8 marshaling boilerplate (~20 sites)** — `plugins/Ofs.Api/Ui.cs`. A caller-allocated
  span + a helper centralizing the stackalloc-vs-heap / `+1` NUL policy.

- **Three name-overflow reread loops in `Project` getters** — `plugins/Ofs.Api/Project.cs`. A generic
  count/loop/stack-buffer/reread helper taking a per-record read delegate.

## Cohesion / file structure

## Remaining duplication

- **Hit-test fade/decimation duplicated from the dot renderer** — `src/UI/ScriptTimeline.cpp` (the
  hit-test path vs the dot-draw path; already drifted — hit-test omits `skipAt`/hidden-region filters).
  One shared visible-dot enumeration helper used by both. (The dot-draw side now also precomputes hidden
  intervals — fold that in.)

