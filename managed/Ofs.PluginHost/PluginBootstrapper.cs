using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Linq;
using Ofs;

namespace Ofs.PluginHost
{
    public class PluginLoadContext : AssemblyLoadContext
    {
        private AssemblyDependencyResolver _resolver;

        public PluginLoadContext(string pluginPath) : base(isCollectible: true)
        {
            _resolver = new AssemblyDependencyResolver(pluginPath);
        }

        protected override Assembly? Load(AssemblyName assemblyName)
        {
            // Do not load the API assembly into the plugin context.
            if (assemblyName.Name == "Ofs.Api")
            {
                var apiAlc = AssemblyLoadContext.GetLoadContext(typeof(OfsPlugin).Assembly);
                var loaded = apiAlc?.Assemblies.FirstOrDefault(a => a.GetName().Name == "Ofs.Api");
                if (loaded != null) return loaded;
                return null;
            }

            string? assemblyPath = _resolver.ResolveAssemblyToPath(assemblyName);
            if (assemblyPath != null)
                return LoadFromAssemblyPath(assemblyPath);

            return null;
        }

        protected override IntPtr LoadUnmanagedDll(string unmanagedDllName)
        {
            string? libraryPath = _resolver.ResolveUnmanagedDllToPath(unmanagedDllName);
            if (libraryPath != null)
                return LoadUnmanagedDllFromPath(libraryPath);

            return IntPtr.Zero;
        }

        // Load the plugin's entry assembly from bytes rather than via LoadFromAssemblyPath, so the DLL (and
        // its PDB) on disk are NOT locked while loaded. That is what lets a developer overwrite them — a
        // rebuild, or the build's deploy into <pref>/plugins — while ofs-ng is running, which the host's
        // hot-reload depends on. The PDB is loaded alongside when present so breakpoints still bind.
        public Assembly LoadMainAssembly(string assemblyPath)
        {
            byte[] asmBytes = System.IO.File.ReadAllBytes(assemblyPath);
            using var asmStream = new System.IO.MemoryStream(asmBytes);
            string pdbPath = System.IO.Path.ChangeExtension(assemblyPath, ".pdb");
            if (System.IO.File.Exists(pdbPath))
            {
                byte[] pdbBytes = System.IO.File.ReadAllBytes(pdbPath);
                using var pdbStream = new System.IO.MemoryStream(pdbBytes);
                return LoadFromStream(asmStream, pdbStream);
            }
            return LoadFromStream(asmStream);
        }
    }

    public static class PluginBootstrapper
    {
        private static readonly object _lock = new object();
        private static List<(PluginLoadContext Context, PluginBridge Bridge, string DllStem)> _loadedPlugins = new();

