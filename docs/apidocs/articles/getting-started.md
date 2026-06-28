<!-- GENERATED from managed/plugins/StarterPlugin/README.md — do not edit. The Docs workflow
     regenerates this before every publish; this committed copy is the local-build fallback. -->
# ofs-ng Starter Plugin

A minimal, ready-to-build template for writing an [ofs-ng](https://github.com/ofs69/ofs-ng#readme) plugin in C#.
Copy this folder, rename it, and start coding. Building it packs the plugin into an installable
`dist/<name>.zip` that you load through ofs-ng's **Plugins → Install plugin from zip...** flow,
and also deploys it directly into ofs-ng's per-user plugins folder for faster iteration.

## Renaming the plugin

> **Do this before anything else.** The folder name, assembly name, and `.dll` must all match
> for ofs-ng to load the plugin — rename first, then build. Changing it later is possible but
> cumbersome.

ofs-ng loads `<folder>/<folder>.dll`, so the **folder name, `AssemblyName`, and `.dll` must
match**. To rename:

1. Rename the folder and `.csproj`, and update both `<AssemblyName>` and `<RootNamespace>` in
   the `.csproj` to match.
2. Change `Name => "..."` in `StarterPlugin.cs` (this is just the display name; it can be
   anything).

## Versioning your plugin

Your plugin carries its own version, separate from the `Ofs.Api` compatibility covered at the
end of this README. Set it once in `StarterPlugin.csproj` and bump it whenever you ship a
change:

```xml
<Version>1.0.0</Version>
```

ofs-ng reads this from the compiled assembly and shows it next to the plugin's name in the
plugin list. The `.csproj` is the only place to set it.

## Requirements

- **.NET 10 SDK** — <https://dotnet.microsoft.com/download>
- An **ofs-ng install** (the folder that contains the ofs-ng executable, plus `managed/` and
  `plugins/`)
- **VS Code** with the *C# Dev Kit* extension (this template ships ready-to-use `.vscode`
  build and debug tasks)

## Point the project at your ofs-ng install

The build (and IntelliSense) reads `Ofs.Api.dll` from `<ofs-ng>/managed`, where `<ofs-ng>` is
the folder holding the ofs-ng executable.

**Working inside the `ofs-ng` repo?** Nothing to do — it defaults to `../../bin`.

**Standalone copy? Set the path once in `StarterPlugin.csproj`.** Near the top, uncomment the
`<OfsDir>` line and point it at your install (forward slashes work on every OS):

```xml
<OfsDir>C:/Tools/ofs-ng</OfsDir>     <!-- Linux: /home/you/ofs-ng -->
```

After editing, run *.NET: Restart Language Server* in VS Code (or reopen the folder) so code
completion re-resolves against the new path.

## Build

### VS Code
Open this folder, then run **Terminal → Run Build Task** (`Ctrl+Shift+B`).

### Command line
```bash
dotnet build
# or, with an explicit ofs-ng path:
dotnet build -p:OfsDir="C:/Tools/ofs-ng"
```

Every successful build does two things:

**Packs** the plugin into **`dist/StarterPlugin.zip`** (next to the `.csproj`), already laid
out the way the installer expects:

```
StarterPlugin.zip
└── StarterPlugin/
    ├── StarterPlugin.dll
    ├── StarterPlugin.runtimeconfig.json
    ├── StarterPlugin.deps.json
    └── StarterPlugin.pdb        # symbols — keep for debugging, delete before sharing a Release
```

(A plugin that pulls in NuGet packages also gets those packages' assemblies here — and, for
packages with native libraries like Emgu.CV, the native `.dll`s and/or a `runtimes/` folder. See
*Adding libraries* below.)

