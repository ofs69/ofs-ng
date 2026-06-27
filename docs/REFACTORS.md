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

- **`Events.h` is a catch-all across unrelated domains** — `src/Core/Events.h`. Bundles input, playback/
  video, axis, region, and localization events. Split into `InputEvents.h` / `PlaybackEvents.h` /
  `AxisEvents.h` / `RegionEvents.h` / `LocalizationEvents.h` (siblings already split this way). Large
  include-churn but mechanical.

- **`VrShader` and `WaveformShader` share one file** — `src/Scenegraph/Shader.{h,cpp}`. Only the abstract
  base `Shader` couples them; each has a single distinct consumer. Split into `VrShader.{h,cpp}` and
  `WaveformShader.{h,cpp}`, leaving base `Shader` + `checkCompileErrors` in `Shader.{h,cpp}`. Update
  consumers (`VideoPlayerWindow.cpp`, `WaveformRenderer.cpp`) and `VrCamera.h` (now includes `Shader.h`
  for `VrShader::hfovDegrees` → should include the new `VrShader.h`).

- **Core event headers include Services-layer headers** — `src/Core/PluginEvents.h`,
  `src/Core/ScriptNodeEvents.h` pull in `Services/*Registry.h` to embed service-defined payloads (only
  `ScriptProject.h`→`JobSystem.h` is the sanctioned upward exception). Move these events into Services,
  or relocate the small payload structs into Core. *(Layering is described, not a stated MUST — low.)*

## Remaining duplication

- **Per-node-type pin arity restated in validator vs evaluator** — `src/Format/Project.cpp` (+
  `ProcessingSystem` / `ProcessingGraphOps`). Add a canonical `nodeInput/OutputPinCount(...)` next to
  `GraphNodeType` and have all sites call it.

- **Hit-test fade/decimation duplicated from the dot renderer** — `src/UI/ScriptTimeline.cpp` (the
  hit-test path vs the dot-draw path; already drifted — hit-test omits `skipAt`/hidden-region filters).
  One shared visible-dot enumeration helper used by both. (The dot-draw side now also precomputes hidden
  intervals — fold that in.)

- **`exportCatalog` / `refreshTranslation` duplicate catalog build/write** — `src/Localization/Translator.cpp`.
  A helper taking a per-key translation fn + header.

- **Footer task-indicator lifecycle duplicated** — `src/Services/WaveformService.cpp` vs
  `VideoTranscoder.cpp` (start/clear the footer task entry). A small `FooterTask` RAII/helper.

- **Node-creation boilerplate across the add-node branches** — `src/UI/ProcessingPanel.cpp` (the
  per-type create blocks each repeat copy-region / allocId / editor-space / push / auto-connect / set-pos;
  the Constant branch hand-rolls its own link splice). A `placeNewNode(...)` helper.
