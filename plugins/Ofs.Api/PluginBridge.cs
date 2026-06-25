using System;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

namespace Ofs
{
    /// <summary>
    /// INTERNAL wiring layer. Given an <see cref="OfsPlugin"/>, the native <see cref="HostApi"/>
    /// and the <see cref="PluginApi"/> to populate, it builds the managed host, runs OnLoad, and
    /// forwards every native callback into the plugin's lifecycle methods and the host's events.
    /// Reachable from Ofs.PluginHost via [InternalsVisibleTo].
    /// </summary>
    internal sealed unsafe class PluginBridge : IDisposable
    {
        private delegate IntPtr GetNameDelegate();
        private delegate void OnLoadDelegate();
        private delegate void OnBuildUIDelegate();
        private delegate void OnUpdateDelegate(float delta);
        private delegate void OnTimeChangeDelegate(double time);
        private delegate void OnPlayChangeDelegate(int playing);
        private delegate void OnSpeedChangeDelegate(float speed);
        private delegate void OnMediaChangeDelegate(IntPtr path);
        private delegate void OnProjectChangeDelegate();
        private delegate void OnAxisModifiedDelegate(int role);
        private delegate void OnActiveAxisChangedDelegate(int role);
        private delegate void OnCommandDelegate(IntPtr id);
        private delegate void OnUnloadDelegate();

        private readonly OfsPlugin _plugin;
        private readonly OfsHost _host;

        // Stored to prevent GC while native pointers are live.
        private GetNameDelegate? _getName;
        private GetNameDelegate? _getVersion; // same () => IntPtr shape as _getName
        private OnLoadDelegate? _onLoad;
        private OnBuildUIDelegate? _onBuildUI;
        private OnUpdateDelegate? _onUpdate;
        private OnTimeChangeDelegate? _onTimeChange;
        private OnPlayChangeDelegate? _onPlayChange;
        private OnSpeedChangeDelegate? _onSpeedChange;
        private OnMediaChangeDelegate? _onMediaChange;
        private OnProjectChangeDelegate? _onProjectChange;
        private OnAxisModifiedDelegate? _onAxisModified;
        private OnActiveAxisChangedDelegate? _onActiveAxisChanged;
        private OnCommandDelegate? _onCommand;
        private OnUnloadDelegate? _onUnload;

        private byte[]? _nameBytes;
        private GCHandle _nameHandle;
        private byte[]? _versionBytes;
        private GCHandle _versionHandle;

        private PluginBridge(OfsPlugin plugin, HostApi* hostApi)
        {
            _plugin = plugin;
            _host = new OfsHost(hostApi);
        }

        /// <summary>Entry point invoked by Ofs.PluginHost. Returns the bridge so the caller can
        /// keep it (and its pinned delegates) alive for the plugin's lifetime.</summary>
        public static PluginBridge Load(OfsPlugin plugin, HostApi* hostApi, IntPtr apiPtr)
        {
            var bridge = new PluginBridge(plugin, hostApi);
            bridge.Initialize(apiPtr);
            return bridge;
        }

        // Re-evaluate the plugin's Name and return a pinned UTF-8 pointer, re-pinning only when the bytes
        // changed (the common case — same language — is a cheap compare). Valid until the next call; the
        // host copies it immediately. Non-throwing: on any failure it returns the last good pinned name.
        private IntPtr CurrentNamePtr()
        {
            try
            {
                byte[] bytes = Encoding.UTF8.GetBytes((_plugin.Name ?? string.Empty) + "\0");
                if (_nameBytes == null || !bytes.AsSpan().SequenceEqual(_nameBytes))
                {
                    if (_nameHandle.IsAllocated) _nameHandle.Free();
                    _nameBytes = bytes;
                    _nameHandle = GCHandle.Alloc(_nameBytes, GCHandleType.Pinned);
                }
            }
            catch { /* keep the last good pinned name */ }
            return _nameHandle.IsAllocated ? _nameHandle.AddrOfPinnedObject() : IntPtr.Zero;
        }

        ~PluginBridge()
        {
            if (_nameHandle.IsAllocated)
                _nameHandle.Free();
            if (_versionHandle.IsAllocated)
                _versionHandle.Free();
        }

