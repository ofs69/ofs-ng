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

> C# changes need a managed rebuild and are higher-risk (the public `Ofs.Api` surface is the plugin
> stabilization contract — keep these internal/non-breaking).

## C# marshaling / loops

## Remaining duplication

- **Hit-test fade/decimation duplicated from the dot renderer** — `src/UI/ScriptTimeline.cpp` (the
  hit-test path vs the dot-draw path; already drifted — hit-test omits `skipAt`/hidden-region filters).
  One shared visible-dot enumeration helper used by both. (The dot-draw side now also precomputes hidden
  intervals — fold that in.)