        [UnmanagedCallersOnly]
        public static unsafe int LoadPluginNative(IntPtr pathPtr, IntPtr apiPtr, IntPtr hostApiPtr)
        {
            PluginLoadContext? context = null;
            try
            {
                // Managed-side ABI layout self-test (mirror of the static_asserts in PluginApi.h). Runs
                // once; a size/offset drift in PluginAbi.cs throws here with a precise message rather than
                // corrupting memory the first time a struct crosses the seam. The `plugins` ctest exercises
                // this on every run.
                AbiLayout.Verify();

                string? path = Marshal.PtrToStringUTF8(pathPtr); // host sends UTF-8 path bytes
                if (string.IsNullOrEmpty(path)) return -1;

                context = new PluginLoadContext(path);
                var assembly = context.LoadMainAssembly(path); // from bytes — leaves the DLL unlocked for hot-reload

                // ── Plugin compatibility guard (Ofs.Api assembly version) ──────────
                // A plugin binds against the HOST's Ofs.Api at load time (see
                // PluginLoadContext.Load — the plugin's own copy is discarded), so a
                // plugin built against an incompatible Ofs.Api would otherwise fail late,
                // deep in execution, with a MissingMethodException / TypeLoadException.
                // The version it was *compiled* against survives in metadata as the
                // Ofs.Api assembly reference, independent of the runtime binding. Compare
                // it to the host's Ofs.Api up front (before GetTypes() forces type load)
                // and reject mismatches cleanly.
                //
                // This is distinct from the native↔managed ABI check (OFS_ABI_VERSION /
                // ApiVersions.AbiVersion), which validates struct layout within one build.
                //
                // Policy: MAJOR must match (breaking changes bump major) and the plugin
                // must not be built against a NEWER Ofs.Api than the host (it could call
                // APIs the host lacks). Same major + older-or-equal = compatible, since
                // additive changes are backward compatible.
                var hostApiVer = typeof(OfsPlugin).Assembly.GetName().Version;
                var pluginApiVer = assembly.GetReferencedAssemblies()
                    .FirstOrDefault(a => a.Name == "Ofs.Api")?.Version;
                if (pluginApiVer == null)
                {
                    Console.WriteLine("[Ofs.PluginHost] Plugin does not reference Ofs.Api; refusing to load.");
                    context.Unload();
                    return -8;
                }
                if (hostApiVer == null || pluginApiVer.Major != hostApiVer.Major || pluginApiVer > hostApiVer)
                {
                    Console.WriteLine($"[Ofs.PluginHost] Plugin built against incompatible Ofs.Api {pluginApiVer} (host provides {hostApiVer}). Rebuild the plugin against this ofs-ng build.");
                    context.Unload();
                    return -8;
                }

                var pluginType = assembly.GetTypes()
                    .FirstOrDefault(t => t.IsClass && !t.IsAbstract && t.IsSubclassOf(typeof(OfsPlugin)));

                if (pluginType == null)
                {
                    context.Unload();
                    return -2;
                }

                if (Activator.CreateInstance(pluginType) is not OfsPlugin plugin)
                {
                    context.Unload();
                    return -3;
                }

                if (hostApiPtr == IntPtr.Zero)
                {
                    context.Unload();
                    return -6;
                }
                var hostApiStruct = (HostApi*)hostApiPtr;
                // Native↔managed ABI check: the native HostApi struct layout must match
                // this Ofs.Api's mirror. (Plugin compatibility was checked above.)
                if (hostApiStruct->Version > ApiVersions.AbiVersion)
                {
                    Console.WriteLine($"[Ofs.PluginHost] HostApi ABI version too new: expected <= {ApiVersions.AbiVersion}, got {hostApiStruct->Version}");
                    context.Unload();
                    return -7;
                }

                var bridge = PluginBridge.Load(plugin, hostApiStruct, apiPtr);

                string stem = System.IO.Path.GetFileNameWithoutExtension(path);
                lock (_lock)
                {
                    _loadedPlugins.Add((context, bridge, stem));
                }

                return 0;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[Ofs.PluginHost] Error loading plugin: {ex.Message}");
                context?.Unload(); // a partially-loaded collectible context would otherwise leak for the run
                return -4;
            }
        }

        // ── Plugin-node TState capture/release ──
        // Fetched by the native PluginManager (like LoadPluginNative) and invoked on the MAIN thread at
        // snapshot build / eval completion. They forward to Ofs.Api's shared Nodes tables — Ofs.Api is the
        // same instance the eval trampolines read, so a captured TState is visible to the worker by handle.

        // Decode a node's state JSON into a TState capture; returns a handle (≥0) for OfsEvalCtx.stateHandle,
        // or -1 on failure/empty. `generation` groups one eval's captures for ReleaseNodeStatesNative.
        [UnmanagedCallersOnly]
        public static unsafe int CaptureNodeStateNative(int slot, IntPtr jsonPtr, int generation)
        {
            try
            {
                string json = jsonPtr == IntPtr.Zero ? string.Empty : (Marshal.PtrToStringUTF8(jsonPtr) ?? string.Empty);
                return Nodes.CaptureState(slot, json, generation);
            }
            catch
            {
                return -1; // never let an exception cross back into native
            }
        }

        // Drop every capture made under `generation`.
        [UnmanagedCallersOnly]
        public static void ReleaseNodeStatesNative(int generation)
        {
            try { Nodes.ReleaseStates(generation); }
            catch { /* never throw across the native boundary */ }
        }

