<p align="center">
  <img src="data/icons/logo256.png" alt="ofs-ng logo" width="300">
</p>

# ofs-ng

A funscript editor. It pairs libmpv video playback with a multi-axis timeline,
a live simulator, a node-based processing graph, and a C# plugin system.

A complete rewrite of OpenFunscripter in modern C++20, built on largely the same stack —
SDL3, Dear ImGui, OpenGL, and libmpv.

## Features

- **Multi-axis timeline** — edit every standard axis (stroke, surge, sway, twist, roll, pitch, …)
  alongside scratch axes on one synchronized timeline.
- **Live 3D simulator** — a real-time 3D preview that plays back the script as you edit.
- **Node-based processing graph** — compose per-region effects and transforms in a visual node editor.
- **C# plugin system** — extend the editor with managed .NET plugins; no C ABI to deal with.
- **Localized UI** — every string is translatable, with multiple languages shipped.

## Platform support

| Platform | Status |
|----------|--------|
| **Windows** | Fully supported — build, run, and debug. |
| **Linux** | Fully supported — build, run, and debug. |
| **macOS** | Indirect only. The codebase is cross-platform and is expected to build and run, but it is **not directly supported** — we don't build, test, or debug on macOS. |

## Getting the source

The project uses git submodules for its third-party libraries, so clone recursively:

```bash
git clone --recurse-submodules <repository-url>
cd ofs-ng
```

If you already cloned without `--recurse-submodules`:

```bash
git submodule update --init --recursive
```

## Building

ofs-ng builds with CMake. You need:

- A **C++20 toolchain** — **MSVC** (Windows) or **Clang** (Linux). GCC is currently
  **not supported**: it hits an internal compiler error (ICE) building the project.
- **CMake** 3.16+
- The **.NET SDK** — required in practice. The build links **NetHost** (via
  `cmake/FindNetHost.cmake`), and the C# plugin system and runtime-compiled script nodes depend on
  it. A `dotnet`-less build completes but ships none of that functionality — treat it as a hard
  requirement.
- **libmpv** — auto-downloaded on the first Windows build; install via your package manager
  on Linux
- **ffmpeg** and **ffprobe** — used at runtime for video transcoding. Bundled next to the
  executable on Windows; on Linux/macOS they're resolved from `PATH`, so install them with your
  package manager (a single `ffmpeg` package provides both binaries).

```bash
# Configure (use your platform's generator — Visual Studio on Windows, Ninja/Make on Linux)
cmake -S . -B build

# Build
cmake --build build -j 8
```

The executable and its runtime files are written to `bin/`.

## Tests

The test suite runs through CTest (build first, then):

```bash
ctest --test-dir build --output-on-failure
```

It registers four suites: `unit` (no window), `plugins` (PluginManager + real CoreCLR; the
.NET runtime is mandatory, so this suite **fails** — never skips — if it can't init the host),
`ui-smoke` (full window + imgui_test_engine), and
`ui-smoke-loc` (the UI suite re-run under a machine translation to catch dropped widget ids).
Run a single suite with `-R`, e.g. `ctest --test-dir build -R ui-smoke --output-on-failure`.

## Plugins

ofs-ng plugins are C# DLLs loaded into the app's .NET runtime — one class deriving from
`OfsPlugin`, with no C ABI or manual marshaling to deal with. The `managed/` directory holds
the plugin API, the host, and example plugins; **[managed/plugins/StarterPlugin](managed/plugins/StarterPlugin)**
is a ready-to-build template to copy from. The full **[C# API reference](https://ofs69.github.io/ofs-ng/)**
documents the `Ofs.Api` surface.

Plugins are managed .NET assemblies (platform-neutral IL). A compiled plugin is tied only to
the `Ofs.Api` version it was built against — not to a specific ofs-ng version — so the same
DLL keeps working across every ofs-ng build that ships a compatible `Ofs.Api`. A rebuild is
needed only when `Ofs.Api` itself changes incompatibly.

## Troubleshooting

### High CPU on NVIDIA GPUs (Windows)

Hint: setting **_Threaded Optimization_ to _Off_** for `ofs-ng.exe` in the NVIDIA Control Panel
(Manage 3D settings → Program Settings) may reduce CPU usage.

## License

ofs-ng is licensed under the **GNU General Public License v3.0** — see [LICENSE](LICENSE) for the
full text. As a complete rewrite of [OpenFunscripter](https://github.com/OpenFunscripter/OFS), it
continues under the same GPLv3 license.
