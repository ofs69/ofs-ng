# ofs-ng Plugin API

Reference documentation for authoring **ofs-ng plugins** and **script nodes** against the
`Ofs.Api` assembly. Every public type, method, and member on this page is generated from the
shipping assembly, so it always matches the version you build against.

[!INCLUDE [api-version](includes/_apiversion.md)]

> Browse the full surface under **[API Reference](api/index.md)**. The `Ofs` namespace is the entry point.

## Start from the StarterPlugin template

The quickest way in is the **[StarterPlugin template](https://github.com/ofs69/ofs-ng/tree/main/managed/plugins/StarterPlugin)** —
a complete, buildable plugin you copy and rename. It already wires up the things you'd otherwise
get wrong by hand:

- the single `Ofs.Api` reference and how to point the build at your ofs-ng install (`OfsDir`),
- packaging into an installable `dist/<name>.zip` and an optional dev-deploy straight into your
  plugins folder,
- optional strongly-typed localization (`Str.resx`) that follows the in-app language.

The full walkthrough is here on the site:

- **[Getting Started](articles/getting-started.md)** — rename, requirements, build, install, debugging,
  libraries, and localization.
- **[Project File](articles/project-file.md)** — the annotated `StarterPlugin.csproj` and what every
  part of it does.

The plugin itself is small:

```csharp
using Ofs;

public sealed class StarterPlugin : OfsPlugin
{
    // Shown in the plugin list and window title. The host re-pulls Name on a language switch.
    public override string Name => "Starter";

    private int _clicks;

    protected override void OnLoad()
    {
        // Register commands, nodes, edit modes, and event subscriptions through Host.
        Host.Log($"{Name} loaded.");
    }

    protected override void OnRenderUi(Ui ui)
    {
        // Drawn every frame on the main thread. Omit this override for a plugin with no window.
        ui.Label($"Time: {Host.Player.Time:0.000}");

        if (ui.Button("Click me"))
            _clicks++;

        ui.Label($"Clicks: {_clicks}");
    }
}
```

## Where to go next

- **`OfsPlugin`** — the base class and its lifecycle (`OnLoad`, `OnUnload`, `OnRenderUi`, `OnUpdate`).
- **[Plugin Loading & OnLoad](articles/plugin-onload.md)** — what you register at load (and only at
  load), what re-runs on a language switch, and how to keep startup quick.
- **[Debugging Plugins](articles/plugin-debugging.md)** — attach a managed debugger, break in `OnLoad`,
  hot-reload, and read swallowed faults from the log.
- **`IOfsHost`** — the host surface: logging, the project, events, and the registries below.
- **[Script Nodes](articles/script-nodes.md)** — the lightweight node path: a `.cs` fragment with a
  `// !ofs:` header, no DLL. Start here for a formula in the processing graph.
- **Nodes** — the plugin-side node API for richer processing-graph nodes (generators, modifiers,
  combiners) with custom UI and typed state.
- **[Node Best Practices](articles/node-best-practices.md)** — the worker-thread contract every node
  body must respect: `static` eval, no captures, cancellation, and discrete-vs-functional.
- **[Plugin Localization](articles/plugin-localization.md)** — ship a `.resx` catalog and let `Str.*`
  follow ofs-ng's active language, with no culture wiring.
- **`Ui`** — the immediate-mode UI surface for node bodies and plugin windows.
- **Editing / Selection / Navigation** — register alternate edit modes, selection modes, and navigators.
- **Commands** — register invocable commands (palette and/or bindable shortcuts).

## Versioning

Plugins are compatible with a host when the **major** version of the `Ofs.Api` they were built
against matches the host's, and the plugin was not built against a newer `Ofs.Api`. The version this
reference documents is shown at the top of this page.
