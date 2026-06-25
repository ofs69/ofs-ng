using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Ofs
{
    /// <summary>
    /// Which step a <see cref="NavStep"/> targets. A navigator may redefine one channel and
    /// <see cref="Nav.Pass"/> the rest back to the native (overlay / adjacent-action) resolution.
    /// </summary>
    public enum NavGranularity
    {
        /// <summary>The overlay grid (frame interval / tempo beat) — the ← / → keys.</summary>
        Frame = 0,
        /// <summary>The adjacent action on the active axis — the ↓ / ↑ keys.</summary>
        Action = 1,
        /// <summary>The nearest adjacent action across all axes — the Ctrl+↓ / Ctrl+↑ keys.</summary>
        ActionAllAxes = 2,
    }

    /// <summary>A step request handed to an active navigator: which way, how many steps, which channel.</summary>
    public readonly struct NavStep
    {
        /// <summary><see cref="StepDirection.Backward"/> for the previous step, <see cref="StepDirection.Forward"/> for the next.</summary>
        public StepDirection Direction { get; }
        /// <summary>Held-repeat burst count (≥ 1) — one event may stand for several rapid steps.</summary>
        public int Reps { get; }
        /// <summary>Which step channel the user asked for; Pass the ones you don't redefine.</summary>
        public NavGranularity Granularity { get; }

        internal NavStep(int direction, int reps, NavGranularity granularity)
        {
            Direction = direction < 0 ? StepDirection.Backward : StepDirection.Forward;
            Reps = Math.Max(1, reps);
            Granularity = granularity;
        }
    }

    /// <summary>
    /// A navigator's answer to a <see cref="NavStep"/>: seek to a time, do nothing, or pass the step to
    /// the native resolution for its granularity.
    /// </summary>
    public readonly struct Nav
    {
        internal bool HasSeek { get; }
        internal bool IsPass { get; }
        internal double Time { get; }
        internal bool HasAxis { get; } // false → keep the host default (the current active axis)
        internal StandardAxis Axis { get; }

        private Nav(bool hasSeek, bool isPass, double time, bool hasAxis, StandardAxis axis)
        {
            HasSeek = hasSeek; IsPass = isPass; Time = time; HasAxis = hasAxis; Axis = axis;
        }

        /// <summary>Move the playhead to <paramref name="time"/> seconds, keeping the current active axis.</summary>
        public static Nav Seek(double time) => new(true, false, time, false, default);

        /// <inheritdoc cref="Seek(double)"/>
        public static Nav Seek(ScriptAction action) => Seek(action.At);

        /// <summary>
        /// Move the playhead to <paramref name="time"/> seconds <b>and</b> make <paramref name="axis"/> the
        /// active axis — the way the native <see cref="NavGranularity.ActionAllAxes"/> step lands on a
        /// multi-axis action and activates whichever axis owns it. Use the plain <see cref="Seek(double)"/>
        /// when you don't need to switch axes; the active one is kept for you.
        /// </summary>
        public static Nav Seek(double time, StandardAxis axis) => new(true, false, time, true, axis);

        /// <inheritdoc cref="Seek(double, StandardAxis)"/>
        public static Nav Seek(ScriptAction action, StandardAxis axis) => Seek(action.At, axis);

        /// <summary>Don't move the playhead (swallow the step).</summary>
        public static Nav None => new(false, false, 0, false, default);

        /// <summary>Defer to the host's native resolution for this step's granularity.</summary>
        public static Nav Pass => new(false, true, 0, false, default);
    }

    /// <summary>Resolves a <see cref="NavStep"/> into a <see cref="Nav"/>. Runs on the main thread.</summary>
    public delegate Nav NavStepHandler(NavStep step);

    /// <summary>
    /// Registration of alternate navigators. A navigator owns what the prev/next-step keys do — given a
    /// step, it decides where the playhead goes (next action, a custom grid, …). It does not move the
    /// playhead any other way and cannot edit actions. Registering only publishes the navigator; the
    /// <b>user</b> activates it from the footer (no plugin-callable setter). Call
    /// <see cref="RegisterMode"/> from <see cref="OfsPlugin.OnLoad"/>.
    /// </summary>
    public sealed unsafe class Navigation
    {
        private readonly OfsHost _host;
        private readonly HostApi* _api;

        internal Navigation(OfsHost host, HostApi* api)
        {
            _host = host;
            _api = api;
        }

        // One registered navigator's resolver (+ optional options UI), reached by slot index (the def's
        // UserData). Static so the [UnmanagedCallersOnly] trampolines can read it; a released slot is nulled
        // on unload and the trampolines then no-op. Slots only ever grow.
        private sealed class NavSlot
        {
            public NavStepHandler OnStep = null!;
            public Action? OnEnter;
            public Action? OnExit;
            public Action<Ui>? OnUi;
            public Ui? Ui; // built once at registration; drawn only while this navigator is active
        }

        private static readonly object s_lock = new();
        private static readonly List<NavSlot?> s_slots = new();
        private readonly List<int> _ownedSlots = new();

        private static NavSlot? GetSlot(int i)
        {
            lock (s_lock) { return (i >= 0 && i < s_slots.Count) ? s_slots[i] : null; }
        }

        // Null this plugin's slots on unload so its delegates (and thus its collectible ALC) are released;
        // the native registry entries are dropped in parallel by UnregisterNavigatorsEvent.
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
        /// Publishes a navigator the user can select in the footer's Step selector. Call from OnLoad.
        /// </summary>
        /// <param name="id">Local id (no namespace prefix); the host prepends the plugin name.</param>
        /// <param name="displayName">Name shown in the footer's Step selector (rendered verbatim).</param>
        /// <param name="onStep">Resolves each step into <see cref="Nav.Seek(double)"/>, <see cref="Nav.None"/>, or <see cref="Nav.Pass"/>.</param>
        /// <param name="onEnter">Optional: runs when the user activates this navigator.</param>
        /// <param name="onExit">Optional: runs when the user leaves this navigator.</param>
        /// <param name="onUi">Optional: draws this navigator's options. The host calls it (with a <see cref="Ui"/>
        /// builder) only while this navigator is the active one, in the docked Tool Options panel — so you no
        /// longer hand-gate the options on <see cref="IsActive"/>. Null = no options section.</param>
        public void RegisterMode(string id, string displayName, NavStepHandler onStep,
            Action? onEnter = null, Action? onExit = null, Action<Ui>? onUi = null)
        {
            _host.AssertMainThread(nameof(RegisterMode));
            _host.AssertOnLoad(nameof(RegisterMode));
            if (string.IsNullOrEmpty(id)) throw new ArgumentException("Navigator id must be non-empty.", nameof(id));
            ArgumentNullException.ThrowIfNull(onStep);

            int slot;
            lock (s_lock)
            {
                slot = s_slots.Count;
                s_slots.Add(new NavSlot
                {
                    OnStep = onStep,
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
                var def = new OfsNavigatorDef
                {
                    Id = (byte*)hId.AddrOfPinnedObject(),
                    DisplayName = (byte*)hName.AddrOfPinnedObject(),
                    OnStep = (IntPtr)(delegate* unmanaged[Cdecl]<void*, OfsNavIntent*, OfsNavIntent*, int>)&StepTrampoline,
                    OnEnter = onEnter != null ? (IntPtr)(delegate* unmanaged[Cdecl]<void*, void>)&EnterTrampoline : default,
                    OnExit = onExit != null ? (IntPtr)(delegate* unmanaged[Cdecl]<void*, void>)&ExitTrampoline : default,
                    OnUi = onUi != null ? (IntPtr)(delegate* unmanaged[Cdecl]<void*, void>)&UiTrampoline : default,
                    UserData = (void*)(nint)slot,
                };
                _api->RegisterNavigator(_api->Ctx, &def);
            }
            finally
            {
                hId.Free();
                hName.Free();
            }
        }

        /// <summary>
        /// Whether the navigator this plugin registered as <paramref name="id"/> is the one the user has
        /// active in the footer's Step selector. Use it to gate the plugin's stepping UI (e.g. grey out
        /// settings until the user selects the navigator). The host resolves <paramref name="id"/> the same
        /// way <see cref="RegisterMode"/> does — it prepends the plugin name — so pass the same local id.
        /// </summary>
        public bool IsActive(string id)
        {
            _host.AssertMainThread(nameof(IsActive));
            if (string.IsNullOrEmpty(id)) return false;
            byte[] bytes = Encoding.UTF8.GetBytes(id + "\0");
            fixed (byte* p = bytes) return _api->IsNavigatorActive(_api->Ctx, p) == 1;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static int StepTrampoline(void* ud, OfsNavIntent* step, OfsNavIntent* outp)
        {
            var slot = GetSlot((int)(nint)ud);
            if (slot?.OnStep == null || step == null || outp == null) return (int)OfsNavResult.None; // released → no move
            var s = new NavStep(step->Direction, step->Reps, (NavGranularity)step->Granularity);
            Nav nav = PluginGuard.Run("navigator:onStep", () => slot.OnStep(s), Nav.None);
            if (nav.IsPass) return (int)OfsNavResult.Pass;
            if (!nav.HasSeek) return (int)OfsNavResult.None;
            outp->Time = nav.Time;
            if (nav.HasAxis) outp->Axis = (int)nav.Axis; // else keep the host default (current active axis)
            return (int)OfsNavResult.Seek;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void EnterTrampoline(void* ud)
        {
            var slot = GetSlot((int)(nint)ud);
            if (slot?.OnEnter != null) PluginGuard.Run("navigator:onEnter", slot.OnEnter);
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void ExitTrampoline(void* ud)
        {
            var slot = GetSlot((int)(nint)ud);
            if (slot?.OnExit != null) PluginGuard.Run("navigator:onExit", slot.OnExit);
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void UiTrampoline(void* ud)
        {
            var slot = GetSlot((int)(nint)ud);
            if (slot?.OnUi == null || slot.Ui == null) return; // released → nothing to draw
            Action<Ui> onUi = slot.OnUi;
            Ui ui = slot.Ui;
            ui.BeginPass();
            try { PluginGuard.Run("navigator:onUi", () => onUi(ui)); }
            finally { ui.EndPass(); }
        }
    }
}
