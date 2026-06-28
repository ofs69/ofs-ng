using System;

namespace Ofs
{
    /// <summary>
    /// Fault isolation at every native→managed entry point. A plugin exception must never cross back
    /// into native — whether through a reverse-P/Invoke thunk (the <see cref="PluginApi"/> callbacks)
    /// or a <c>[UnmanagedCallersOnly]</c> node trampoline. Either is an instant, unrecoverable process
    /// crash. Every guarded call logs the exception with full detail and returns a safe default, and
    /// also raises a (host-throttled) user-facing notification so a misbehaving plugin is visible.
    /// </summary>
    internal static class PluginGuard
    {
        // Sinks are installed by the OfsHost ctor. They are static because the node trampolines
        // are static and have no host instance to reach. Ofs.Api is loaded once into the shared host
        // ALC, so these are process-wide; the last host constructed wins. That is fine: the native
        // log/notify they forward to derive the offending plugin name from the host call context.
        internal static Action<string, string>? LogSink;  // (ctx, fullDetail) -> ofs.log
        internal static Action<string>? FaultSink;        // (faultCtx)        -> throttled fault toast

        // Identifies the host whose sinks are currently installed. Both sinks capture that OfsHost, which
        // transitively owns the plugin's command/shortcut/event delegates (and thus the plugin's whole
        // AssemblyLoadContext). If these static fields outlived the plugin, they would root its ALC and it
        // would never unload — so ClearSinks nulls them on unload, guarded by owner identity so a still
        // newer plugin's sinks are never clobbered.
        private static object? _sinkOwner;

        internal static void InstallSinks(object owner, Action<string, string> log, Action<string> fault)
        {
            _sinkOwner = owner;
            LogSink = log;
            FaultSink = fault;
        }

        // Drop the sinks if (and only if) this owner still holds them, so the unloading plugin's host
        // stops rooting its ALC. A plugin loaded after this one already replaced the owner — leave it be.
        internal static void ClearSinks(object owner)
        {
            if (!ReferenceEquals(_sinkOwner, owner)) return;
            _sinkOwner = null;
            LogSink = null;
            FaultSink = null;
        }

        // Reports a swallowed exception. Used both by Run(...) and inline in the hot-path trampolines.
        internal static void Report(string ctx, Exception ex)
        {
            // Logging/notifying must never throw across the boundary we are protecting.
            try { LogSink?.Invoke(ctx, $"unhandled exception in {ctx}: {ex}"); } catch { }
            try { FaultSink?.Invoke(ctx); } catch { }
        }

        internal static void Run(string ctx, Action body)
        {
            try { body(); }
            catch (Exception ex) { Report(ctx, ex); }
        }

        internal static T Run<T>(string ctx, Func<T> body, T fallback)
        {
            try { return body(); }
            catch (Exception ex) { Report(ctx, ex); return fallback; }
        }
    }
}
