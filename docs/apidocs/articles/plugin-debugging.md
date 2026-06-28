# Debugging Plugins

A plugin runs **inside** the ofs-ng process, loaded into its .NET runtime — there is no separate
plugin process to launch. You debug it by **attaching a managed debugger to the running ofs-ng
process**. This page is the full reference; [Getting Started](getting-started.md) has the one-time
project/IDE setup (the template's `.vscode` tasks, `OfsDir`, build/install).

## Attach a debugger

1. Build a **Debug** build. The template's build auto-deploys the `.dll` **and `.pdb`** straight into
   the per-user plugins folder; the `.pdb` is what makes breakpoints bind. (Debug gives accurate
   stepping and all locals — see *Debug vs Release* in [Getting Started](getting-started.md).)
2. Launch ofs-ng normally and confirm the trust prompt on first load.
3. Attach to the `ofs-ng` process as **managed (.NET)** code:
   - **VS Code** — *Start Debugging* (`F5`) with the **"Attach to ofs-ng"** config the template ships
     in `.vscode/launch.json`, then pick the `ofs-ng` process.
   - **Visual Studio / Rider** — *Attach to Process…*, select `ofs-ng`, and choose the managed/.NET
     code type.
4. Set breakpoints in your plugin source. They bind once ofs-ng has loaded the plugin and turn solid;
   per-frame code ([`OnRenderUi`](xref:Ofs.OfsPlugin.OnRenderUi*),
   [`OnUpdate`](xref:Ofs.OfsPlugin.OnUpdate*)) and event/command handlers hit immediately.

## Breaking in `OnLoad`

Plugins load at ofs-ng startup, so the debugger usually isn't attached yet when
[`OnLoad`](xref:Ofs.OfsPlugin.OnLoad) runs — a breakpoint there is missed. To stop in `OnLoad`, drop a

```csharp
System.Diagnostics.Debugger.Break();
```

at the top of it: when a debugger is attached it breaks, and with none attached it's a no-op.

## Iterate with hot reload

After editing, rebuild — the build auto-deploys the new `.dll`/`.pdb`. Enable **Plugins → [plugin
name] → Hot reload (developer)** and ofs-ng picks up the rebuilt code without a restart. The reload
unloads and reconstructs the plugin, so [`OnLoad`](xref:Ofs.OfsPlugin.OnLoad) re-runs and your
commands/nodes/modes re-register (see [Plugin Loading & OnLoad](plugin-onload.md)). **Re-attach the
debugger after a reload** — the managed context unloads and reloads, which drops the debug connection.

> **Script nodes** hot-reload on their own (editor save, focus-gain, explicit Reload, or the per-node
> Watch toggle) — they aren't part of the plugin reload above. See [Script Nodes](script-nodes.md).

## Diagnostics without a debugger

Often the fastest signal is a log line or a toast — and both work from any thread.

- **Logging:** [`Host.Log(message)`](xref:Ofs.IOfsHost.Log*) (info) or
  [`Host.Log(level, message)`](xref:Ofs.IOfsHost.Log*) with a [`LogLevel`](xref:Ofs.LogLevel). Lines
  go to ofs-ng's log (console and log file), prefixed with your plugin. Safe to call from worker
  threads.
- **Toasts:** [`Host.Notify(level, message)`](xref:Ofs.IOfsHost.Notify*) (or the
  `NotifyInfo`/`NotifySuccess`/`NotifyWarning`/`NotifyError` shorthands) raises a user-facing toast,
  shown verbatim with your plugin's name. It is **not** throttled — don't call it every frame.

## When a plugin throws

The host **guards every plugin entry point** — `OnLoad` / `OnUpdate` / `OnRenderUi`, event and command
handlers, node eval and UI callbacks, and dialog callbacks. An exception that escapes one of these is
**caught, logged at error level (with the failing context), and surfaced as a throttled toast** — it
won't crash ofs-ng. So when a feature "does nothing," **check the log and the toast first**: a swallowed
exception is the usual cause.

A throwing **node eval** additionally degrades safely for that evaluation — a functional node passes its
input through, a discrete node emits nothing — rather than producing garbage. Fix the throw; the node
isn't permanently wrong, it just produced a neutral result while it was faulting.

## Script-node compile & header errors

A [script node](script-nodes.md) is compiled at runtime, so authoring mistakes surface as you edit:

- A **compile error** puts the node in an error state and writes the diagnostics to the log; fix the
  `.cs` and save to recompile (hot reload picks it up).
- A malformed **`// !ofs:` header directive** is dropped with a logged warning rather than failing
  silently — so a knob or pin that didn't appear has an explanation in the log.

## See also

- [Getting Started](getting-started.md) — IDE/project setup, build, install, and Debug vs Release.
- [Plugin Loading & OnLoad](plugin-onload.md) — the load/reload lifecycle a breakpoint sits in.
- [Script Nodes](script-nodes.md) — script hot-reload and the `// !ofs:` header.