**Deploys** a copy directly into ofs-ng's per-user plugins folder
(`%APPDATA%\ofs\ofs-ng\plugins\StarterPlugin\` on Windows,
`~/.local/share/ofs/ofs-ng/plugins/StarterPlugin/` on Linux). This is the fast dev path — no
manual install step after each rebuild. Opt out with `-p:DeployToPref=false` if you don't want
this.

## Install your plugin

**During development:** the build's auto-deploy puts the plugin directly in the right folder.
ofs-ng shows a one-time trust prompt the first time it sees those bytes; after that, enable
**Plugins → [plugin name] → Hot reload (developer)** and rebuilt code is picked up
automatically while ofs-ng is running.

**To share a build:** install through the app:

1. In ofs-ng, open **Plugins → Install plugin from zip...** and pick `dist/StarterPlugin.zip`.
2. Confirm the trust prompt. The plugin is extracted into ofs-ng's per-user plugins folder and
   loads immediately (no restart needed).

To remove a plugin, use **Plugins → [plugin name] → Uninstall...**.

## Adding libraries

Add NuGet packages the normal way — `dotnet add package <name>` or a `<PackageReference>` in the
`.csproj`. Their managed assemblies are copied next to your plugin, packed into the zip, and deployed
to the pref folder automatically.

Packages with **native** libraries (e.g. `Emgu.CV.runtime.windows`, which carries `cvextern.dll`)
need the build to be RID-specific, or the native files are silently dropped and the plugin fails at
its first call into the native library with a `DllNotFoundException`. This template sets
`RuntimeIdentifier` to the building SDK's RID for exactly that reason, so native deps work out of the
box — at the cost of pinning each build to one OS/arch (which a native dependency requires anyway).
By default this is the RID of the SDK you build with; override it to target another platform:

```bash
dotnet build -p:RuntimeIdentifier=win-x64      # 64-bit Windows
dotnet build -p:RuntimeIdentifier=linux-x64    # 64-bit Linux
dotnet build -p:RuntimeIdentifier=osx-arm64    # Apple Silicon macOS
```

The native NuGet package must, of course, actually ship binaries for the RID you pick (e.g.
`Emgu.CV.runtime.windows` only carries Windows natives — pair it with the matching `win-*` RID).

## Debug vs Release builds

`dotnet build` (and the VS Code build task) produces a **Debug** build: unoptimized, with
full symbols — what you want while developing. For something you hand to other users, build
**Release**:

```bash
dotnet build -c Release
```

`-c Release` only changes the configuration — ofs-ng is still located the same ways (your
tasks.json / `OFS_DIR` setting, the in-repo default, or an explicit `-p:OfsDir=...`). So if
you point at ofs-ng on the command line, pass both:
`dotnet build -c Release -p:OfsDir="C:/Tools/ofs-ng"`.

| | Debug (default) | Release (`-c Release`) |
|--|------------------|------------------------|
| Optimizations | off | on |
| Debugger experience | accurate stepping, all locals | optimized; stepping/locals may be approximate |
| Symbols (`.pdb`) | emitted & zipped | emitted & zipped |
| Use for | local development & debugging | sharing with users |

Both configurations write the **same** `dist/StarterPlugin.zip`, so a later Debug build
overwrites the Release zip (and vice-versa) — pass `-c Release` to rebuild in the configuration
you actually want before sharing.

> The `.pdb` inside the zip is what makes breakpoints work (see below). When distributing a
> Release build to end users you can delete it from the zip — they don't need it.

## Debugging your plugin

Your plugin runs *inside* the ofs-ng process, so you debug it by **attaching a managed debugger
to the running `ofs-ng` process** — build a **Debug** build (the auto-deploy puts the `.dll` and
`.pdb` in the pref folder), launch ofs-ng, then in VS Code *Start Debugging* (`F5`) with the
**"Attach to ofs-ng"** config and pick the `ofs-ng` process. Breakpoints bind once the plugin loads.

The full guide — breaking in `OnLoad`, hot-reload + re-attach, logging/notification diagnostics, and
reading swallowed plugin faults — is on the docs site:
**[Debugging Plugins](https://ofs69.github.io/ofs-ng/articles/plugin-debugging.html)**.

## What's in here

| File | Purpose |
|------|---------|
| `StarterPlugin.cs`   | The plugin — one class deriving from `OfsPlugin`. |
| `StarterPlugin.csproj` | References `Ofs.Api.dll`, packs an installable zip, and auto-deploys to the pref plugins folder on every build. |
| `.vscode/` | Build task and the attach-to-ofs-ng debug config. |

## Localization (optional)

For a single language, pass literal strings to `ui.Label`/`ui.Button`/etc. and skip this section —
ofs-ng does **not** translate plugin strings, so every label, section title, and your plugin `Name`
renders verbatim in whatever language you wrote it.

To support more than one language, reference strings through the strongly-typed `Str` accessor and ship
a `.resx` catalog. There is **no culture wiring to write**: `OfsPlugin` keeps `Str` pointed at ofs-ng's
active language (including live switches in Preferences).

```csharp
ui.Label(string.Format(Str.TimeFmt, Host.Player.Time));   // Str.TimeFmt = "Time: {0:F2}s"
if (ui.Button(Str.ClickMe)) { /* ... */ }
```

**Per-frame strings** — anything drawn in `OnRenderUi` or a node's `ui` callback — follow the language
automatically, because you re-read `Str.*` every frame.

**Registration-time strings** the host stores once — a **command title**, a **node display name**, your
plugin **`Name`** — just build from `Str.*` too:

- Command title / node name: pass the `Str.*` value, e.g.
  `Host.Commands.Register("id", Str.MyCommand, handler)` or
  `Host.Nodes.AddModifier<TState>("id", Str.MyNode, eval)`.
- Plugin `Name`: return `Str.*` from your `Name` property.

When the user switches language, the host **unloads and reloads every plugin**, so `OnLoad` re-runs and
re-registers all of these in the new language — no provider or live-update wiring needed.

**The catalog.** The `.resx` files live in `Localization/`: a neutral `Localization/Str.resx` (the
fallback — already included here) plus one `Localization/Str.<culture>.resx` per language. There's no
CLI to scaffold a `.resx`; create a language by copying the neutral file (it carries the required
schema) and translating the `<value>`s:

```bash
cp Localization/Str.resx Localization/Str.ja.resx      # then edit the <value>s
```

Each `Str.<culture>.resx` compiles to a satellite assembly (`<culture>/<name>.resources.dll`); the
pack/deploy steps glob recursively, so culture folders ship in the zip and pref folder automatically. A
key missing in the active language falls back to neutral `Str.resx`.

> **Caveat — the culture must exist as an ofs-ng language.** ofs-ng picks your `Str.<culture>.resx`
> by the **BCP 47 culture tag of its active UI language**, handed to the plugin as `Host.Culture`. That
> tag is the `[_meta].culture` field inside a `lang/<id>.toml` catalog — **not** the catalog's filename.
> So `Str.ja.resx` only ever loads when ofs-ng has a language declaring `culture = "ja"` *and the user
> selects it*; if no such language exists, `Host.Culture` never becomes `ja` and your plugin stays on
> neutral `Str.resx`. (The shipped Japanese catalog is `lang/ja_[AI].toml` — a `ja_[AI]` filename, but
> `culture = "ja"` inside — which is what pairs it with `Str.ja.resx`.) Match the `<culture>` suffix to
> the `[_meta].culture` tag of the ofs-ng language you're targeting — including script/region subtags
> like `zh-Hant` or `pt-BR`, which .NET resolves with fallback (`zh-Hant → zh → neutral`).

**`Str` comes from the resx.** Add a key to `Str.resx` and `Str.<Key>` is available on the next
build.

> The editor red-underlines `Str` (and any newly added key) until the build has generated the accessor.
> Build once; if the underlines linger, run *.NET: Restart Language Server* in VS Code (or reload the
> project in Visual Studio).

**Don't want localization?** Delete the `Localization/` folder and the `EmbeddedResource` block from the `.csproj`,
and pass literal strings to `ui.*` directly.

For the raw language signal, `Host.Culture` and `Host.Language` are also exposed (read them at `OnLoad`),
but most plugins never need them. For a larger worked example see `managed/plugins/Ofs.Core/` (`Str.resx`,
`Str.ja.resx`, and its `Str.*` getters).

## API version compatibility

A compiled plugin only loads on ofs-ng builds with a compatible `Ofs.Api`. If yours is
incompatible, ofs-ng skips it and logs:

```
[Ofs.PluginHost] Plugin built against incompatible Ofs.Api 2.0.0.0 (host provides 1.0.0.0). Rebuild the plugin against this ofs-ng build.
```

The fix: point `OfsDir` / `OFS_DIR` at the ofs-ng you're targeting and rebuild. The binding
is to the `Ofs.Api` version, not a specific ofs-ng version, so one compiled `.dll` works
across every ofs-ng build that ships a compatible `Ofs.Api`; rebuild only when `Ofs.Api`
itself changes incompatibly.
