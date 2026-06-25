using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Ofs
{
    /// <summary>The kind of an <see cref="EditIntent"/> — what authoring gesture the user made.
    /// Mirrors the host's edit-intent vocabulary.</summary>
    // Values are the ABI contract: they match native OfsEditIntentKind / EditIntentKind (the router maps
    // by value). Explicit so a reorder can't silently remap gestures once third-party plugins compile.
    public enum EditIntentKind
    {
        /// <summary>Add a point at a clicked time (<see cref="EditIntent.Time"/>, <see cref="EditIntent.Pos"/>).</summary>
        AddPoint = 0,
        /// <summary>Add a point at the playhead on the active axis (<see cref="EditIntent.Pos"/>).</summary>
        AddPointAtPlayhead = 1,
        /// <summary>Move a point from <see cref="EditIntent.FromTime"/> to <see cref="EditIntent.Time"/>/<see cref="EditIntent.Pos"/>.</summary>
        MovePoint = 2,
        /// <summary>Remove the point at <see cref="EditIntent.Time"/>.</summary>
        RemovePoint = 3,
        /// <summary>Remove the current selection.</summary>
        RemoveSelected = 4,
        /// <summary>Nudge the selection: in time by <see cref="EditIntent.Direction"/> × <see cref="EditIntent.Reps"/>
        /// when <see cref="EditIntent.Direction"/> is not <see cref="StepDirection.None"/>, otherwise in position by <see cref="EditIntent.Pos"/>
        /// (a delta). A nudge that moves a <i>single</i> action is delivered as <see cref="MovePoint"/> instead, so a
        /// mode handling only MovePoint covers single-action keyboard moves too; only a multi-action nudge arrives here.</summary>
        MoveSelection = 5,
        /// <summary>Paste the clipboard at <see cref="EditIntent.Time"/> (<see cref="EditIntent.Exact"/> keeps original times).</summary>
        Paste = 6,
    }

    /// <summary>
    /// A single edit gesture an active edit mode observes and may transform. The host hands one of these
    /// to <c>onIntent</c>; only the fields named for <see cref="Kind"/> are meaningful. To replace a
    /// gesture, build new intents with the static factory methods and return
    /// <see cref="EditResult.Replace(EditIntent[])"/>.
    /// </summary>
    public readonly struct EditIntent
    {
        /// <summary>Which gesture this intent represents; it determines which other fields are meaningful.</summary>
        public EditIntentKind Kind { get; }
        /// <summary>The axis the gesture targets.</summary>
        public StandardAxis Axis { get; }
        /// <summary>AddPoint/RemovePoint: target time. MovePoint: destination time. Paste: paste time.</summary>
        public double Time { get; }
        /// <summary>MovePoint: the point's original time.</summary>
        public double FromTime { get; }
        /// <summary>AddPoint/AddPointAtPlayhead: position. MovePoint: destination position. MoveSelection: position delta.</summary>
        public int Pos { get; }
        /// <summary>MoveSelection: <see cref="StepDirection.Backward"/> / <see cref="StepDirection.Forward"/> for a
        /// time nudge; <see cref="StepDirection.None"/> for a position nudge.</summary>
        public StepDirection Direction { get; }
        /// <summary>MoveSelection (time nudge): held-repeat burst count (≥ 1).</summary>
        public int Reps { get; }
        /// <summary>Paste: paste at the clipboard's original times rather than relative to <see cref="Time"/>.</summary>
        public bool Exact { get; }
        /// <summary>MoveSelection (time nudge): seek the playhead to the moved selection afterward.</summary>
        public bool SeekAfter { get; }

        internal EditIntent(EditIntentKind kind, StandardAxis axis, double time, double fromTime, int pos,
            StepDirection direction, int reps, bool exact, bool seekAfter)
        {
            Kind = kind; Axis = axis; Time = time; FromTime = fromTime; Pos = pos;
            Direction = direction; Reps = reps; Exact = exact; SeekAfter = seekAfter;
        }

        /// <summary>Add an action at <paramref name="time"/> with position <paramref name="pos"/> on <paramref name="axis"/>.</summary>
        public static EditIntent AddPoint(StandardAxis axis, double time, int pos)
            => new(EditIntentKind.AddPoint, axis, time, 0, pos, StepDirection.None, 1, false, false);

        /// <inheritdoc cref="AddPoint(StandardAxis, double, int)"/>
        public static EditIntent AddPoint(StandardAxis axis, ScriptAction action)
            => AddPoint(axis, action.At, action.Pos);

        /// <summary>Add an action at the playhead with position <paramref name="pos"/> on the active axis.</summary>
        public static EditIntent AddPointAtPlayhead(int pos)
            => new(EditIntentKind.AddPointAtPlayhead, StandardAxis.L0, 0, 0, pos, StepDirection.None, 1, false, false);

        /// <summary>Move the action at <paramref name="fromTime"/> to (<paramref name="toTime"/>, <paramref name="toPos"/>).</summary>
        public static EditIntent MovePoint(StandardAxis axis, double fromTime, double toTime, int toPos)
            => new(EditIntentKind.MovePoint, axis, toTime, fromTime, toPos, StepDirection.None, 1, false, false);

        /// <summary>Move the action at <paramref name="fromTime"/> to <paramref name="to"/>.</summary>
        public static EditIntent MovePoint(StandardAxis axis, double fromTime, ScriptAction to)
            => MovePoint(axis, fromTime, to.At, to.Pos);

        /// <summary>Remove the action at <paramref name="time"/> on <paramref name="axis"/>.</summary>
        public static EditIntent RemovePoint(StandardAxis axis, double time)
            => new(EditIntentKind.RemovePoint, axis, time, 0, 0, StepDirection.None, 1, false, false);

        /// <inheritdoc cref="RemovePoint(StandardAxis, double)"/>
        public static EditIntent RemovePoint(StandardAxis axis, ScriptAction action)
            => RemovePoint(axis, action.At);

        /// <summary>Remove the current selection on <paramref name="axis"/>.</summary>
        public static EditIntent RemoveSelected(StandardAxis axis)
            => new(EditIntentKind.RemoveSelected, axis, 0, 0, 0, StepDirection.None, 1, false, false);

        /// <summary>A position nudge of the selection by <paramref name="delta"/>.</summary>
        public static EditIntent MoveSelectionByPos(StandardAxis axis, int delta)
            => new(EditIntentKind.MoveSelection, axis, 0, 0, delta, StepDirection.None, 1, false, false);

        /// <summary>A time nudge of the selection by <paramref name="direction"/> × <paramref name="reps"/>.</summary>
        public static EditIntent MoveSelectionByTime(StandardAxis axis, StepDirection direction, int reps = 1, bool seekAfter = false)
            => new(EditIntentKind.MoveSelection, axis, 0, 0, 0,
                direction == StepDirection.Backward ? StepDirection.Backward : StepDirection.Forward,
                Math.Max(1, reps), false, seekAfter);

        /// <summary>Paste the clipboard at <paramref name="time"/>; <paramref name="exact"/> keeps the
        /// clipboard's original times rather than offsetting relative to <paramref name="time"/>.</summary>
        public static EditIntent Paste(double time, bool exact = false)
            => new(EditIntentKind.Paste, StandardAxis.L0, time, 0, 0, StepDirection.None, 1, exact, false);

        /// <inheritdoc cref="Paste(double, bool)"/>
        public static EditIntent Paste(ScriptAction action, bool exact = false)
            => Paste(action.At, exact);
    }

    /// <summary>What an edit mode decides for an intent: apply it unchanged, drop it, or replace it with
    /// other intents.</summary>
    public readonly struct EditResult
    {
        internal OfsEditDisposition Disposition { get; }
        internal IReadOnlyList<EditIntent>? Replacements { get; }

        private EditResult(OfsEditDisposition d, IReadOnlyList<EditIntent>? r) { Disposition = d; Replacements = r; }

        /// <summary>Apply the intent unchanged (native behavior).</summary>
        public static EditResult Pass => new(OfsEditDisposition.Pass, null);

        /// <summary>Discard the intent; nothing is applied.</summary>
        public static EditResult Drop => new(OfsEditDisposition.Drop, null);

        /// <summary>Discard the original; apply the given intents instead (none ≡ <see cref="Drop"/>).
        /// The intents are <b>projected</b> across the active edit group below the seam — a lead-axis
        /// mutation fans to followers as a mechanical offset (the common case: drags, nudges).</summary>
        /// <remarks>You may emit several intents — including several for the same axis. The host applies them
        /// <b>in the order given</b>, each against the state the previous one left (no dedup or merge), and
        /// coalesces the whole batch into a <b>single undo step</b>. Because each intent sees the prior one's
        /// result, chain by the moved-to coordinate: a <c>MovePoint(from: 2.0, …)</c> after an intent that
        /// moved an action to 2.0 retargets that just-moved action.</remarks>
        /// <example>Turn one click into three points on the gestured axis (one undo step):
        /// <code>
        /// EditIntentKind.AddPoint => EditResult.Replace(
        ///     EditIntent.AddPoint(intent.Axis, intent.Time, 10),
        ///     EditIntent.AddPoint(intent.Axis, intent.Time + 1.0, 20),
        ///     EditIntent.AddPoint(intent.Axis, intent.Time + 2.0, 30)),
        /// </code></example>
        public static EditResult Replace(params EditIntent[] intents) => new(OfsEditDisposition.Replace, intents);

        /// <summary>Discard the original; apply the given intents instead.</summary>
        public static EditResult Replace(IEnumerable<EditIntent> intents)
            => new(OfsEditDisposition.Replace, new List<EditIntent>(intents));

        /// <summary>Like <see cref="Replace(EditIntent[])"/>, but the host applies these intents to the
        /// lead axis alone and then <b>re-consults this mode once per other editable axis</b> (the request
        /// retargeted to that axis), applying each result verbatim — no projection. Use it when "the same
        /// gesture" means something computed from each axis's own data (e.g. snap to per-axis grids). The
        /// per-axis re-consultations are leaves: a <see cref="ReplacePerAxis(EditIntent[])"/> returned from
        /// one degrades to a plain <see cref="Replace(EditIntent[])"/> (one level deep). For a gesture that
        /// edits an existing action in place (a move or a remove), a follower axis that has no action at the
        /// lead's source time is dropped, not re-consulted — its actions don't line up with the lead, so the
        /// retargeted intent would name nothing on it (the same axis the host skips in native projection).</summary>
        public static EditResult ReplacePerAxis(params EditIntent[] intents)
            => new(OfsEditDisposition.ReplacePerAxis, intents);

        /// <summary>Per-axis replacement (see <see cref="ReplacePerAxis(EditIntent[])"/>).</summary>
        public static EditResult ReplacePerAxis(IEnumerable<EditIntent> intents)
            => new(OfsEditDisposition.ReplacePerAxis, new List<EditIntent>(intents));
    }

    /// <summary>Handles one edit intent for an active mode. Runs on the main thread.</summary>
    public delegate EditResult EditIntentHandler(EditIntent intent);

    /// <summary>
    /// Registration of alternate edit modes. A mode owns how the user's editing gestures resolve — it can
    /// observe, drop, or replace each <see cref="EditIntent"/>. Registering only publishes the mode; the
    /// <b>user</b> activates it from the footer (there is no plugin-callable setter). Call
    /// <see cref="RegisterMode"/> from <see cref="OfsPlugin.OnLoad"/>.
    /// </summary>
    /// <remarks>The <c>onIntent</c> callback runs on the main thread per gesture. It must return quickly.</remarks>
    public sealed unsafe class Editing
    {
        private readonly OfsHost _host;
        private readonly HostApi* _api;

        internal Editing(OfsHost host, HostApi* api)
        {
            _host = host;
            _api = api;
        }

        /// <summary>
        /// Seconds one action move-step travels under the active overlay (a frame interval, or a tempo
        /// beat) — the distance a left/right time nudge advances per rep. The host already applies it when
        /// delivering a single-action nudge as a <see cref="EditIntentKind.MovePoint"/>; a mode handling a
        /// multi-action <see cref="EditIntentKind.MoveSelection"/> multiplies it by direction × reps itself.
        /// </summary>
        public double StepTime
        {
            get
            {
                _host.AssertMainThread(nameof(StepTime));
                return _api->GetMoveStepTime(_api->Ctx);
            }
        }

        // One registered mode's managed callbacks, reached by slot index (the def's UserData). Static so
        // the [UnmanagedCallersOnly] trampolines (which have no instance) can read it; a released slot is
        // nulled on unload and the trampolines treat null as native (Pass). Slots only ever grow.
        private sealed class ModeSlot
        {
            public EditIntentHandler OnIntent = null!;
            public Action? OnEnter;
            public Action? OnExit;
            public Action<Ui>? OnUi;
            public Ui? Ui; // built once at registration over the host api; drawn only while the mode is active
        }

        private static readonly object s_lock = new();
        private static readonly List<ModeSlot?> s_slots = new();
        private readonly List<int> _ownedSlots = new();

        private static ModeSlot? GetSlot(int i)
        {
            lock (s_lock) { return (i >= 0 && i < s_slots.Count) ? s_slots[i] : null; }
        }

        // Null this plugin's slots on unload so its delegates (and thus its collectible ALC) are released;
        // the native registry entries are dropped in parallel by UnregisterEditModesEvent.
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
        /// Publishes an edit mode the user can select in the footer. Call from OnLoad.
        /// </summary>
        /// <param name="id">Local id (no namespace prefix); the host prepends the plugin name.</param>
        /// <param name="displayName">Name shown in the footer's edit-mode selector (rendered verbatim).</param>
        /// <param name="onIntent">Resolves each edit gesture: Pass / Drop / Replace.</param>
        /// <param name="onEnter">Optional: runs when the user activates this mode.</param>
        /// <param name="onExit">Optional: runs when the user leaves this mode.</param>
        /// <param name="onUi">Optional: draws this mode's options. The host calls it (with a <see cref="Ui"/>
        /// builder) only while this mode is the active one, in the docked Tool Options panel — so you no
        /// longer hand-gate the options on <see cref="IsActive"/>. Read/write your own backing fields in it,
        /// exactly as in <see cref="OfsPlugin.OnRenderUi"/>. Null = the mode contributes no options section.</param>
        public void RegisterMode(string id, string displayName, EditIntentHandler onIntent,
            Action? onEnter = null, Action? onExit = null, Action<Ui>? onUi = null)
        {
            _host.AssertMainThread(nameof(RegisterMode));
            _host.AssertOnLoad(nameof(RegisterMode));
            if (string.IsNullOrEmpty(id)) throw new ArgumentException("Edit mode id must be non-empty.", nameof(id));
            ArgumentNullException.ThrowIfNull(onIntent);

            int slot;
            lock (s_lock)
            {
                slot = s_slots.Count;
                s_slots.Add(new ModeSlot
                {
                    OnIntent = onIntent,
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
                var def = new OfsEditModeDef
                {
                    Id = (byte*)hId.AddrOfPinnedObject(),
                    DisplayName = (byte*)hName.AddrOfPinnedObject(),
                    OnEditIntent = (IntPtr)(delegate* unmanaged[Cdecl]<void*, OfsEditIntent*,
                        delegate* unmanaged[Cdecl]<void*, OfsEditIntent*, void>, void*, int>)&IntentTrampoline,
                    OnEnter = onEnter != null ? (IntPtr)(delegate* unmanaged[Cdecl]<void*, void>)&EnterTrampoline : default,
                    OnExit = onExit != null ? (IntPtr)(delegate* unmanaged[Cdecl]<void*, void>)&ExitTrampoline : default,
                    OnUi = onUi != null ? (IntPtr)(delegate* unmanaged[Cdecl]<void*, void>)&UiTrampoline : default,
                    UserData = (void*)(nint)slot,
                };
                _api->RegisterEditMode(_api->Ctx, &def);
            }
            finally
            {
                hId.Free();
                hName.Free();
            }
        }

        /// <summary>
        /// Whether the edit mode this plugin registered as <paramref name="id"/> is the one the user has
        /// active in the footer. Use it to gate the plugin's editing UI (e.g. grey out settings until the
        /// user selects the mode). The host resolves <paramref name="id"/> the same way
        /// <see cref="RegisterMode"/> does — it prepends the plugin name — so pass the same local id.
        /// </summary>
        public bool IsActive(string id)
        {
            _host.AssertMainThread(nameof(IsActive));
            if (string.IsNullOrEmpty(id)) return false;
            byte[] bytes = Encoding.UTF8.GetBytes(id + "\0");
            fixed (byte* p = bytes) return _api->IsEditModeActive(_api->Ctx, p) == 1;
        }

        // ── ABI marshaling (OfsEditIntent mirrors EditIntent field-for-field — a plain copy) ──

        internal static EditIntent FromAbi(in OfsEditIntent a)
        {
            var kind = (EditIntentKind)a.Kind;
            var axis = (a.Axis >= 0 && a.Axis < (int)StandardAxis.Count) ? (StandardAxis)a.Axis : StandardAxis.L0;
            return new EditIntent(kind, axis, a.Time, a.FromTime, a.Pos,
                (StepDirection)a.Direction, Math.Max(1, a.Reps), a.Exact != 0, a.SeekAfter != 0);
        }

        internal static OfsEditIntent ToAbi(in EditIntent i)
            => new OfsEditIntent
            {
                Kind = (int)i.Kind,
                Axis = (int)i.Axis,
                Time = i.Time,
                FromTime = i.FromTime,
                Pos = i.Pos,
                Direction = (int)i.Direction,
                Reps = Math.Max(1, i.Reps),
                Exact = i.Exact ? 1 : 0,
                SeekAfter = i.SeekAfter ? 1 : 0,
            };

        // ── Trampolines (static; survive plugin unload — they live in the shared Ofs.Api) ──

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static int IntentTrampoline(void* ud, OfsEditIntent* inp,
            delegate* unmanaged[Cdecl]<void*, OfsEditIntent*, void> emit, void* sink)
        {
            var slot = GetSlot((int)(nint)ud);
            if (slot?.OnIntent == null || inp == null) return (int)OfsEditDisposition.Pass; // released → native
            EditIntent intent = FromAbi(*inp);
            EditResult result = PluginGuard.Run("editmode:onIntent", () => slot.OnIntent(intent), EditResult.Pass);
            bool replaces = result.Disposition == OfsEditDisposition.Replace
                            || result.Disposition == OfsEditDisposition.ReplacePerAxis;
            if (replaces && result.Replacements != null && emit != null)
                foreach (var r in result.Replacements)
                {
                    OfsEditIntent o = ToAbi(r);
                    emit(sink, &o);
                }
            return (int)result.Disposition;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void EnterTrampoline(void* ud)
        {
            var slot = GetSlot((int)(nint)ud);
            if (slot?.OnEnter != null) PluginGuard.Run("editmode:onEnter", slot.OnEnter);
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void ExitTrampoline(void* ud)
        {
            var slot = GetSlot((int)(nint)ud);
            if (slot?.OnExit != null) PluginGuard.Run("editmode:onExit", slot.OnExit);
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void UiTrampoline(void* ud)
        {
            var slot = GetSlot((int)(nint)ud);
            if (slot?.OnUi == null || slot.Ui == null) return; // released → nothing to draw
            Action<Ui> onUi = slot.OnUi;
            Ui ui = slot.Ui;
            ui.BeginPass();
            try { PluginGuard.Run("editmode:onUi", () => onUi(ui)); }
            finally { ui.EndPass(); }
        }
    }
}
