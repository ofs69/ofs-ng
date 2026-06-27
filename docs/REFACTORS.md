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

## Router / registry templating

- **Triplicated mode-registry infrastructure (C#)** — `plugins/Ofs.Api/Editing.cs`, `Navigation.cs`,
  `Selection.cs` (~150 lines each: slot record, lock/list, `GetSlot`/`ReleaseOwnedSlots`, `RegisterMode`
  preamble, trampolines). Extract a generic `ModeRegistry<TSlot>` base for the slot bookkeeping. Note:
  `[UnmanagedCallersOnly]` trampolines can't move into a generic — leave those per-registry.

- **`ReleaseOwnedSlots` slot-nulling loop duplicated in 4 registries (C#)** — `Nodes.cs`, `Editing.cs`,
  `Navigation.cs`, `Selection.cs`. A shared base / static `OwnedSlots` helper. Folds into the
  `ModeRegistry<TSlot>` work above.

> C# changes need a managed rebuild and are higher-risk (the public `Ofs.Api` surface is the plugin
> stabilization contract — keep these internal/non-breaking).

## C# marshaling / loops

- **Per-widget UTF-8 marshaling boilerplate (~20 sites)** — `plugins/Ofs.Api/Ui.cs`. A caller-allocated
  span + a helper centralizing the stackalloc-vs-heap / `+1` NUL policy.

## Remaining duplication

- **Hit-test fade/decimation duplicated from the dot renderer** — `src/UI/ScriptTimeline.cpp` (the
  hit-test path vs the dot-draw path; already drifted — hit-test omits `skipAt`/hidden-region filters).
  One shared visible-dot enumeration helper used by both. (The dot-draw side now also precomputes hidden
  intervals — fold that in.)