        // Purge all deferred Node.Update queues. Called on project load: stale mutations keyed to a reused
        // node id must not leak or apply to the freshly loaded project's nodes.
        [UnmanagedCallersOnly]
        public static void ClearNodeUpdatesNative()
        {
            try { Nodes.ClearPendingNodeUpdates(); }
            catch { /* never throw across the native boundary */ }
        }

        // Current managed heap size in bytes (the footer renders it). forceFullCollection: false ⇒ this just
        // reads the GC's running total, it does NOT trigger a collection, so it is cheap to poll per frame.
        [UnmanagedCallersOnly]
        public static long GetManagedHeapBytesNative()
        {
            try { return GC.GetTotalMemory(false); }
            catch { return -1; } // never throw across the native boundary
        }

        // Drop the plugin from the registry and request its (collectible) ALC unload. Kept in its own
        // non-inlined method so no local in UnloadPluginNative keeps the context alive across the GC
        // loop below. Returns the ALC as a WeakReference so the caller can wait for actual collection.
        [MethodImpl(MethodImplOptions.NoInlining)]
        private static bool RemoveAndUnload(string stem, out WeakReference? ctxRef)
        {
            ctxRef = null;
            PluginLoadContext ctx;
            PluginBridge bridge;
            lock (_lock)
            {
                int idx = _loadedPlugins.FindIndex(p => p.DllStem == stem);
                if (idx < 0) return false;
                ctx = _loadedPlugins[idx].Context;
                bridge = _loadedPlugins[idx].Bridge;
                _loadedPlugins.RemoveAt(idx);
            }
            // Drop the static references (node slots, pinned name, callback delegates) that would
            // otherwise keep the plugin assembly alive — without this the ALC never collects.
            bridge.Dispose();
            ctxRef = new WeakReference(ctx);
            ctx.Unload();
            return true;
        }

        // finalShutdown != 0 ⇒ the whole app is closing (not a runtime disable/uninstall).
        [UnmanagedCallersOnly]
        public static unsafe int UnloadPluginNative(IntPtr stemPtr, int finalShutdown)
        {
            try
            {
                string? stem = Marshal.PtrToStringUTF8(stemPtr); // host sends UTF-8 (matches LoadPluginNative)
                if (string.IsNullOrEmpty(stem)) return -1;

                if (!RemoveAndUnload(stem, out var ctxRef))
                {
                    Console.WriteLine($"[Ofs.PluginHost] Plugin not found for unload: {stem}");
                    return -2;
                }

                // App shutdown: the process is about to exit, so the ALC's lock on the plugin DLL is moot.
                // RemoveAndUnload already ran the cooperative teardown (cancel UnloadToken, drop host-held
                // delegates, request ALC unload); forcing collection and reporting non-collectibility here
                // would be pointless work and noise. Skip both.
                if (finalShutdown != 0)
                    return 0;

                // Runtime disable/uninstall: the host wants the DLL freed now so the folder can be
                // overwritten/deleted. ALC.Unload() only *requests* it; the lock is released once the GC
                // collects the context, so force collection until it is gone.
                for (int i = 0; ctxRef!.IsAlive && i < 10; i++)
                {
                    GC.Collect();
                    GC.WaitForPendingFinalizers();
                }
                if (ctxRef.IsAlive)
                {
                    // Still rooted after forced collection. With Ofs.Api shared in the host's non-collectible
                    // context, this is usually a process-global framework cache (System.Text.Json, Regex, …)
                    // holding one of the plugin's types — something a plugin generally can't avoid, so it is
                    // not a plugin error. It is also not fatal: the host's deferred-uninstall finishes the
                    // removal on the next launch, when the file is free. Informational only — no error log,
                    // no user toast.
                    Console.WriteLine($"[Ofs.PluginHost] Plugin '{stem}' could not be released immediately; " +
                                      "its files will be cleaned up on the next launch.");
                }
                return 0;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[Ofs.PluginHost] Error unloading plugin: {ex.Message}");
                return -3;
            }
        }
    }
}