        /// <summary>Release everything that would otherwise root the plugin's AssemblyLoadContext, so
        /// it can actually be collected when Ofs.PluginHost unloads it. Without this the plugin never
        /// truly unloads and its DLL stays locked for the whole run (blocking uninstall/replace).
        /// Called by Ofs.PluginHost.UnloadPluginNative before AssemblyLoadContext.Unload().</summary>
        public void Dispose()
        {
            // Signal cooperative cancellation FIRST so a plugin's background threads/timers/Tasks observe
            // UnloadToken and wind down before we tear delegates and the ALC out from under them.
            _host.SignalUnload();

            // Sever every plugin-supplied delegate the host sub-objects still hold — event subscriptions,
            // command/shortcut handlers and node slots. Each captures the plugin instance and,
            // because Ofs.Api is shared with the host's non-collectible context, roots the plugin's
            // collectible ALC. This is the host's job, not the plugin's: a plugin must never have to
            // unsubscribe from host events by hand to be collectible.
            _host.DropPluginDelegates();

            if (_nameHandle.IsAllocated)
                _nameHandle.Free();
            if (_versionHandle.IsAllocated)
                _versionHandle.Free();

            // Drop the marshalled-callback delegates (they capture the plugin instance) so nothing in
            // this bridge keeps the plugin ALC alive after it is dropped from the registry.
            _getName = null; _getVersion = null; _onLoad = null; _onBuildUI = null; _onUpdate = null;
            _onTimeChange = null; _onPlayChange = null; _onSpeedChange = null; _onMediaChange = null;
            _onProjectChange = null; _onAxisModified = null; _onActiveAxisChanged = null;
            _onCommand = null; _onUnload = null;

            GC.SuppressFinalize(this);
        }

