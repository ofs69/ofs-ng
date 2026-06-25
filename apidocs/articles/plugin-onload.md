# Plugin Loading & `OnLoad`

[`OnLoad`](xref:Ofs.OfsPlugin.OnLoad) is where a plugin wires itself into the host. It runs **once**,
on the main thread, right after the plugin is constructed and [`Host`](xref:Ofs.OfsPlugin.Host) is
available. Everything a plugin *contributes* to the host — commands, nodes, edit/select/navigate
modes — is registered here, and **only** here: those calls throw if made later.

## The lifecycle, in order

1. The host constructs your `OfsPlugin` subclass.
2. The host wires [`Host`](xref:Ofs.OfsPlugin.Host) (and your localized `Str` catalog, if any).
3. **`OnLoad()` runs** — register here.
4. Per frame, for the plugin's life: [`OnUpdate(dt)`](xref:Ofs.OfsPlugin.OnUpdate*) then
   [`OnRenderUi(ui)`](xref:Ofs.OfsPlugin.OnRenderUi*) (the latter only if you override it).
5. On disable / app shutdown / language switch: [`OnUnload()`](xref:Ofs.OfsPlugin.OnUnload) runs, then
   the [`UnloadToken`](xref:Ofs.IOfsHost.UnloadToken) is cancelled.

`Host` is `null` before step 2 — never touch it from a constructor or field initializer. Do that work
in `OnLoad`.

## What can *only* be done in `OnLoad`

The host's registration surfaces latch a "during OnLoad" flag and reject calls made outside it (the
message is *"…must be called from OnLoad, not at runtime"*). Register everything up front:

| API | Registers |
|-----|-----------|
| [`Host.Commands.Register`](xref:Ofs.Commands.Register*) | a palette / bindable command |
| [`Host.Nodes.AddNode`](xref:Ofs.Nodes.AddNode*) | a processing-graph node (generator / modifier / combiner) |
| [`Host.Editing.RegisterMode`](xref:Ofs.Editing.RegisterMode*) | an alternate timeline edit mode |
| [`Host.Navigation.RegisterMode`](xref:Ofs.Navigation.RegisterMode*) | an alternate navigator (how stepping moves the playhead) |
| [`Host.Selection.RegisterMode`](xref:Ofs.Selection.RegisterMode*) | an alternate selection mode |

Why the restriction: the host builds its command table, node palette, and mode selectors once and
treats them as fixed for the session. Allowing mid-session registration would mean racing those tables
against the UI and the evaluation threads that read them. So the contract is simple — **declare your
full surface at load.** A registration is dropped automatically on unload; you never unregister by
hand.

```csharp
protected override void OnLoad()
{
    Host.Commands.Register("greet", "Say hello", () => Host.NotifyInfo("Hello!"));

    Host.Nodes.AddNode<GainState>("gain", "Gain",
        new NodeShape(inputs: ["in"], outputs: ["out"]),
        static (double t, ReadOnlySpan<float> ins, in GainState s, NodeContext ctx, Span<float> outs)
            => outs[0] = ins[0] * s.Gain,
        ui: static (Ui ui, ref GainState s) => ui.DragFloat("Gain", ref s.Gain, 0.01f, 0f, 4f));

    Host.Editing.RegisterMode("ripple", "Ripple", OnRippleIntent);
}
```

> Calling any of these from `OnUpdate`, `OnRenderUi`, an event handler, or a command handler throws.
> If you find yourself wanting to register conditionally at runtime, register everything at load and
> gate the *behavior* instead (e.g. a command handler that no-ops when disabled).

## What belongs in `OnLoad` (but isn't enforced)

These aren't gated, yet `OnLoad` is the right place — they're set-up-once concerns, and they re-run
correctly across the host's language-switch reload (below).

- **Event subscriptions.** Subscribe to the player and axes here:
  [`Host.Player.TimeChanged`](xref:Ofs.Player.TimeChanged),
  [`Host.Player.PlayingChanged`](xref:Ofs.Player.PlayingChanged),
  [`Host.Player.MediaChanged`](xref:Ofs.Player.MediaChanged),
  [`Host.Axes.Modified`](xref:Ofs.Axes.Modified),
  [`Host.Axes.ProjectChanged`](xref:Ofs.Axes.ProjectChanged). You don't unsubscribe in `OnUnload` —
  the host drops every plugin handler on teardown (they capture the plugin instance and would
  otherwise keep its assembly loaded).
- **Persisted settings.** Create [`Host.AppScoped<T>`](xref:Ofs.IOfsHost.AppScoped*) (global) and
  [`Host.Project.Scoped<T>`](xref:Ofs.Project.Scoped*) (per-project) handles once and keep them in
  fields. The host flushes changes every frame and on close — there's nothing manual to call.
- **Reading the active language.** [`Host.Language`](xref:Ofs.IOfsHost.Language) /
  [`Host.Culture`](xref:Ofs.IOfsHost.Culture) — read at load (see below).

```csharp
private AppScoped<Settings> _settings = null!;

protected override void OnLoad()
{
    _settings = Host.AppScoped<Settings>("settings");
    Host.Player.MediaChanged += path => Host.Log($"Now editing {path}");
    Host.Axes.Modified += axis => _dirty = true;
}
```

## `OnLoad` re-runs on a language switch

When the user changes ofs-ng's UI language, the host **unloads and reloads every plugin** — so
`OnLoad` runs again in the new language. This is the mechanism that lets registration-time strings (a
command title, a node display name, your plugin [`Name`](xref:Ofs.OfsPlugin.Name)) follow the
language: build them from your `Str` catalog and they re-register translated, with no live-update
wiring. The corollary: **read [`Host.Language`](xref:Ofs.IOfsHost.Language) /
[`Host.Culture`](xref:Ofs.IOfsHost.Culture) at `OnLoad`**, not cached from an earlier run.

## Keep `OnLoad` quick

Plugins load during ofs-ng startup, so `OnLoad` is on the critical path to the first frame. Register,
subscribe, and create handles — then return. Push any heavy work (scanning a folder, warming a model,
network I/O) onto a background task and observe [`UnloadToken`](xref:Ofs.IOfsHost.UnloadToken) so it
stops cleanly:

```csharp
protected override void OnLoad()
{
    _ = Task.Run(async () =>
    {
        await WarmUpAsync(Host.UnloadToken);   // honors cancellation
        Host.RunOnMainThread(() => _ready = true); // marshal results back
    });
}
```

Don't assume a project or media is loaded in `OnLoad` — a plugin may load before any project is open.
React to [`Host.Player.MediaChanged`](xref:Ofs.Player.MediaChanged) /
[`Host.Axes.ProjectChanged`](xref:Ofs.Axes.ProjectChanged) instead of reading state at load.

## See also

- [`OfsPlugin`](xref:Ofs.OfsPlugin) — the base class and its lifecycle hooks.
- [`IOfsHost`](xref:Ofs.IOfsHost) — the full host surface registered against here.
- [Node Best Practices](node-best-practices.md) — the rules a node registered in `OnLoad` must follow
  once it runs.
- [Debugging Plugins](plugin-debugging.md) — including how to break in `OnLoad`.
