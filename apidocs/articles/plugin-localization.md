# Plugin Localization

ofs-ng renders plugin-supplied strings **verbatim** — it does *not* translate them. Localizing a
plugin is the plugin's own job, and entirely **optional**: for a single language, pass literal strings
to `ui.Label` / `ui.Button` / a command title / your plugin [`Name`](xref:Ofs.OfsPlugin.Name) and skip
this page. To support more than one language, reference strings through a strongly-typed `Str` accessor
backed by a `.resx` catalog — and let `OfsPlugin` keep that accessor pointed at ofs-ng's active
language for you.

## The `Str` accessor, with zero culture wiring

You reach localized strings through a generated, compile-checked accessor — `Str.ClickMe` instead of a
`"ClickMe"` magic string. The build generates the `Str` class from your `Str.resx`; adding a key makes
`Str.<Key>` available on the next build.

The one thing you'd normally get wrong by hand — *which* language those getters resolve in — is wired
for you. A generated resx accessor keys off `CultureInfo.CurrentUICulture` (the OS UI culture, which has
nothing to do with ofs-ng's in-app language picker). The moment the host is set on your plugin (before
`OnLoad`), `OfsPlugin` redirects the accessor's static `Culture` to
[`Host.Culture`](xref:Ofs.IOfsHost.Culture) — so the getters follow the language the user picked in
ofs-ng, without touching the process-wide culture. There is **no provider to register and no live-update
code to write**.

> The accessor is found **by shape, not by name** — the host looks for the (one) type carrying both a
> static `ResourceManager` and a settable static `Culture`. So you may name the class anything (the
> `.csproj`'s `StronglyTypedClassName`); the convention used throughout ofs-ng is `Str`. A plugin that
> ships no such class is simply left alone.

## String lifetimes — all handled by one reload

There are two kinds of user-visible string in a plugin, and both follow the language correctly for the
same underlying reason.

**Per-frame strings** — anything you draw each frame in [`OnRenderUi`](xref:Ofs.OfsPlugin.OnRenderUi*)
or a node's `ui` callback — follow the language automatically, because you re-read `Str.*` every frame:

```csharp
protected override void OnRenderUi(Ui ui)
{
    ui.Label(string.Format(Str.TimeFmt, Host.Player.Time)); // Str.TimeFmt = "Time: {0:F2}s"
    if (ui.Button(Str.ClickMe)) { /* … */ }
}
```

**Registration-time strings** the host stores once — a **command title**, a **node display name**, your
plugin **[`Name`](xref:Ofs.OfsPlugin.Name)** — would seem stuck in whatever language was active at load.
They aren't, because **on a UI-language switch the host unloads and reloads every plugin**, so
[`OnLoad`](xref:Ofs.OfsPlugin.OnLoad) re-runs and re-registers everything in the new language. Just
build these from `Str.*` too:

```csharp
public override string Name => Str.PluginName;

protected override void OnLoad()
{
    Host.Commands.Register("greet", Str.GreetCommand, () => Host.NotifyInfo(Str.Greeting));
    Host.Nodes.AddNode<GainState>("gain", Str.GainNode, shape, GainEval);
}
```

The same reload is why you should **read [`Host.Language`](xref:Ofs.IOfsHost.Language) /
[`Host.Culture`](xref:Ofs.IOfsHost.Culture) at `OnLoad`**, not cache them from an earlier run — `OnLoad`
always runs in the current language. (See [Plugin Loading & OnLoad](plugin-onload.md) for the full
reload story.)

## The catalog

Ship a **neutral `Str.resx`** (the fallback) plus one **`Str.<culture>.resx` per language**. There's no
CLI to scaffold one — copy the neutral file (it carries the required schema) and translate the
`<value>`s:

```bash
cp Str.resx Str.ja.resx      # then edit the <value>s
```

Each `Str.<culture>.resx` compiles to a satellite assembly (`<culture>/<name>.resources.dll`); the
StarterPlugin pack/deploy steps glob recursively, so culture folders ship in the zip and the pref
folder automatically. A key missing in the active language falls back to the neutral `Str.resx`.

## ⚠️ The culture must exist as an ofs-ng language

This is the one rule that trips people up. ofs-ng selects your `Str.<culture>.resx` by the **ISO 639
code of its active UI language**, surfaced to the plugin as [`Host.Culture`](xref:Ofs.IOfsHost.Culture).
That code is the **`[_meta].iso639` field inside a `lang/<id>.toml` catalog — not the catalog's
filename**.

So `Str.ja.resx` loads only when **both**:

1. ofs-ng has a language whose `iso639 = "ja"`, **and**
2. the user has selected it.

If no such language exists, [`Host.Culture`](xref:Ofs.IOfsHost.Culture) never becomes `ja` and your
plugin stays on the neutral `Str.resx`. ofs-ng's built-in English (and any unknown code) maps to the
**invariant** culture — i.e. your neutral catalog.

> The shipped Japanese catalog is `lang/ja_[AI].toml` — a `ja_[AI]` *filename*, but `iso639 = "ja"`
> *inside* — which is what pairs it with `Str.ja.resx`. **Match the `<culture>` suffix of your `.resx`
> to the `iso639` code of the ofs-ng language you're targeting**, not to any filename.

## Build wiring (`.csproj`)

Strongly-typed generation is a small `EmbeddedResource` block. The StarterPlugin ships it ready to go;
the essential part:

```xml
<ItemGroup>
  <EmbeddedResource Update="Str.resx">
    <Generator>MSBuild:Compile</Generator>
    <StronglyTypedLanguage>CSharp</StronglyTypedLanguage>
    <StronglyTypedNamespace>YourPlugin</StronglyTypedNamespace>
    <StronglyTypedClassName>Str</StronglyTypedClassName>
    <StronglyTypedFileName>$(MSBuildProjectDirectory)/Generated/Str.Designer.cs</StronglyTypedFileName>
  </EmbeddedResource>
</ItemGroup>
```

Declaring the neutral language in the `.csproj` (`<NeutralLanguage>en</NeutralLanguage>`) emits
`[assembly: NeutralResourcesLanguage]`, so the resource manager skips a satellite probe for English.

> The editor red-underlines `Str` (and any newly added key) until the build has generated the accessor.
> **Build once**; if the underlines linger, run *.NET: Restart Language Server* in VS Code (or reload
> the project in Visual Studio).

**Don't want localization?** Delete `Str.resx` and the `EmbeddedResource` block, and pass literal
strings to `ui.*` directly.

## Rolling your own (no `Str` accessor)

If you manage resources yourself, feed [`Host.Culture`](xref:Ofs.IOfsHost.Culture) to your own lookups
so they follow ofs-ng's picker rather than the OS culture:

```csharp
string label = _resources.GetString("ClickMe", Host.Culture) ?? "Click me";
```

[`Host.Language`](xref:Ofs.IOfsHost.Language) gives the raw ISO 639 code ("en" for built-in English)
if you need the string form. Read either at `OnLoad`.

## See also

- [Getting Started](getting-started.md) — the StarterPlugin's `.resx` setup, pack/deploy, and a
  condensed version of this section.
- [Plugin Loading & OnLoad](plugin-onload.md) — why `OnLoad` re-runs on a language switch.
- [`IOfsHost.Culture`](xref:Ofs.IOfsHost.Culture) / [`IOfsHost.Language`](xref:Ofs.IOfsHost.Language) —
  the active-language signal.
- `plugins/Ofs.Core/` — a larger worked example (`Str.resx`, `Str.ja.resx`, and its `Str.*` getters).
