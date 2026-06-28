using System;
using System.Buffers;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace Ofs
{
    /// <summary>
    /// The host surface available to a plugin, organized into cohesive sub-objects.
    /// </summary>
    public interface IOfsHost
    {
        /// <summary>Video playback: time, duration, transport, and playback events.</summary>
        Player Player { get; }
        /// <summary>Per-axis read access and edits, plus axis/project change events.</summary>
        Axes Axes { get; }
        /// <summary>Register invocable commands (palette and/or bindable shortcuts).</summary>
        Commands Commands { get; }
        /// <summary>Register processing-graph nodes (generators, modifiers, combiners).</summary>
        Nodes Nodes { get; }
        /// <summary>Native file/folder pickers and confirmation prompts.</summary>
        Dialogs Dialogs { get; }
        /// <summary>The open project: chapters, bookmarks, regions, and project metadata.</summary>
        Project Project { get; }
        /// <summary>Register alternate edit modes for the timeline.</summary>
        Editing Editing { get; }
        /// <summary>Register alternate navigators (how stepping moves the playhead).</summary>
        Navigation Navigation { get; }
        /// <summary>Register alternate selection modes.</summary>
        Selection Selection { get; }

        /// <summary>
        /// An <see cref="AppScoped{T}"/> handle over this plugin's global settings at
        /// <paramref name="key"/>: it loads the value now and saves it back whenever it changes —
        /// persisted in this plugin's own file under the app's settings directory, shared across all
        /// projects (unlike <see cref="Project.Scoped{T}(string)"/>, which is per-project). Create it once
        /// in <see cref="OfsPlugin.OnLoad"/>; the host flushes edits once per frame and again on app close,
        /// so there is nothing manual to call. Main-thread only.
        /// </summary>
        AppScoped<T> AppScoped<T>(string key) where T : new();

        /// <summary>The BCP 47 culture tag of ofs-ng's active UI language ("en" for built-in English, else
        /// the tag the translation declares, e.g. "ja", "zh-Hant"). Main-thread only.</summary>
        string Language { get; }

        /// <summary>ofs-ng's active UI language as a <see cref="CultureInfo"/> — feed it to your own
        /// <c>ResourceManager.GetString(key, Host.Culture)</c> so the plugin follows the language the
        /// user picked in ofs-ng (not the OS culture). English/unknown maps to the invariant culture
        /// (i.e. your neutral <c>.resx</c>). Read it at OnLoad: the host reloads the plugin on a UI-language
        /// switch, so OnLoad re-runs in the new language. Main-thread only.</summary>
        CultureInfo Culture { get; }

        /// <summary>Writes to the host log at <see cref="LogLevel.Info"/>. Any thread.</summary>
        void Log(string message);
        /// <summary>Writes to the host log at the given level. Any thread.</summary>
        void Log(LogLevel level, string message);

        /// <summary>Raises a user-facing toast at the given level, shown verbatim (the host prefixes this
        /// plugin's name). Any thread. Not throttled — do not call every frame.</summary>
        void Notify(NotifyLevel level, string message);
        /// <summary>Shorthand for <see cref="Notify"/> at <see cref="NotifyLevel.Info"/>.</summary>
        void NotifyInfo(string message);
        /// <summary>Shorthand for <see cref="Notify"/> at <see cref="NotifyLevel.Success"/>.</summary>
        void NotifySuccess(string message);
        /// <summary>Shorthand for <see cref="Notify"/> at <see cref="NotifyLevel.Warning"/>.</summary>
        void NotifyWarning(string message);
        /// <summary>Shorthand for <see cref="Notify"/> at <see cref="NotifyLevel.Error"/>.</summary>
        void NotifyError(string message);

        // --- Threading ---

        /// <summary>True when called on ofs-ng's main thread — the only thread that may touch most Host
        /// members. Use the <c>RunOnMainThread</c> helpers to marshal work in from another thread.</summary>
        bool IsMainThread { get; }
        /// <summary>Queues <paramref name="work"/> to run on the main thread at the start of the next frame.
        /// Fire-and-forget; safe to call from any thread.</summary>
        void RunOnMainThread(Action work);
        /// <summary>Queues <paramref name="work"/> on the main thread and returns a task that completes when
        /// it has run (or is cancelled if the plugin unloads first). Awaitable from any thread.</summary>
        Task RunOnMainThreadAsync(Action work);
        /// <summary>Queues <paramref name="work"/> on the main thread and returns a task carrying its result
        /// (or cancelled if the plugin unloads first). Awaitable from any thread.</summary>
        Task<T> RunOnMainThreadAsync<T>(Func<T> work);

        /// <summary>Cancelled when this plugin is being unloaded (disabled or app shutdown). Use it to
        /// stop background threads, timers and Tasks. After cancellation, do not touch Host members.
        /// Note: <see cref="OfsPlugin.OnUnload"/> runs BEFORE this is cancelled — OnUnload is the
        /// imperative cleanup hook; the token is for work that outlives OnUnload.</summary>
        CancellationToken UnloadToken { get; }
    }

    /// <summary>Concrete host implementation over the native <see cref="HostApi"/> pointer.</summary>
    internal sealed unsafe class OfsHost : IOfsHost, IDisposable
    {
        private readonly HostApi* _api;
        private readonly int _mainThreadId;
        private bool _inOnLoad;

        // Cancelled when this plugin unloads; surfaced to plugins via UnloadToken so they can stop
        // background work. Cancelled in SignalUnload (called from PluginBridge.Dispose).
        private readonly CancellationTokenSource _unloadCts = new();

        // Bumped once per frame (PluginBridge._onUpdate) before plugin update logic runs. Per-frame Axis
        // views capture it and refuse access on a later frame — see Axis.CheckFresh.
        private int _frameGen;

        // Background-thread work marshaled onto the main thread, pumped once per frame.
        private readonly ConcurrentQueue<Action> _mainThreadQueue = new();

        // Every live ProjectScoped<T> / AppScoped<T>. Each is flushed once per frame (its Sync) at the start
        // of OnUpdate so a plugin never has to remember a manual Sync() — see FlushScopedValues. Disposed and
        // cleared on unload alongside the other plugin-rooting delegates (a scoped value holds a plugin-defined
        // T plus a reused JSON writer).
        private readonly List<IScopedValue> _scopedValues = new();

        public Player Player { get; }
        public Axes Axes { get; }
        public Commands Commands { get; }
        public Nodes Nodes { get; }
        public Dialogs Dialogs { get; }
        public Project Project { get; }
        public Editing Editing { get; }
        public Navigation Navigation { get; }
        public Selection Selection { get; }

        public OfsHost(HostApi* api)
        {
            _api = api;
            _mainThreadId = Environment.CurrentManagedThreadId;

            // Route every swallowed plugin exception (bridge lambdas + node trampolines) to this host's log
            // and a throttled toast. The sinks are static because node trampolines are static; the last host
            // constructed wins, which is fine — native attributes the fault by the active plugin call context.
            // These lambdas capture this host, so SignalUnload clears them — otherwise the static sinks would
            // root this host (and the plugin delegates it owns) and the plugin's ALC would never unload.
            PluginGuard.InstallSinks(this, (ctx, msg) => Log(LogLevel.Error, $"[{ctx}] {msg}"), ReportFault);

            Player = new Player(this, api);
            Axes = new Axes(this, api);
            Commands = new Commands(this, api);
            Nodes = new Nodes(this, api);
            Dialogs = new Dialogs(this, api);
            Project = new Project(this, api);
            Editing = new Editing(this, api);
            Navigation = new Navigation(this, api);
            Selection = new Selection(this, api);
        }

        internal HostApi* Api => _api;

        internal void BeginOnLoad() => _inOnLoad = true;
        internal void EndOnLoad() => _inOnLoad = false;

        internal int FrameGen => _frameGen;
        internal void BeginFrame() => _frameGen++;

        // Register a ProjectScoped<T>/AppScoped<T> so it is flushed automatically every frame
        // (FlushScopedValues) and disposed on unload.
        internal void RegisterScopedFlush(IScopedValue value) => _scopedValues.Add(value);

        // Persist every ProjectScoped<T>/AppScoped<T> whose Value changed since last frame. Called once per
        // frame at the start of OnUpdate so an author never has to remember a manual Sync(); each flush diffs
        // (serialize + compare) and writes only on change. Resilient: one scoped value that throws (e.g. a
        // settings type that won't serialize) is logged and skipped without aborting the rest.
        internal void FlushScopedValues()
        {
            foreach (var scoped in _scopedValues)
            {
                try { scoped.Sync(); }
                catch (Exception ex) { Log(LogLevel.Error, $"Scoped value sync threw: {ex}"); }
            }
        }

        public CancellationToken UnloadToken => _unloadCts.Token;

        // Cancel + dispose the unload token so background work observes cancellation before the bridge
        // tears down node slots and the ALC. Best-effort; must never throw on teardown.
        internal void SignalUnload()
        {
            // Drop the static fault sinks if they still point at this host, so they stop rooting it (and the
            // plugin command/event delegates it owns) — without this the plugin's ALC never collects.
            PluginGuard.ClearSinks(this);
            try { _unloadCts.Cancel(); } catch { }
            Dispose();
        }

        // Disposes the unload token source. Invoked from SignalUnload (after cancellation) on plugin teardown.
        public void Dispose()
        {
            try { _unloadCts.Dispose(); } catch { }
        }

        // Drop every plugin-supplied delegate the host sub-objects hold: event subscriptions (Player/Axes),
        // command handlers, and node slots. Because Ofs.Api is shared with the host's
        // (non-collectible) load context, each such delegate captures the plugin instance and roots its
        // collectible ALC — so the plugin DLL would never unload (its file stays locked). Severing them here
        // means a plugin never has to unsubscribe by hand. Best-effort; must never throw on teardown.
        internal void DropPluginDelegates()
        {
            try { Player.ClearSubscribers(); } catch { }
            try { Axes.ClearSubscribers(); } catch { }
            // Each scoped value captures a plugin-defined T and owns a reused JSON writer; dispose then drop.
            try { foreach (var scoped in _scopedValues) { try { scoped.Dispose(); } catch { } } _scopedValues.Clear(); } catch { }
            try { Commands.ClearHandlers(); } catch { }
            try { Nodes.ReleaseOwnedSlots(); } catch { }
            try { Editing.ReleaseOwnedSlots(); } catch { }
            try { Navigation.ReleaseOwnedSlots(); } catch { }
            try { Selection.ReleaseOwnedSlots(); } catch { }
        }

        internal void AssertMainThread(string method)
        {
            if (Environment.CurrentManagedThreadId != _mainThreadId)
                throw new InvalidOperationException(
                    $"Host.{method} must be called from the main thread. " +
                    "Use RunOnMainThread / RunOnMainThreadAsync to marshal work from another thread.");
        }

        internal void AssertOnLoad(string method)
        {
            if (!_inOnLoad)
                throw new InvalidOperationException($"Host.{method} must be called from OnLoad, not at runtime.");
        }

        // --- Threading ---

        public bool IsMainThread => Environment.CurrentManagedThreadId == _mainThreadId;

        public void RunOnMainThread(Action work)
        {
            ArgumentNullException.ThrowIfNull(work);
            _mainThreadQueue.Enqueue(work);
        }

        public Task RunOnMainThreadAsync(Action work)
        {
            ArgumentNullException.ThrowIfNull(work);
            var tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            // After unload the queue is no longer pumped, so resolve as canceled instead of hanging forever.
            if (_unloadCts.IsCancellationRequested) { tcs.SetCanceled(_unloadCts.Token); return tcs.Task; }
            _mainThreadQueue.Enqueue(() =>
            {
                if (_unloadCts.IsCancellationRequested) { tcs.TrySetCanceled(_unloadCts.Token); return; }
                try { work(); tcs.TrySetResult(); }
                catch (Exception ex) { tcs.TrySetException(ex); }
            });
            return tcs.Task;
        }

        public Task<T> RunOnMainThreadAsync<T>(Func<T> work)
        {
            ArgumentNullException.ThrowIfNull(work);
            var tcs = new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);
            if (_unloadCts.IsCancellationRequested) { tcs.SetCanceled(_unloadCts.Token); return tcs.Task; }
            _mainThreadQueue.Enqueue(() =>
            {
                if (_unloadCts.IsCancellationRequested) { tcs.TrySetCanceled(_unloadCts.Token); return; }
                try { tcs.TrySetResult(work()); }
                catch (Exception ex) { tcs.TrySetException(ex); }
            });
            return tcs.Task;
        }

        /// <summary>Runs all queued main-thread work. Called once per frame at the start of OnUpdate.</summary>
        internal void PumpMainThreadQueue()
        {
            while (_mainThreadQueue.TryDequeue(out var work))
            {
                try { work(); }
                catch (Exception ex) { Log(LogLevel.Error, $"RunOnMainThread work threw: {ex}"); }
            }
        }

        // --- Misc host services ---

        public AppScoped<T> AppScoped<T>(string key) where T : new()
        {
            AssertMainThread(nameof(AppScoped));
            var scoped = new AppScoped<T>(this, key);
            // Flush edits once per frame (the host disposes + drops the list on unload via DropPluginDelegates,
            // so the plugin needs no manual Sync or teardown). App settings don't reload mid-session, so —
            // unlike ProjectScoped — there is no project-change subscription.
            RegisterScopedFlush(scoped);
            return scoped;
        }

        // This plugin's stored app-settings value for the key as a JsonElement, or an empty element
        // (ValueKind == Undefined) when nothing is stored. Backs AppScoped<T>.Reload. Main-thread only.
        internal JsonElement GetAppData(string key)
        {
            AssertMainThread(nameof(GetAppData));
            if (string.IsNullOrEmpty(key)) return default;
            byte[] keyBytes = Encoding.UTF8.GetBytes(key + "\0");
            string json = GrowAndRead((buf, size) =>
            {
                fixed (byte* keyPtr = keyBytes)
                    return _api->GetAppData(_api->Ctx, keyPtr, buf, size);
            });
            if (string.IsNullOrEmpty(json)) return default;
            using var doc = JsonDocument.Parse(json);
            return doc.RootElement.Clone(); // Clone owns its memory independently of the document
        }

        // Raw UTF-8 JSON write for AppScoped<T>.Sync, which already holds the serialized value in its reused
        // buffer (skips a JsonElement round-trip). The host caches the write in memory and persists it
        // debounced once per frame and on app close; an empty payload erases the key. Main-thread only.
        internal void SetAppData(string key, ReadOnlySpan<byte> utf8Json)
        {
            AssertMainThread(nameof(SetAppData));
            if (string.IsNullOrEmpty(key)) return;
            byte[] keyBytes = Encoding.UTF8.GetBytes(key + "\0");
            byte[] valBytes = new byte[utf8Json.Length + 1]; // + NUL: the host reads a C string
            utf8Json.CopyTo(valBytes);
            fixed (byte* keyPtr = keyBytes)
            fixed (byte* valPtr = valBytes)
                _api->SetAppData(_api->Ctx, keyPtr, valPtr);
        }

        public string Language
        {
            get
            {
                AssertMainThread(nameof(Language));
                return GrowAndRead((buf, size) => _api->GetActiveLanguage(_api->Ctx, buf, size));
            }
        }

        public CultureInfo Culture => CultureFromCode(Language);

        // Map ofs-ng's BCP 47 culture tag to a .NET culture. "" / "en" → invariant (a plugin's neutral
        // .resx). A script/region subtag (e.g. "zh-Hant") resolves to that specific culture, with .NET's
        // own resource fallback walking it down. Any tag .NET doesn't recognize falls back to invariant
        // rather than throwing.
        private static CultureInfo CultureFromCode(string code)
        {
            code ??= string.Empty;
            if (code.Length == 0 || code.Equals("en", StringComparison.OrdinalIgnoreCase))
                return CultureInfo.InvariantCulture;
            try { return CultureInfo.GetCultureInfo(code); }
            catch (CultureNotFoundException) { return CultureInfo.InvariantCulture; }
        }

        public void Log(string message) => Log(LogLevel.Info, message);

        public void Log(LogLevel level, string message)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(message + "\0");
            fixed (byte* p = bytes)
                _api->HostLog(_api->Ctx, (int)level, p);
        }

        // Raise a user-facing error toast for a swallowed plugin fault. The host names the plugin and
        // throttles/coalesces, so calling this per occurrence (even per node sample) is safe. `faultCtx`
        // is the entry point that threw (e.g. "OnUpdate", "node:gen").
        internal void ReportFault(string faultCtx)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(faultCtx + "\0");
            fixed (byte* p = bytes)
                _api->HostReportFault(_api->Ctx, p);
        }

        // Plugin-initiated user-facing toast at the chosen level, shown verbatim (host prefixes the
        // plugin name). Not throttled — do not call every frame.
        public void Notify(NotifyLevel level, string message)
        {
            if (message == null) return;
            byte[] bytes = Encoding.UTF8.GetBytes(message + "\0");
            fixed (byte* p = bytes)
                _api->HostNotify(_api->Ctx, (int)level, p);
        }

        public void NotifyInfo(string message) => Notify(NotifyLevel.Info, message);
        public void NotifySuccess(string message) => Notify(NotifyLevel.Success, message);
        public void NotifyWarning(string message) => Notify(NotifyLevel.Warning, message);
        public void NotifyError(string message) => Notify(NotifyLevel.Error, message);

        // Reads a NUL-terminated UTF-8 string from a native fill callback. Per the HostApi getter
        // contract the callback RETURNS the full required byte length (excl NUL); if that doesn't fit the
        // buffer it passed, we reallocate to exactly (required + 1) and read once more — a single
        // deterministic retry, never a silent clip. The buffer is always NUL-terminated, so a fit on the
        // first call reads back immediately.
        internal delegate int NativeFill(byte* buf, int size);

        internal static string GrowAndRead(NativeFill fill)
        {
            // Loop (not a single retry) on the getter's reported size: each `required` is the exact byte
            // length needed, so the second read normally fits — but if the underlying value grew again
            // between reads we re-read at the new size rather than silently truncating.
            int size = 1024;
            while (true)
            {
                byte[] rented = ArrayPool<byte>.Shared.Rent(size);
                try
                {
                    fixed (byte* buf = rented)
                    {
                        int required = fill(buf, rented.Length);
                        if (required < rented.Length) // fit (room for the value + its NUL)
                            return Marshal.PtrToStringUTF8((IntPtr)buf) ?? string.Empty;
                        size = required + 1;
                    }
                }
                finally { ArrayPool<byte>.Shared.Return(rented); }
            }
        }

        // The overflow tail shared by the composite getters (Project.Chapters/Bookmarks/Regions). Those loops
        // first read each record's name into a small shared stack buffer; when a name didn't fit (the getter
        // reported `required` >= that buffer), this re-reads just that record into a buffer sized exactly for
        // it. Kept separate from GrowAndRead because the caller already did the first (stack-buffer) read.
        internal static string RereadName(int required, NativeFill reread)
        {
            byte[] big = ArrayPool<byte>.Shared.Rent(required + 1);
            try
            {
                fixed (byte* buf = big)
                {
                    reread(buf, big.Length);
                    return Marshal.PtrToStringUTF8((IntPtr)buf) ?? string.Empty;
                }
            }
            finally { ArrayPool<byte>.Shared.Return(big); }
        }
    }
}
