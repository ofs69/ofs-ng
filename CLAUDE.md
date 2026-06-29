# Engineering Guide

ofs-ng is a funscript editor: libmpv video playback paired with a multi-axis timeline, a live 3D
simulator, a node-based processing graph, and a C# plugin system. See **[README](README.md)** for the
user-facing overview, platform support, and full build prerequisites.

This file is the **engineering contract** for the codebase — the architecture and the rules any change
must respect. It is written for everyone who edits the code, human contributors and AI coding assistants
alike (Claude Code reads it automatically). The rules below are not style preferences: most encode an
invariant that the threading model, the undo system, the localization pipeline, or the plugin ABI
depends on. When in doubt, follow them literally.

## Documentation map

Deeper references live in `docs/`:

| Doc | Covers |
|-----|--------|
| `docs/ARCHITECTURE.md` | The three-primitive design in prose, the interaction extension points, and the processing-node model |
| `docs/TRANSLATING.md` | Maintaining translation catalogs and adding languages via `tools/translations.py` (the `sync`/`todo`/`apply` workflow) |

## Build

See **[README](README.md)** for prerequisites and first-time setup. The commands below use
`cmake-build-debug-visual-studio` (the IDE-generated build directory) — substitute your own configured
directory, e.g. `build`:

```
cmake --build cmake-build-debug-visual-studio -j 8
```

- The no-target build above builds everything; the app itself is the `ofs-ng` target (e.g.
  `cmake --build cmake-build-debug-visual-studio --target ofs-ng -j 8`).
- Warnings are errors (`/WX` on MSVC); clang-tidy runs per-target if found (also warnings-as-errors).
- Format the tree: `cmake --build cmake-build-debug-visual-studio --target format`.

## Test

Build first, then run the whole suite:

```
ctest --test-dir cmake-build-debug-visual-studio --output-on-failure
```

