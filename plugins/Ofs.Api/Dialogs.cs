using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace Ofs
{
    /// <summary>The button set for <see cref="Dialogs.Confirm"/>.</summary>
    // Values are the ABI contract: they match the native confirmDialog kind (0=ok 1=okcancel 2=yesno).
    // Explicit so a reorder can't silently remap button sets once third-party plugins compile.
    public enum ConfirmKind
    {
        /// <summary>A single OK button (acknowledgement only).</summary>
        Ok = 0,
        /// <summary>OK and Cancel buttons.</summary>
        OkCancel = 1,
        /// <summary>Yes and No buttons.</summary>
        YesNo = 2,
    }

    /// <summary>
    /// Native file/folder/message dialogs, backed by the host's native dialog integration. Every call is
    /// NON-BLOCKING: it queues the dialog and returns a <see cref="Task"/> that completes — on the main
    /// thread, on a later frame — once the user answers. <c>await</c> it from a command or button handler;
    /// the frame loop keeps rendering while the dialog is open. Main-thread only (call, not the await).
    /// </summary>
    /// <remarks>
    /// Because the call returns immediately and resumes later, you drive a dialog from an <c>async</c>
    /// handler. The continuation runs back on the main thread, so it is safe to touch <c>Host</c> after the
    /// <c>await</c>. A cancel/dismiss yields <c>null</c> (or <c>false</c> for <see cref="Confirm"/>); always
    /// handle it. If the plugin unloads while a dialog is open the task is cancelled — guard the await with
    /// the plugin's <see cref="IOfsHost.UnloadToken"/> (or pass it) when the continuation does real work.
    /// </remarks>
    /// <example>
    /// A command that opens a file, confirms, and reports — each dialog awaited in turn:
    /// <code language="csharp">
    /// Host.Commands.Register("import", "Import script…", async () =>
    /// {
    ///     // ';'-separated glob list; null = all files. Returns null if the user cancelled.
    ///     string? path = await Host.Dialogs.OpenFile(
    ///         title: "Import script",
    ///         filterPatterns: "*.funscript;*.csv",
    ///         filterDesc: "Scripts");
    ///     if (path == null) return; // cancelled
    ///
    ///     if (!await Host.Dialogs.Confirm("Import", $"Import {path}?", ConfirmKind.YesNo))
    ///         return; // user chose No
    ///
    ///     Host.NotifySuccess($"Imported {path}");
    /// });
    /// </code>
    /// Save-as with a default name and a single filter, and a multi-select open:
    /// <code language="csharp">
    /// string? dest = await Host.Dialogs.SaveFile(
    ///     title: "Export axis", defaultName: "axis.funscript",
    ///     filterPatterns: "*.funscript", filterDesc: "Funscript");
    /// if (dest != null) { /* write to dest */ }
    ///
    /// string[]? files = await Host.Dialogs.OpenFiles("Add clips", "*.mp4;*.mkv", "Video");
    /// foreach (string f in files ?? Array.Empty&lt;string&gt;())
    ///     Host.Log(f);
    ///
    /// string? folder = await Host.Dialogs.PickFolder("Pick output folder");
    /// </code>
    /// Awaiting from a button handler inside <see cref="OfsPlugin.OnRenderUi"/> — the call is fire-and-forget
    /// (don't <c>await</c> in the render path); the continuation lands on a later frame, still on the main thread:
    /// <code language="csharp">
    /// if (ui.Button("Choose file…"))
    ///     _ = PickAsync();
    ///
    /// async Task PickAsync()
    /// {
    ///     string? path = await Host.Dialogs.OpenFile("Choose file");
    ///     if (path != null) _selected = path; // safe: back on the main thread
    /// }
    /// </code>
    /// </example>
    public sealed unsafe class Dialogs
    {
        private readonly OfsHost _host;
        private readonly HostApi* _api;

        internal Dialogs(OfsHost host, HostApi* api)
        {
            _host = host;
            _api = api;
        }

        private static byte[] Utf8(string s) => Encoding.UTF8.GetBytes(s + "\0");

        /// <summary>Opens a file picker. <paramref name="filterPatterns"/> is a ';'-separated glob list
        /// (e.g. "*.png;*.jpg"), or null for all files. The task yields the chosen path, or null if cancelled.</summary>
        public Task<string?> OpenFile(string title = "Open", string? filterPatterns = null, string? filterDesc = null,
            CancellationToken cancel = default)
        {
            _host.AssertMainThread(nameof(OpenFile));
            if (_host.UnloadToken.IsCancellationRequested) return Task.FromCanceled<string?>(_host.UnloadToken);
            var bridge = new DialogBridge<string?>();
            var gch = GCHandle.Alloc(bridge); // freed by FileResultTrampoline once the host calls back
            bridge.Arm(_host.UnloadToken, cancel);
            byte[] t = Utf8(title);
            byte[]? fp = filterPatterns == null ? null : Utf8(filterPatterns);
            byte[]? fd = filterDesc == null ? null : Utf8(filterDesc);
            fixed (byte* tp = t)
            fixed (byte* fpp = fp)
            fixed (byte* fdp = fd)
                _api->OpenFileDialog(_api->Ctx, tp, fpp, fdp, &FileResultTrampoline, (void*)GCHandle.ToIntPtr(gch));
            return bridge.Tcs.Task;
        }

        /// <summary>Opens a file picker that lets the user choose several files. <paramref name="filterPatterns"/>
        /// is a ';'-separated glob list (e.g. "*.png;*.jpg"), or null for all files. The task yields the chosen
        /// paths, or null if the user cancelled (selected nothing).</summary>
        public Task<string[]?> OpenFiles(string title = "Open", string? filterPatterns = null, string? filterDesc = null,
            CancellationToken cancel = default)
        {
            _host.AssertMainThread(nameof(OpenFiles));
            if (_host.UnloadToken.IsCancellationRequested) return Task.FromCanceled<string[]?>(_host.UnloadToken);
            var bridge = new DialogBridge<string[]?>();
            var gch = GCHandle.Alloc(bridge); // freed by FilesResultTrampoline once the host calls back
            bridge.Arm(_host.UnloadToken, cancel);
            byte[] t = Utf8(title);
            byte[]? fp = filterPatterns == null ? null : Utf8(filterPatterns);
            byte[]? fd = filterDesc == null ? null : Utf8(filterDesc);
            fixed (byte* tp = t)
            fixed (byte* fpp = fp)
            fixed (byte* fdp = fd)
                _api->OpenFilesDialog(_api->Ctx, tp, fpp, fdp, &FilesResultTrampoline, (void*)GCHandle.ToIntPtr(gch));
            return bridge.Tcs.Task;
        }

        /// <summary>Opens a save-as picker. The task yields the chosen path, or null if cancelled.</summary>
        public Task<string?> SaveFile(string title = "Save", string defaultName = "",
            string? filterPatterns = null, string? filterDesc = null, CancellationToken cancel = default)
        {
            _host.AssertMainThread(nameof(SaveFile));
            if (_host.UnloadToken.IsCancellationRequested) return Task.FromCanceled<string?>(_host.UnloadToken);
            var bridge = new DialogBridge<string?>();
            var gch = GCHandle.Alloc(bridge);
            bridge.Arm(_host.UnloadToken, cancel);
            byte[] t = Utf8(title);
            byte[] dn = Utf8(defaultName);
            byte[]? fp = filterPatterns == null ? null : Utf8(filterPatterns);
            byte[]? fd = filterDesc == null ? null : Utf8(filterDesc);
            fixed (byte* tp = t)
            fixed (byte* dp = dn)
            fixed (byte* fpp = fp)
            fixed (byte* fdp = fd)
                _api->SaveFileDialog(_api->Ctx, tp, dp, fpp, fdp, &FileResultTrampoline, (void*)GCHandle.ToIntPtr(gch));
            return bridge.Tcs.Task;
        }

        /// <summary>Opens a folder picker. The task yields the chosen folder, or null if cancelled.</summary>
        public Task<string?> PickFolder(string title = "Select Folder", CancellationToken cancel = default)
        {
            _host.AssertMainThread(nameof(PickFolder));
            if (_host.UnloadToken.IsCancellationRequested) return Task.FromCanceled<string?>(_host.UnloadToken);
            var bridge = new DialogBridge<string?>();
            var gch = GCHandle.Alloc(bridge);
            bridge.Arm(_host.UnloadToken, cancel);
            byte[] t = Utf8(title);
            fixed (byte* tp = t)
                _api->PickFolder(_api->Ctx, tp, &FileResultTrampoline, (void*)GCHandle.ToIntPtr(gch));
            return bridge.Tcs.Task;
        }

        /// <summary>Shows a message/confirmation box. The task yields true for OK/Yes, false for Cancel/No
        /// (or if the dialog was dismissed).</summary>
        public Task<bool> Confirm(string title, string message, ConfirmKind kind = ConfirmKind.OkCancel,
            CancellationToken cancel = default)
        {
            _host.AssertMainThread(nameof(Confirm));
            if (_host.UnloadToken.IsCancellationRequested) return Task.FromCanceled<bool>(_host.UnloadToken);
            var bridge = new DialogBridge<bool>();
            var gch = GCHandle.Alloc(bridge);
            bridge.Arm(_host.UnloadToken, cancel);
            byte[] t = Utf8(title);
            byte[] m = Utf8(message);
            fixed (byte* tp = t)
            fixed (byte* mp = m)
                _api->ConfirmDialog(_api->Ctx, tp, mp, (int)kind, &ConfirmResultTrampoline, (void*)GCHandle.ToIntPtr(gch));
            return bridge.Tcs.Task;
        }

        // Static trampolines the host invokes (once, on the main thread by ABI contract — like the
        // edit/select/nav intent trampolines, they're host-driven and so deliberately don't re-assert the
        // main thread) when the user answers. Each recovers the bridging DialogBridge from the GCHandle
        // passed as userData, completes its task, stops listening for unload, and frees the handle. The task resumes the awaiting plugin code inline on
        // this (main) thread. If the plugin already unloaded, the task is cancelled and TrySetResult no-ops.
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void FileResultTrampoline(void* userData, byte* path)
        {
            var gch = GCHandle.FromIntPtr((IntPtr)userData);
            try
            {
                if (gch.Target is DialogBridge<string?> b)
                {
                    b.Tcs.TrySetResult(path == null ? null : Decode(path));
                    b.Disarm();
                }
            }
            catch (Exception ex) { PluginGuard.Report("dialog:file", ex); }
            finally { gch.Free(); }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void FilesResultTrampoline(void* userData, byte** paths, int count)
        {
            var gch = GCHandle.FromIntPtr((IntPtr)userData);
            try
            {
                if (gch.Target is DialogBridge<string[]?> b)
                {
                    string[]? result = null;
                    if (paths != null && count > 0)
                    {
                        result = new string[count];
                        for (int i = 0; i < count; i++)
                            result[i] = Decode(paths[i]);
                    }
                    b.Tcs.TrySetResult(result); // null == cancelled / nothing chosen
                    b.Disarm();
                }
            }
            catch (Exception ex) { PluginGuard.Report("dialog:files", ex); }
            finally { gch.Free(); }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void ConfirmResultTrampoline(void* userData, int result)
        {
            var gch = GCHandle.FromIntPtr((IntPtr)userData);
            try
            {
                if (gch.Target is DialogBridge<bool> b)
                {
                    b.Tcs.TrySetResult(result != 0);
                    b.Disarm();
                }
            }
            catch (Exception ex) { PluginGuard.Report("dialog:confirm", ex); }
            finally { gch.Free(); }
        }

        // Bridges one in-flight async dialog: the awaiting plugin's TaskCompletionSource plus a registration
        // on the plugin's unload token (linked with any caller token). If the plugin unloads while the dialog
        // is still open, the registration completes the task as cancelled so the awaiting continuation stops
        // rooting the plugin's ALC — otherwise the open dialog's strong GCHandle pins it until the user closes
        // the dialog. The native callback still owns that handle and frees it when the dialog returns; by then
        // TrySetResult is a harmless no-op on the already-cancelled task.
        private sealed class DialogBridge<T>
        {
            public readonly TaskCompletionSource<T> Tcs = new();
            private CancellationTokenSource? _linked;
            private CancellationTokenRegistration _reg;

            public void Arm(CancellationToken unload, CancellationToken caller)
            {
                _linked = CancellationTokenSource.CreateLinkedTokenSource(unload, caller);
                _reg = _linked.Token.Register(static s => ((DialogBridge<T>)s!).Tcs.TrySetCanceled(), this);
            }

            public void Disarm()
            {
                _reg.Dispose();
                _linked?.Dispose();
            }
        }

        private static string Decode(byte* p)
        {
            int len = 0;
            while (p[len] != 0) len++;
            return Encoding.UTF8.GetString(p, len);
        }
    }
}
