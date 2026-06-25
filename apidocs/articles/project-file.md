# The plugin project file

A plugin is an ordinary .NET class library with a few required properties and some build targets
that package and deploy it. The annotated `StarterPlugin.csproj` below is the canonical reference —
copy it and rename. The key parts:

- **`EnableDynamicLoading`** — required, so ofs-ng can load the assembly into its own collectible
  context.
- **`AssemblyName` / folder / `.dll`** — must all match; ofs-ng loads `<folder>/<folder>.dll`.
- **`Version`** — your plugin's own version, shown next to its name in the plugin list.
- **`OfsDir`** — where your ofs-ng install lives; the build reads `Ofs.Api.dll` from `<OfsDir>/managed`.
  Resolution order: `-p:OfsDir=…` → `OFS_DIR` env var → the in-repo default `../../bin`.
- **`RuntimeIdentifier`** — defaults to the building SDK's RID so packages with **native** libraries
  ship their native assets; pin it (`-p:RuntimeIdentifier=win-x64`) to target another platform.
- **Localization `EmbeddedResource`** — optional strongly-typed `Str` accessor generated from
  `Str.resx`; delete it if you don't localize.
- **`PackPluginZip` / `DeployToPrefPlugins`** — after each build, pack `dist/<name>.zip` and copy the
  plugin straight into your per-user plugins folder for fast iteration (`-p:DeployToPref=false` to opt
  out). `Ofs.Api` is excluded from both — ofs-ng owns the one canonical copy it loads everyone against.

[!code-xml[StarterPlugin.csproj](../../plugins/StarterPlugin/StarterPlugin.csproj)]