CTest registers four tests: `unit` (no window), `plugins` (PluginManager + real CoreCLR; the .NET
runtime is **mandatory** — a CoreCLR/plugin test that can't init the host **fails**, it never skips),
`ui-smoke` (full window + imgui_test_engine), and `ui-smoke-loc` (the same UI suite re-run under a
machine translation to catch lost `###id`s). Run one with `-R`, e.g.
`ctest --test-dir cmake-build-debug-visual-studio -R ui-smoke --output-on-failure`. To run a single UI
suite or test, pass a filter straight to the binary: `bin/test/ui-tests/ofs-ui-tests --test-filter=plugin_crash`.

## Architecture

The codebase is structured around three primitives. No ECS framework is used.

### Three primitives

| Type | Role |
|------|------|
| `ScriptProject` | Single source of truth for all project state — a plain C++ struct hierarchy |
| `EventQueue` | The only channel for cross-system communication — typed, deferred, thread-safe |
| Services | Behavior units that read `ScriptProject` and push events |

`OfsApp` owns all three and composes them. No framework governs the structure.

### ScriptProject

`src/Core/ScriptProject.h` — owns `AxisState axes[kStandardAxisCount]` (indexed by the `StandardAxis`
enum) plus all top-level state structs: `state`, `overlay`, `simulator`, `videoPlayer`, `metadata`,
`bookmarks`, `playback`, `timelineView`, the always-sorted `regions` vector, the opaque per-plugin
`pluginData` store (round-trips with save; the host never interprets it), per-chapter scene-view memory,
the transient graph-load / auto-eval flags, and the `active*` / `stored*` interaction-selection ids. The
header is the source of truth for the full field list and each field's transient/serialization notes; the
non-obvious invariants are below.

**Rules:**
- **MUST** pass `ScriptProject` as `ScriptProject&`. Never store a `ScriptProject*`.
- **MUST** mutate axis fields only through `ScriptProject::mutate(role, fn, eq)`. This applies `fn`, syncs the selection (removes entries no longer in `actions`), sets `axis.dirty = true`, and pushes `AxisModifiedEvent`. Pass `affectsData=false` for a **display-only** flag write (`isVisible`/`isLocked`/`showInStrip`): the change still persists (dirty + `editRevision`) but skips the `AxisModifiedEvent` push, so it does not kick off a processing re-eval / plugin notify the action data never warrants. It does **not** snapshot undo state — `UndoSystem` auto-snapshots by registering `on<E>()` handlers for all undoable event types before `ProjectManager` does (registration order is preserved by `EventQueue`). The undoable action-mutation events the `EditIntentRouter` emits (`AddActionAtTimeEvent`, `MoveActionEvent`, `RemoveActionAtTimeEvent`, `RemoveSelectedActionsEvent`, `PasteActionsEvent`, `MoveSelectionTime/PositionEvent`) carry a `bool snapshot` field that the router **host-stamps** via a per-gesture latch — `true` on the first mutation of a gesture, `false` on the 2nd..Nth — so a multi-mutation `Replace` (even one mixing adds/removes) coalesces into a single undo step. Each defaults to `true`, so a standalone (non-router) push snapshots on its own. `ModifyRegionEvent` carries the same flag but is UI-set `true` only on gesture start. See *Interaction extension points* in `docs/ARCHITECTURE.md`.
- Non-axis fields (`state`, `overlay`, `simulator`, `videoPlayer`, `metadata`, `bookmarks`, `playback`, `timelineView`, `regions`, `procSelRegionId`, `pluginData`, `defaultSceneView`/`activeSceneView`, `pendingGraphLoad`, `autoEvalEnabled`, `activeEditMode`/`activeNavigator`/`activeSelectionMode` and their `stored*` counterparts) may be written directly inside service event handlers. There is no `mutate` wrapper for them. (The six interaction-selection ids are written only by the `EditIntentRouter` / `NavigatorRouter` / `SelectIntentRouter`; see *Interaction extension points* in `docs/ARCHITECTURE.md`.)
- `AxisState::resolved` and `AxisState::pendingEval` are transient processing/job fields, not document state. `ProcessingSystem` manages them directly; they are exempt from the `mutate()` rule to avoid recursive `AxisModifiedEvent` loops.
- **MUST NOT** access `ScriptProject` from a worker thread. Workers receive an `AxisSnapshot` (a value copy), never a reference.

### EventQueue

`src/Core/EventQueue.h` — backed by `moodycamel::ConcurrentQueue`. Handlers registered with `on<E>()` are always called on the main thread. `push<E>()` is thread-safe and callable from any thread.

**Rules:**
- **MUST** register all handlers before the first worker thread is created (in constructors or `OfsApp::init()`). The handler table is effectively read-only during operation.
- **MUST** call `eventQueue.freeze()` after all `on<E>()` registrations and before `jobSystem.start()`. `freeze()` asserts at runtime if any `on<E>()` call is made afterwards, preventing registration races.
- **MUST** call `drain()` exactly once per frame, as the first operation in `onUpdate()`.
- **MUST NOT** call `drain()` from anywhere other than the main loop.
- **MUST NOT** call service methods directly from another service. Push an event instead.
- **MUST NOT** call service methods directly from UI windows. Push an event instead.
- Event handlers **MUST** return quickly (within the ~16 ms frame budget). Long work is posted to `JobSystem`.
- Workers **MUST** communicate results via `push()`. They **MUST NOT** call any service method or touch `ScriptProject` directly.

### Services

Services own behavior. They do not own project state (that lives in `ScriptProject`). Services may own internal non-project state: undo history, thread pool handles, plugin file handles, video player state managed by libmpv, etc.

| Service | Owns |
|---------|------|
| `ProjectManager` | File I/O, axis/selection mutations, backup timer |
| `ProcessingSystem` | Node graph evaluation, triggers `JobSystem` workers |
| `JobSystem` | BS::thread_pool wrapper, `EvalJob` handles |
| `UndoSystem` | Undo/redo stack |
| `VideoPlayer` / `MpvVideoPlayer` | libmpv handle, playback state |
| `PluginManager` | Loaded C# plugin list, .NET CLR host |
| `ScriptSystem` / `ScriptRegistry` / `ScriptWatch` | Compiled C# script nodes (see *Processing nodes* in `docs/ARCHITECTURE.md`) |
| `BindingSystem` | Key binding table and binding presets |
| `CommandRegistry` | Command palette / invocable command table |
| `EffectRegistry` | Built-in and plugin-contributed effects |
| `UpdateChecker` | Off-thread check of the GitHub releases feed against the build's git tag (HTTP via the .NET-backed `Ofs.HostServices`, see *Plugin system*) |

**Rules:**
- **MUST** accept `ScriptProject&` and `EventQueue&` by reference. Never store `Application*` or `OfsApp*`.
- **MUST NOT** call another service's methods directly. Push an event instead. **Exempt:** the
  passive-lookup registries — `CommandRegistry` (incl. `run()`/`find()`), `EditModeRegistry` /
  `NavigatorRegistry` / `SelectionModeRegistry`, and `EffectRegistry` — are a read/dispatch seam, not
  behavior-owning services. A service or UI window may query them (`find`/`entries`/`all`) and the
  routers/`BindingSystem` may invoke `CommandRegistry::run` directly; this routes no state mutation
  through a hidden back channel, so the event-log rule doesn't apply. Mind their validity window
  (results dangle across a plugin (re)load — see the registry headers).
- Event handlers **MUST** return within a single frame budget. Post long work to `JobSystem`.
- **MUST NOT** spawn untracked threads. All background work goes through `JobSystem`.
- **MUST NOT** introduce global state or singletons. Every service is constructed explicitly in `OfsApp` and reaches its collaborators through references and the event queue.
- Use `jobSystem.submit(job, snap, fn)` for axis evaluation work (cancellable, carries `AxisSnapshot`). Use `jobSystem.submitTask(fn)` for general fire-and-forget work (returns `std::future<bool>`). Do not use `std::async`.

### UI windows

`src/UI/` — ImGui immediate-mode windows. Pure renderers that own no persistent state except transient frame-local variables (e.g. `ImGui::InputText` buffers).

**Rules:**
- Render functions **MUST** take `const ScriptProject&` for reads and `EventQueue&` for the write path.
- **MUST NOT** mutate `ScriptProject` directly. All user actions produce an `EventQueue::push()`.
- **MUST NOT** push several events to simulate one mutation — the UI must not know mutation internals. Define a purpose-specific event instead.
- **MUST NOT** contain business logic. No data merging, no format conversion beyond display.
- **MUST NOT** store pointers into `ScriptProject` across frames. Read fields fresh each render.
- Before implementing any ImGui widget, check `lib/imgui/imgui_demo.cpp` for the canonical usage.

### Allocation in hot paths

ImGui render functions execute every frame. Heap allocation inside them causes per-frame malloc/free churn, GC pressure on the allocator, and cache misses.

**Rules:**
- **MUST NOT** construct `std::string` or `std::vector` inside render functions or any other per-frame hot path.
- **MUST** use `fmtScratch(...)` (`src/Util/FrameAllocator.h`) to produce label and display strings. It writes into the pre-allocated frame arena and returns a `const char*` valid until the next `FrameAllocator::reset()` call (which happens once per frame).
- **MUST NOT** call `FrameAllocator::reset()` from anywhere except the top of the frame loop. One reset per frame keeps all `fmtScratch` pointers alive for the full render pass.
- For temporary arrays needed during a render pass, prefer `FrameAllocator::allocArray<T>(count)` over `std::vector<T>`.
- Heap allocation is acceptable outside hot paths: one-time init, file I/O, event handler responses that fire infrequently.

### Localized strings

Every user-visible string is defined once in `tools/localization/strings.toml` (the English source of truth)
and reached through the generated `Tr` enum / `Str::` constants — never typed as a literal in a render
path. A build step (`localization_gen`) regenerates `StringsGenerated.{h,cpp}` from the catalog and
**validates every `lang/*.toml` against it** (missing key, empty translation, `{N}` placeholder
mismatch, or unknown key fails the build).

- **MUST NOT** pass a user-visible string literal to any ImGui call in a render path — a label,
  button, menu item, header, tooltip, combo entry, modal title/body, or `SeparatorText`. Add a key to
  `strings.toml` and use the `Str::` constant. (Pure structural/`##hidden` ids and `printf` format
  specifiers like `"%.0f px"` are not user-visible text and stay literal.)
- **MUST** use the right `TrKey` spelling for the call site: implicit `const char*` for a plain
  `const char*` parameter (`Button(Str::PrefDelete)`); `.id("slug")` for a translatable label that
  needs a stable widget id (`Str::PrefTitle.id("preferences")`); `.fmt(args…)` for a string with `{N}`
  placeholders (validated, frame-arena allocated); `.c_str()` only where the implicit conversion can't
  fire — a `std::string` parameter, an `fmt`/`fmtScratch` argument, or a `printf`-style vararg
  (`TextDisabled("%s", k.c_str())`). Use `key.icon(ICON)` to prefix an icon glyph, or
  `key.iconId(ICON, "slug")` when the icon+label widget also needs a stable `###id`.
- **MUST** add the matching key (with `english` + `description`, and `placeholders` for formatted
  strings) to `strings.toml`, then run `python tools/translations.py sync` to propagate the change into
  **every** language catalog — do not hand-edit the per-language files. An out-of-sync translation file
  fails the build. All catalogs live in `lang/*.toml` and are strictly validated, so a catalog stays
  broken-build until every key is translated. AI-translated catalogs carry an `_[AI]` filename
  suffix (e.g. `lang/jp_[AI].toml`) so the language picker shows machine translations as such. The full
  suite runs twice — built-in English and `jp_[AI]` (ctest `ui-smoke` + `ui-smoke-loc`) — so a label
  that lost its `###id` is caught. See `docs/TRANSLATING.md` for the full `sync`/`todo`/`apply` workflow.
- Theme color/var **swatch** labels (the ~100 entries keyed off `kAxisColors` &c.) are intentionally
  left unlocalized — don't "fix" them ad hoc.

### Localization-ready UI layout

The UI is localized: any displayed string may be replaced by a longer translation, and the app runs
at varying font size / DPI. A **literal pixel size** is therefore wrong on two axes — it neither
survives a longer string nor scales with the font. The rules that keep new UI from regressing:

- **MUST NOT** pass a literal pixel width to anything that holds or positions translatable text:
  `SetNextItemWidth(220.f)`, a widget size `{110, 0}`, `TableSetupColumn(…, WidthFixed, 120.f)`,
  `SetNextWindowSize`/`…Constraints`, `SameLine(absoluteX)`, `SetCursorPosX(lit)`, `PushTextWrapPos(lit)`.
  Use a content/font-relative value instead: `-FLT_MIN` (fill), `GetContentRegionAvail().x - reserved`,
  or `GetFontSize() * k` / `GetFrameHeight()` / `GetTextLineHeight()` multiples.
- **MUST** size buttons and labels to their content — auto-size `{0, 0}`, or
  `CalcTextSize(label).x + 2 * FramePadding.x`, optionally clamped to a font-relative minimum so a row
  stays even. Never hardcode a verb button to `{60,0}`/`{110,0}`/`{150,0}`.
- **MUST NOT** size a container to a hardcoded **English** literal (`CalcTextSize("Press")`). Measure
  the *actual* (translated) string you will render. Same for `Combo` item lists baked as `"A\0B\0"`.
- **MUST NOT** lay out label→value rows with absolute `SameLine(offset)` / `SetCursorPosX` that assume
  the English label width, and **MUST NOT** align columns with hand-padded spaces in a format string.
  Use a 2-column table (`ofs::ui::beginForm`/`formRow`) or `SameLine()` after measuring.
- Draw-list text (`ImDrawList::AddText`, `ofs::ui::addTextShadow`) has **no** auto-size, auto-clip, or
  elision. When it can overflow its area you **MUST** measure and **elide** (append `…`); never
  fit-or-drop (silently hide) or overrun a neighbor.
- Input fields that may receive localized text **MUST** use the `std::string`-bound `InputText`
  overload, not a fixed `char[]` — the byte cap truncates UTF-8 (a CJK glyph is 3 bytes, so `char[128]`
  holds ~42 characters).
- **Widget identity:** every widget or window whose visible label is translatable **MUST** carry a
  stable, language-independent `###id` suffix (e.g. `Begin("Timeline###timeline")`,
  `Checkbox("Invert###sim_invert")`). A `##suffix` is **not** enough — the visible prefix stays in the
  ID hash, so a translated label silently becomes a different widget (lost state, reset dock/ini,
  broken `imgui_test_engine` refs). The id after `###` must be unique across the app.

Format/measure strings still go through `fmtScratch` per the Allocation rules above; these rules govern
the *sizes and ids*, not the allocation.

### Path and filename encoding (Windows-safe)

Paths must survive non-ASCII characters (e.g. a `José`/`日本語` username under `%APPDATA%`, or a video in such a folder). On Windows `std::filesystem::path` stores `wchar_t`, but its **narrow** interface uses the legacy ANSI codepage, which silently drops anything outside it. Every external library we use (SDL — including its native file dialogs, libmpv, ImGui, nlohmann_json, the C# plugin ABI) speaks **UTF-8**. So the one invariant is:

> Every `std::string` / `const char*` that holds a path is **UTF-8**. `std::filesystem::path` is the lossless carrier. Convert only at the boundary, only through the two helpers in `src/Util/PathUtil.h`.

**Rules:**
- **MUST** convert `path` → `std::string` with `ofs::util::toUtf8(path)`. **MUST NOT** use `path.string()` / `path.generic_string()` (lossy ANSI on Windows).
- **MUST** convert `std::string`/`const char*` → `path` with `ofs::util::fromUtf8(utf8)`. **MUST NOT** use the narrow `std::filesystem::path(str)` constructor, `path / str`, or `path += str` with a narrow string — they decode as ANSI. (Constructing from a `std::wstring`/`std::u8string` is fine — mark such a line `// utf8-ok`.)
- **MUST** convert at the boundary when a UTF-8 `std::string` path reaches `std::filesystem` (e.g. `std::filesystem::exists(fromUtf8(s))`, not `std::filesystem::exists(s)`). Implicit narrow conversions are the silent failure mode.
- Keep paths as `std::filesystem::path` for as long as possible; reach for a `std::string` only to hand UTF-8 to JSON, libmpv, ImGui (`io.IniFilename`), a dialog, or the plugin C ABI.
- spdlog is built with `SPDLOG_WCHAR_FILENAMES` (`lib/CMakeLists.txt`); pass the file sink `path.wstring()` on Windows so the log path survives a non-ASCII pref dir. Win32 calls take the wide API directly (`path.c_str()` is `const wchar_t*`).
- Run `python tools/check_path_encoding.py` before committing path-touching changes. It flags `.string()` and narrow `path(...)` construction; the `// utf8-ok` marker exempts an intentional wide/u8 conversion. It cannot see implicit conversions, so the rules above still apply by hand.

### Threading model

| Thread | Allowed operations |
|--------|--------------------|
| **Main** | `EventQueue::drain()`, `ScriptProject::mutate()`, all service event handlers, all ImGui rendering, `JobSystem::submit()` |
| **Worker N** | Read `AxisSnapshot` (value copy), compute, `EventQueue::push()` result |

Workers never touch `ScriptProject`. Workers never call service methods. The only cross-thread mechanism is `EventQueue::push()`.

Plugin callbacks are always invoked from the main thread. Plugins must return within the frame budget.

### Frame loop

```
OfsApp::onUpdate(dt):
  1. eventQueue.drain()         // apply all queued events, including worker results from last frame
  2. projectManager->update(dt) // file save polling, backup timer
  3. videoPlayer->update(dt)
  4. onImGuiRender()            // UI windows push events for user actions; no ScriptProject mutation here
```

`drain()` is always first. Worker results from the previous frame are applied before the UI renders — one-frame latency for async results is intentional.

### Async job contract

`onAxisModified` creates a `shared_ptr<EvalJob>`, stores it in `AxisState::pendingEval`, and calls `JobSystem::submit(job, snap, fn)`. The worker captures the same `shared_ptr`. UI reads `pendingEval != nullptr` to show a progress indicator.

- Workers **MUST** check `job->isCancelled()` at every natural loop boundary (between regions, between large action batches) and return early if cancelled.
- When a new job is submitted for an axis, **MUST** call `pendingEval->cancel()` on the previous job first.
- A cancelled job's result is silently discarded by the staleness guard in `onEvalComplete` (pointer comparison against `AxisState::pendingEval`).
- `AxisSnapshot` carries a full copy of all axes' actions (`axes[kStandardAxisCount]`) plus the region list so multi-axis graph evaluation needs no further access to `ScriptProject`.

### SceneGraph

`src/Scenegraph/` — a plain node tree (`SceneNode` / `SceneGraph`). Owned by `ScriptSimulator`.

**Rules:**
- `updateTransforms()` and `render()` are main-thread-only.
- `SceneNode*` pointers are stable for the lifetime of their owning `SceneGraph`.
- `SceneGraph` does not interact with `ScriptProject` or `EventQueue` directly. The owning system reads `ScriptProject::simulator` and updates node transforms each frame.

## Key data types

- **`StandardAxis`** (`src/Core/StandardAxis.h`): the 20 axis roles (`kStandardAxisCount = 20`) — `L0–L2, R0–R2, V0–V1, A0–A1` plus the user-created scratch axes `S0–S9`. Axes are accessed by index (`scriptProject.axes[role]`); no sorting needed.
- **`ProcessingRegion`** (`src/Core/ProcessingRegion.h`): time range with a `ProcessingNodeGraph` (imnodes). A node declares **N input pins and M output pins** (0..16 in, 1..16 out — `OFS_MAX_NODE_PINS`), so the imnodes id space is a tagged bit layout encapsulated in the **`GraphId`** helper, not a fixed stride. Every imnodes-facing id is minted/parsed through `GraphId` (`nodeBody`/`outPin(n,slot)`/`inPin(n,slot)`/`staticAttr`/`link`): bits 31..9 = owner record id, 8..6 = tag (NodeBody/OutPin/InPin/Static/Link), 5..0 = pin slot (0..63). The tag makes it collision-free by construction — **never hand-encode an imnodes id**, always go through `GraphId`. Links carry `fromPin` (source output slot) and `toPin` (target input slot).
- **`VectorSet<ScriptAxisAction>`**: sorted set of `{double at, int pos}` used for axis actions and selections.
- **`AxisSnapshot`** (`src/Services/JobSystem.h`): a value copy of the axis data — every axis's actions, the region list, and the resolved plugin/script node references — handed to a worker. It is the only safe way to pass axis data across a thread boundary; multi-axis region evaluation reads only the snapshot, never `ScriptProject`. See the header for the exact members.

## Comments

A comment must earn its place by saying something the code cannot. Comment the **why**, not the **what**.

**MUST NOT** write superficial comments — ones that restate what the next line already says, label an obvious block, or narrate a routine step:

```cpp
i++;                              // increment i
// loop over the axes
for (auto &axis : axes) { ... }
// set the background color
dl->AddRectFilled(min, max, bg);
```

These add visual noise, drift out of sync with the code, and bury the comments that actually carry information.

**MUST NOT** write *tombstone* comments — ones that narrate code that is no longer there or justify the **absence** of an action. When you delete or change code, delete its comment too; do not leave a note explaining what used to happen or why a line is gone:

```cpp
// no select-left here — the host ships a native one
resolveActiveMedia();   // not stored in the file
// nothing to persist, so we don't mark the project dirty
project.state.intraMediaPath = path;
```

The reader sees the code that *exists*; a comment about code that *doesn't* only rots and misleads. If the rationale for a removal matters, it belongs in the commit message, not at the deletion site. (Documenting an enduring design property of a type — e.g. "this field is transient, not serialized" on the field itself — is fine; narrating a specific edit is not.)

**SHOULD** write a comment when it captures something not visible at the call site: the rationale for a non-obvious choice, an invariant or ordering constraint the reader must preserve, a subtle edge case, a workaround for an external-library quirk (with a reference), or a deliberate deviation from the obvious approach. Prefer clarifying names and structure over a comment; reach for a comment only when the intent still won't fit in the code itself. The existing rationale comments in `Theme.cpp`, `EventQueue.h`, and the `ScriptProject` mutation rules are the bar — match that, not filler.

## Backwards-compatibility markers

Now that the app is in production (see the README/release notes), code that exists **only** to keep reading or migrating data written by an older build — a legacy serialization default, a field absent in old files, a one-way migration of stale on-disk/pref/`Ofs.Api` state — **MUST** carry a dedicated, greppable tag so the compatibility debt is auditable and can be retired deliberately rather than rediscovered by accident:

```cpp
// COMPAT(YYYY-MM-DD): <what old data/behavior this supports; the condition under which it can be removed>
```

- The date is **when the shim was added**, not a deadline — it tells a future reader how fresh (or stale) the concern is.
- Grep `COMPAT(` to enumerate every compatibility shim and its age. Once the named retirement condition holds (a major-version boundary, or enough time that no pre-date files remain), the block can go.
- Tag the **backward-reading** path, not a forward-compatible write. An additive JSON field that old builds simply ignore needs no tag; the **new** code's "default this field when absent" branch is the shim, and that is what gets the marker.
- This is **not** a license to add back-compat for data that never shipped — the single-build native `HostApi` is rebuilt with its `Ofs.Api`, so a host-side pointer there can't legitimately be null. Only real version skew (old file/plugin/pref vs. newer host) earns a `COMPAT` tag.

## Libraries

### Keep

| Library | Role |
|---------|------|
| SDL3 | Window, input, OpenGL context, native file/folder dialogs |
| Dear ImGui (docking) | Immediate-mode UI |
| imnodes | Node graph widget (`ProcessingPanel`) |
| libmpv | Video playback |
| spdlog / fmt | Logging and string formatting |
| nlohmann\_json | JSON serialization |
| glm | Math |
| glad2 | OpenGL loader |
| moodycamel::ConcurrentQueue | Lock-free MPMC queue; backs `EventQueue::push()` |
| BS::thread\_pool | Fixed-size thread pool; backs `JobSystem` |

## Plugin system

C ABI (`HostApi` / `PluginApi` structs of function pointers, `src/Services/PluginApi.h`). Plugins are C# DLLs loaded via .NET CoreCLR (`DotNetHost`). Shipped first-party plugins are built to `bin/managed/plugins/<name>/` (under the managed dir, not a top-level `bin/plugins/` users would mistake for their install folder); user-installed plugins live in the writable `<pref>/plugins/`. The native↔managed struct layout is guarded by `OFS_ABI_VERSION` (must match the C# mirror in `Ofs.Api/PluginAbi.cs`); whether a given *plugin* is compatible is a separate check against the `Ofs.Api` assembly version in `PluginBootstrapper`. C# script nodes are compiled at runtime by `ScriptSystem` — see *Processing nodes* in `docs/ARCHITECTURE.md`.

**Host-internal managed code (NOT a plugin).** The same CoreCLR runtime also hosts **`Ofs.HostServices`** (source in `managed/Ofs.HostServices/`, built to `bin/<cfg>/managed/` next to `Ofs.Api`/`Ofs.PluginHost`/`Ofs.ScriptHost`). It is deliberately standalone — references no `Ofs.Api`, is never loaded through `PluginLoadContext`, and touches none of the plugin/script ABI — so the native host loads it through its **own** `DotNetHost` and calls its `[UnmanagedCallersOnly]` entry points directly. It exists to lean on the already-mandatory .NET runtime instead of vendoring a native library: today it backs the app's only outbound HTTP (an `HttpClient` GET behind `ofs::util::httpGet`, brought up by `initManagedHttp()`; the response crosses back through a native callback sink, not a returned buffer). That is why **no libcurl/native TLS stack ships**. Like the plugin/script hosts it is trust-gated — its bytes are verified against a baked SHA-256 (it is in `OFS_MANAGED_ASSEMBLIES`) before load. When adding such host-internal .NET helpers, keep them out of `managed/plugins/` and free of `Ofs.Api`.

**ABI vs API — breaking the ABI does *not* break plugins; only breaking the API does.** At load, `PluginLoadContext` discards the `Ofs.Api` a plugin ships and binds it to the *host's own* copy (`PluginBootstrapper.cs:22-29`), and every ABI struct (`HostApi`, `PluginApi`, `OfsNodeDef`, `OfsEvalCtx`, `OfsEditIntent`, …) is `internal` to `Ofs.Api`. So the C ABI is internal to one shipped unit (native host + its `Ofs.Api`, always rebuilt together); a third-party plugin couples only to the *public* C# surface, which the host builds from the ABI structs inside its own trampolines. Consequence: growing/reordering a marshaled struct is a coordinated host rebuild (bump `OFS_ABI_VERSION` — a build-consistency guard, **not** a plugin gate), transparent to already-built plugins. There are no native-ABI one-way doors and "reserve space in the struct now" is a non-goal. The *only* plugin-compatibility gate is the `Ofs.Api` assembly version (`Major`-match, `plugin ≤ host`); that public surface — and bumping its version on additive changes — is the real stabilization contract. Any ABI change visible to a plugin is, by definition, a public-surface (API) change.

Every `HostApi` function pointer takes `void* ctx` as its first argument. The caller passes `hostApi.ctx`, which points to the `PluginCtx` struct (`src/Services/PluginManager.h`) stored inside `PluginManager` — it carries pointers to the project, event queue, services, and per-render-pass UI state. This eliminates the need for file-scope statics. The C# `OfsHost` and `UiBuilder` wrappers read `_api->Ctx` and forward it on every call.

Plugin callbacks run on the main thread and must return within the frame budget. Do not introduce new `std::async`. Do not write `ScriptProject` fields directly from outside a service event handler (use `mutate()` for axis fields; write non-axis fields directly within service event handlers).

## Mouse interactions

The full pointer-gesture reference (timeline curve, band bar, bookmarks, simulator overlay, and the
swappable edit/select/step modes) lives in **[CHEATSHEET.md](CHEATSHEET.md)** — the canonical source.
When you add or change a mouse gesture, update that file.

## Target file layout

`src/` is organized by architectural layer, one directory per concern: **`App/`** (SDL/ImGui init and
the `OfsApp` composition root), **`Core/`** (`ScriptProject`, `EventQueue`, events, and all project-state
types), **`Services/`** (the behavior units from the Services table above), **`Platform/`** (SDL/OpenGL/
ImGui backends, the `DotNetHost` CoreCLR host, headless support), **`UI/`** (render-only windows),
**`Video/`** (the libmpv engine), and the **`Scenegraph/`**, **`Format/`**, **`Util/`**, and
**`Localization/`** supporting layers. Browse `src/` for the current files — this is the layering
contract, not an exhaustive listing.

## Conceptual Cohesion

A file may define more than one type only when those types are so tightly coupled they **cannot meaningfully exist independently**. The test: could type B be moved to its own file without also having to move type A? If yes, it must be in its own file.

**Allowed in one file:**
- A class and its directly-owned state enum or flags (e.g. `ButtonState` next to `Button`)
- Two or three structs that together define a single ABI or protocol boundary and have no independent consumers (e.g. `HostApi` + `PluginApi` version constants in `PluginApi.h`)
- A type and the `to_json`/`from_json` overloads for that exact type

**Not allowed:**
- Unrelated state structs whose only shared trait is that they end up inside the same owner struct
- A catch-all file that collects miscellaneous small structs from different conceptual domains
- Events from unrelated domains in one file (prefer domain-grouped event files, e.g. `PlaybackEvents.h`, `AxisEvents.h`)