        private void Initialize(IntPtr apiPtr)
        {
            _plugin.SetHost(_host);

            // This initial OnLoad is intentionally NOT guarded: a plugin whose OnLoad throws is broken and
            // must fail to load. The exception propagates to PluginBootstrapper.LoadPluginNative, whose
            // catch returns a failure code. (Only the rarely-reinvoked _onLoad function pointer is guarded.)
            _host.BeginOnLoad();
            try { _plugin.InvokeOnLoad(); }
            finally { _host.EndOnLoad(); }

            if (apiPtr == IntPtr.Zero) return;

            _nameBytes = Encoding.UTF8.GetBytes(_plugin.Name + "\0");
            _nameHandle = GCHandle.Alloc(_nameBytes, GCHandleType.Pinned);

            // Version is read straight from the plugin assembly's informational version (set via
            // <Version> in the .csproj); plugins cannot override it.
            _versionBytes = Encoding.UTF8.GetBytes(ReadAssemblyVersion(_plugin) + "\0");
            _versionHandle = GCHandle.Alloc(_versionBytes, GCHandleType.Pinned);
            IntPtr versionPtr = _versionHandle.AddrOfPinnedObject();

            // _getName/_getVersion cannot throw into native — leave them unguarded. _getName re-evaluates
            // Name each call and re-pins when it changed (the host re-calls it after a UI-language switch so
            // a Str.*-derived name follows the language); the pointer is valid until the next call, which is
            // when the host copies it. _getVersion stays a fixed pinned pointer (version is immutable).
            _getName = () => CurrentNamePtr();
            _getVersion = () => versionPtr;
            _onLoad = () => PluginGuard.Run("OnLoad", () => _plugin.InvokeOnLoad());

            // Bump the frame generation and pump the main-thread queue, then run per-frame plugin logic.
            // FlushScopedValues persists any ProjectScoped<T> edited during the previous frame's update/render
            // before this frame mutates them — one frame of latency, matching SetData's already-deferred write.
            _onUpdate = d => PluginGuard.Run("OnUpdate", () =>
            {
                _host.BeginFrame();
                _host.PumpMainThreadQueue();
                _host.FlushScopedValues();
                _plugin.InvokeOnUpdate(d);
            });

            // Player + axes events are raised on the host sub-objects.
            _onTimeChange = t => PluginGuard.Run("OnTimeChange", () => _host.Player.RaiseTimeChanged(t));
            _onPlayChange = p => PluginGuard.Run("OnPlayChange", () => _host.Player.RaisePlayingChanged(p != 0));
            _onSpeedChange = s => PluginGuard.Run("OnSpeedChange", () => _host.Player.RaiseSpeedChanged(s));
            _onMediaChange = p => PluginGuard.Run("OnMediaChange",
                () => _host.Player.RaiseMediaChanged(Marshal.PtrToStringUTF8(p) ?? string.Empty));
            _onProjectChange = () => PluginGuard.Run("OnProjectChange", () => _host.Axes.RaiseProjectChanged());
            _onAxisModified = r => PluginGuard.Run("OnAxisModified", () => _host.Axes.RaiseModified(ClampRole(r)));
            _onActiveAxisChanged = r => PluginGuard.Run("OnActiveAxisChanged",
                () => _host.Axes.RaiseActiveChanged(ClampRole(r)));

            // Command invocation dispatches through the Commands registry.
            _onCommand = p => PluginGuard.Run("OnCommand",
                () => _host.Commands.Dispatch(Marshal.PtrToStringUTF8(p) ?? string.Empty));

            _onUnload = () => PluginGuard.Run("OnUnload", () => _plugin.InvokeOnUnload());

            var api = (PluginApi*)apiPtr;
            api->Version = ApiVersions.AbiVersion;
            api->GetName = Marshal.GetFunctionPointerForDelegate(_getName);
            api->GetVersion = Marshal.GetFunctionPointerForDelegate(_getVersion);
            api->OnLoad = Marshal.GetFunctionPointerForDelegate(_onLoad);
            api->OnUpdate = Marshal.GetFunctionPointerForDelegate(_onUpdate);
            api->OnTimeChange = Marshal.GetFunctionPointerForDelegate(_onTimeChange);
            api->OnPlayChange = Marshal.GetFunctionPointerForDelegate(_onPlayChange);
            api->OnSpeedChange = Marshal.GetFunctionPointerForDelegate(_onSpeedChange);
            api->OnMediaChange = Marshal.GetFunctionPointerForDelegate(_onMediaChange);
            api->OnProjectChange = Marshal.GetFunctionPointerForDelegate(_onProjectChange);
            api->OnAxisModified = Marshal.GetFunctionPointerForDelegate(_onAxisModified);
            api->OnActiveAxisChanged = Marshal.GetFunctionPointerForDelegate(_onActiveAxisChanged);
            api->OnCommand = Marshal.GetFunctionPointerForDelegate(_onCommand);
            api->OnUnload = Marshal.GetFunctionPointerForDelegate(_onUnload);

            // Only expose a UI window if the plugin actually overrides OnRenderUi.
            if (OverridesRenderUi(_plugin))
            {
                var ui = new Ui(_host.Api);
                _onBuildUI = () =>
                {
                    ui.BeginPass();
                    try { PluginGuard.Run("OnRenderUi", () => _plugin.InvokeOnRenderUi(ui)); }
                    finally { ui.EndPass(); }
                };
                api->OnBuildUI = Marshal.GetFunctionPointerForDelegate(_onBuildUI);
            }
        }

        // A role index from native that's out of range (corrupt, or a forward-compat axis an older plugin
        // doesn't know) clamps to L0 rather than casting to a bogus enum — matches the range-check in
        // Editing.FromAbi and the selection trampoline. Active axis is also never-none, so L0 is its floor.
        private static StandardAxis ClampRole(int r) =>
            (r >= 0 && r < (int)StandardAxis.Count) ? (StandardAxis)r : StandardAxis.L0;

        // Version shown next to the name in the plugin list, read from the plugin assembly's
        // informational version (set via <Version> in the .csproj). "" if it carries none.
        private static string ReadAssemblyVersion(OfsPlugin plugin)
        {
            string v = plugin.GetType().Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()
                ?.InformationalVersion ?? "";
            // SourceLink builds append "+<gitsha>" (SemVer build metadata) to the informational
            // version; strip it so the UI shows a clean "1.2.3", not "1.2.3+a1b2c3d".
            int plus = v.IndexOf('+');
            return plus >= 0 ? v.Substring(0, plus) : v;
        }

        private static bool OverridesRenderUi(OfsPlugin plugin)
        {
            var m = plugin.GetType().GetMethod(
                "OnRenderUi",
                BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public,
                binder: null, types: new[] { typeof(Ui) }, modifiers: null);
            return m != null && m.DeclaringType != typeof(OfsPlugin);
        }
    }
}
