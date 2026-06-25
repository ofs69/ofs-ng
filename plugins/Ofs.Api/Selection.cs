using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Ofs
{
    /// <summary>
    /// Which selection-authoring gesture a <see cref="SelectRequest"/> carries. A mode reinterprets what
    /// the gesture <em>means</em>; the native resolution selects every candidate it covers.
    /// </summary>
    public enum SelectGesture
    {
        /// <summary>Marquee / time-range drag — every action in [<see cref="SelectRequest.StartTime"/>,
        /// <see cref="SelectRequest.EndTime"/>] (the native resolution ignores the position range).</summary>
        Box = 0,
        /// <summary>Select-all (Ctrl+A) — every action on the axis.</summary>
        All = 1,
        /// <summary>Click / Ctrl-click — the action at <see cref="SelectRequest.StartTime"/>.</summary>
        Point = 2,
    }

    /// <summary>
    /// One selection gesture handed to an active selection mode, <b>once per editable axis</b> (the host
    /// fans out per-axis). The mode reads the region and axis, enumerates candidates itself through
    /// <c>Host.Axes[req.Axis]</c>, and returns the actions it keeps.
    /// </summary>
    public readonly struct SelectRequest
    {
        /// <summary>Which gesture authored this request.</summary>
        public SelectGesture Gesture { get; }
        /// <summary>The axis being resolved (the lead, then each group follower — one call each).</summary>
        public StandardAxis Axis { get; }
        /// <summary>Box: range start. Point: the clicked time (== <see cref="EndTime"/>).</summary>
        public double StartTime { get; }
        /// <summary>Box: range end.</summary>
        public double EndTime { get; }
        /// <summary>Point: the clicked position.</summary>
        public int Pos { get; }

        internal SelectRequest(SelectGesture gesture, StandardAxis axis, double startTime, double endTime,
            int pos)
        {
            Gesture = gesture; Axis = axis; StartTime = startTime; EndTime = endTime;
            Pos = pos;
        }
    }

    /// <summary>
    /// What a selection mode decides for a request: select the native candidates, select nothing, or
    /// select exactly the named actions instead.
    /// </summary>
    public readonly struct SelectResult
    {
        internal OfsSelectDisposition Disposition { get; }
        internal IReadOnlyList<ScriptAction>? Actions { get; }

        private SelectResult(OfsSelectDisposition d, IReadOnlyList<ScriptAction>? actions) { Disposition = d; Actions = actions; }

        /// <summary>Select every native candidate the gesture covers (the default resolution).</summary>
        public static SelectResult Pass => new(OfsSelectDisposition.Pass, null);

        /// <summary>Select nothing for this axis.</summary>
        public static SelectResult Drop => new(OfsSelectDisposition.Drop, null);

        /// <summary>Select exactly these actions (an action that names no point on the axis is ignored;
        /// none ≡ <see cref="Drop"/>).</summary>
        public static SelectResult Replace(params ScriptAction[] actions)
            => new(OfsSelectDisposition.Replace, actions);

        /// <summary>Select exactly these actions.</summary>
        public static SelectResult Replace(IEnumerable<ScriptAction> actions)
            => new(OfsSelectDisposition.Replace, new List<ScriptAction>(actions));
    }

    /// <summary>Resolves one <see cref="SelectRequest"/> into a <see cref="SelectResult"/>. Runs on the main thread.</summary>
    public delegate SelectResult SelectHandler(SelectRequest request);

    /// <summary>
    /// Registration of alternate selection modes. A mode owns <em>which actions a selection gesture
    /// selects</em> — given the region the user gestured over, it decides what becomes selected (only the
    /// peaks, only actions above a threshold, every Nth, …). It cannot edit actions, only select existing
    /// ones. Registering only publishes the mode; the <b>user</b> activates it from the footer's Select
    /// selector (no plugin-callable setter). Call <see cref="RegisterMode"/> from <see cref="OfsPlugin.OnLoad"/>.
    /// </summary>
    /// <remarks>The <c>onSelect</c> callback runs on the main thread, once per editable axis. It must return quickly.</remarks>
    public sealed unsafe class Selection
    {
        private readonly OfsHost _host;
        private readonly HostApi* _api;

        internal Selection(OfsHost host, HostApi* api)
        {
            _host = host;
            _api = api;
        }

        // One registered mode's resolver (+ optional options UI), reached by slot index (the def's
        // UserData). Static so the [UnmanagedCallersOnly] trampolines can read it; a released slot is nulled
        // on unload and the trampolines then no-op (onSelect → Pass). Slots only ever grow.
        private sealed class SelSlot
        {
            public SelectHandler OnSelect = null!;
            public Action? OnEnter;
            public Action? OnExit;
            public Action<Ui>? OnUi;
            public Ui? Ui; // built once at registration; drawn only while this mode is active
        }

        private static readonly object s_lock = new();
        private static readonly List<SelSlot?> s_slots = new();
        private readonly List<int> _ownedSlots = new();

        private static SelSlot? GetSlot(int i)
        {
            lock (s_lock) { return (i >= 0 && i < s_slots.Count) ? s_slots[i] : null; }
        }

        // Null this plugin's slots on unload so its delegates (and thus its collectible ALC) are released;
        // the native registry entries are dropped in parallel by UnregisterSelectionModesEvent.
        internal void ReleaseOwnedSlots()
        {
            lock (s_lock)
            {
                foreach (int i in _ownedSlots)
                    if (i >= 0 && i < s_slots.Count)
                        s_slots[i] = null;
                _ownedSlots.Clear();
            }
        }

        /// <summary>
        /// Publishes a selection mode the user can select in the footer's Select selector. Call from OnLoad.
        /// </summary>
        /// <param name="id">Local id (no namespace prefix); the host prepends the plugin name.</param>
        /// <param name="displayName">Name shown in the footer's Select selector (rendered verbatim).</param>
        /// <param name="onSelect">Resolves each gesture per axis: Pass / Drop / Replace(times).</param>
        /// <param name="onEnter">Optional: runs when the user activates this mode.</param>
        /// <param name="onExit">Optional: runs when the user leaves this mode.</param>
        /// <param name="onUi">Optional: draws this mode's options. The host calls it (with a <see cref="Ui"/>
        /// builder) only while this mode is the active one, in the docked Tool Options panel — so you no
        /// longer hand-gate the options on <see cref="IsActive"/>. Null = no options section.</param>
        public void RegisterMode(string id, string displayName, SelectHandler onSelect,
            Action? onEnter = null, Action? onExit = null, Action<Ui>? onUi = null)
        {
            _host.AssertMainThread(nameof(RegisterMode));
            _host.AssertOnLoad(nameof(RegisterMode));
            if (string.IsNullOrEmpty(id)) throw new ArgumentException("Selection mode id must be non-empty.", nameof(id));
            ArgumentNullException.ThrowIfNull(onSelect);

            int slot;
            lock (s_lock)
            {
                slot = s_slots.Count;
                s_slots.Add(new SelSlot
                {
                    OnSelect = onSelect,
                    OnEnter = onEnter,
                    OnExit = onExit,
                    OnUi = onUi,
                    Ui = onUi != null ? new Ui(_api) : null,
                });
                _ownedSlots.Add(slot);
            }

            var hId = GCHandle.Alloc(Encoding.UTF8.GetBytes(id + "\0"), GCHandleType.Pinned);
            var hName = GCHandle.Alloc(Encoding.UTF8.GetBytes((displayName ?? string.Empty) + "\0"), GCHandleType.Pinned);
            try
            {
                var def = new OfsSelectModeDef
                {
                    Id = (byte*)hId.AddrOfPinnedObject(),
                    DisplayName = (byte*)hName.AddrOfPinnedObject(),
                    OnSelect = (IntPtr)(delegate* unmanaged[Cdecl]<void*, OfsSelectRequest*,
                        delegate* unmanaged[Cdecl]<void*, ScriptAction, void>, void*, int>)&SelectTrampoline,
                    OnEnter = onEnter != null ? (IntPtr)(delegate* unmanaged[Cdecl]<void*, void>)&EnterTrampoline : default,
                    OnExit = onExit != null ? (IntPtr)(delegate* unmanaged[Cdecl]<void*, void>)&ExitTrampoline : default,
                    OnUi = onUi != null ? (IntPtr)(delegate* unmanaged[Cdecl]<void*, void>)&UiTrampoline : default,
                    UserData = (void*)(nint)slot,
                };
                _api->RegisterSelectionMode(_api->Ctx, &def);
            }
            finally
            {
                hId.Free();
                hName.Free();
            }
        }

        /// <summary>
        /// Whether the selection mode this plugin registered as <paramref name="id"/> is the one the user has
        /// active in the footer. Use it to gate the plugin's selection UI. The host resolves
        /// <paramref name="id"/> the same way <see cref="RegisterMode"/> does (it prepends the plugin name),
        /// so pass the same local id.
        /// </summary>
        public bool IsActive(string id)
        {
            _host.AssertMainThread(nameof(IsActive));
            if (string.IsNullOrEmpty(id)) return false;
            byte[] bytes = Encoding.UTF8.GetBytes(id + "\0");
            fixed (byte* p = bytes) return _api->IsSelectionModeActive(_api->Ctx, p) == 1;
        }

        // ── Trampoline (static; survives plugin unload — it lives in the shared Ofs.Api) ──

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static int SelectTrampoline(void* ud, OfsSelectRequest* inp,
            delegate* unmanaged[Cdecl]<void*, ScriptAction, void> emit, void* sink)
        {
            var slot = GetSlot((int)(nint)ud);
            if (slot?.OnSelect == null || inp == null) return (int)OfsSelectDisposition.Pass; // released → native
            var axis = (inp->Axis >= 0 && inp->Axis < (int)StandardAxis.Count) ? (StandardAxis)inp->Axis : StandardAxis.L0;
            var req = new SelectRequest((SelectGesture)inp->Gesture, axis, inp->StartTime, inp->EndTime,
                inp->Pos);
            SelectResult result = PluginGuard.Run("selectmode:onSelect", () => slot.OnSelect(req), SelectResult.Pass);
            if (result.Disposition == OfsSelectDisposition.Replace && result.Actions != null && emit != null)
                foreach (ScriptAction a in result.Actions)
                    emit(sink, a);
            return (int)result.Disposition;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void EnterTrampoline(void* ud)
        {
            var slot = GetSlot((int)(nint)ud);
            if (slot?.OnEnter != null) PluginGuard.Run("selectmode:onEnter", slot.OnEnter);
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void ExitTrampoline(void* ud)
        {
            var slot = GetSlot((int)(nint)ud);
            if (slot?.OnExit != null) PluginGuard.Run("selectmode:onExit", slot.OnExit);
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void UiTrampoline(void* ud)
        {
            var slot = GetSlot((int)(nint)ud);
            if (slot?.OnUi == null || slot.Ui == null) return; // released → nothing to draw
            Action<Ui> onUi = slot.OnUi;
            Ui ui = slot.Ui;
            ui.BeginPass();
            try { PluginGuard.Run("selectmode:onUi", () => onUi(ui)); }
            finally { ui.EndPass(); }
        }
    }
}
