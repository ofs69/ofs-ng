using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace Ofs
{
    /// <summary>
    /// The set of funscript axes. Reads are cheap and direct through the indexer; writes go
    /// through a buffered <see cref="Axis.Edit"/> finished with <see cref="AxisEdit.Commit"/>.
    /// </summary>
    public sealed unsafe class Axes
    {
        private readonly OfsHost _host;
        private readonly HostApi* _api;
        private readonly Dictionary<StandardAxis, Axis> _cache = new();
        private ScriptAction[] _scratch = new ScriptAction[4096];

        internal Axes(OfsHost host, HostApi* api)
        {
            _host = host;
            _api = api;
        }

        /// <summary>A read view of the axis, refreshed with the host's current actions and
        /// selection. Valid for the current frame — read fresh each frame, do not cache.</summary>
        /// <exception cref="InvalidOperationException">The role does not exist in the project. Plugins can
        /// only access axes that already exist and cannot create one — guard with
        /// <see cref="Existing"/><c>.Contains(role)</c>.</exception>
        public Axis this[StandardAxis role]
        {
            get
            {
                _host.AssertMainThread("Axes[]");
                // Reject an absent axis loudly instead of returning an empty view whose reads come back
                // empty and whose edits silently drop at commit. The host has no API to create an axis, so
                // a role that doesn't exist (AxisState::exists() — shown, holds data, or locked) is a plugin
                // bug. IsAxisVisible returns the host's not-exist sentinel (-1) for an absent role.
                if (_api->IsAxisVisible(_api->Ctx, (int)role) < 0)
                    throw new InvalidOperationException(
                        $"Axis {role} does not exist in the project. Plugins can only access existing axes " +
                        "(guard with Axes.Existing.Contains(role)); they cannot create one.");
                if (!_cache.TryGetValue(role, out var axis))
                    _cache[role] = axis = new Axis(_host, _api, role);

                int actionCount = _api->GetAxisActionCount(_api->Ctx, (int)role);
                EnsureScratch(actionCount);
                fixed (ScriptAction* p = _scratch)
                    _api->GetAxisActions(_api->Ctx, (int)role, p, actionCount);
                axis.SyncActions(_scratch, actionCount);

                int selCount = _api->GetAxisSelectionCount(_api->Ctx, (int)role);
                EnsureScratch(selCount);
                fixed (ScriptAction* p = _scratch)
                    _api->GetAxisSelection(_api->Ctx, (int)role, p, selCount);
                axis.SyncSelection(_scratch, selCount);

                axis.Stamp(_host.FrameGen); // valid for this frame only; later access throws (Axis.CheckFresh)
                return axis;
            }
        }

        /// <summary>The currently focused axis. There is always an active axis (it defaults to L0, and L0
        /// can never be removed or hidden), so this never returns null.</summary>
        public StandardAxis Active
        {
            get
            {
                _host.AssertMainThread(nameof(Active));
                return (StandardAxis)_api->GetActiveAxisRole(_api->Ctx);
            }
        }

        /// <summary>Makes <paramref name="role"/> the active axis.</summary>
        /// <exception cref="InvalidOperationException">The role does not exist in the project — guard with
        /// <see cref="Existing"/><c>.Contains(role)</c>. Plugins cannot create or activate an absent axis.</exception>
        public void SetActive(StandardAxis role)
        {
            _host.AssertMainThread(nameof(SetActive));
            if (_api->IsAxisVisible(_api->Ctx, (int)role) < 0)
                throw new InvalidOperationException(
                    $"Cannot activate axis {role}: it does not exist in the project. Plugins can only " +
                    "activate existing axes (guard with Axes.Existing.Contains(role)).");
            _api->SetActiveAxis(_api->Ctx, (int)role);
        }

        // Cached so repeated polling in OnUpdate doesn't allocate a HashSet per call. Invalidated when the
        // project loads or any axis is modified (a modify can make a scratch axis appear/disappear).
        private IReadOnlySet<StandardAxis>? _existingCache;

        /// <summary>The roles that exist in the project (shown, holding data, or locked). A plugin can only
        /// read or edit an existing axis — indexing or activating an absent role throws, and there is no API
        /// to create one (axis lifecycle is owned by the host/UI). Guard with <c>Existing.Contains(role)</c>
        /// before touching a role that isn't guaranteed (<c>L0</c> always exists; scratch axes may not).</summary>
        public IReadOnlySet<StandardAxis> Existing
        {
            get
            {
                _host.AssertMainThread(nameof(Existing));
                if (_existingCache != null) return _existingCache;
                int count = _api->GetAxisRoles(_api->Ctx, null, 0);
                var set = new HashSet<StandardAxis>(count > 0 ? count : 0);
                if (count > 0)
                {
                    int* buf = stackalloc int[count];
                    _api->GetAxisRoles(_api->Ctx, buf, count);
                    for (int i = 0; i < count; i++)
                        set.Add((StandardAxis)buf[i]);
                }
                return _existingCache = set;
            }
        }

        /// <summary>Raised when a different project is loaded (so cached axis data should be re-read).</summary>
        public event Action? ProjectChanged;
        /// <summary>Raised when an axis's actions change. The argument is the affected role.</summary>
        public event Action<StandardAxis>? Modified;
        /// <summary>Raised when the focused axis changes. The argument is the new active role.</summary>
        public event Action<StandardAxis>? ActiveChanged;

        internal void RaiseProjectChanged() { _existingCache = null; ProjectChanged?.Invoke(); }
        internal void RaiseModified(StandardAxis role) { _existingCache = null; Modified?.Invoke(role); }
        internal void RaiseActiveChanged(StandardAxis role) => ActiveChanged?.Invoke(role);

        // Drop all plugin subscriptions on unload — see OfsHost.DropPluginDelegates.
        internal void ClearSubscribers()
        {
            ProjectChanged = null;
            Modified = null;
            ActiveChanged = null;
        }

        private void EnsureScratch(int count)
        {
            if (count > _scratch.Length)
                _scratch = new ScriptAction[(int)Math.Min((long)count * 2, int.MaxValue)];
        }
    }

    /// <summary>
    /// A read-only snapshot of one axis for the current frame: its sorted actions, selection
    /// and O(log n) lookup helpers. Begin a write with <see cref="Edit"/>.
    /// </summary>
    public sealed unsafe class Axis
    {
        private readonly OfsHost _host;
        private readonly HostApi* _api;

        private readonly List<ScriptAction> _actions = new();
        private readonly List<ScriptAction> _selection = new();
        // Read-only wrappers over the backing lists, handed out instead of the lists themselves so a
        // downcast can't reach in and mutate them behind the host's dirty-tracking/sort invariant. Cached
        // (the wrapped list is re-synced in place each frame), so reads don't allocate.
        private readonly IReadOnlyList<ScriptAction> _actionsView;
        private readonly IReadOnlyList<ScriptAction> _selectionView;
        private int _gen = -1; // frame generation this snapshot was stamped for; -1 = never handed out

        internal Axis(OfsHost host, HostApi* api, StandardAxis role)
        {
            _host = host;
            _api = api;
            Role = role;
            _actionsView = _actions.AsReadOnly();
            _selectionView = _selection.AsReadOnly();
        }

        /// <summary>Which axis this snapshot is of.</summary>
        public StandardAxis Role { get; }
        /// <summary>The axis's actions, sorted by time. Valid for the current frame only.</summary>
        public IReadOnlyList<ScriptAction> Actions { get { CheckFresh(); return _actionsView; } }
        /// <summary>The currently selected actions, sorted by time. Valid for the current frame only.</summary>
        public IReadOnlyList<ScriptAction> Selection { get { CheckFresh(); return _selectionView; } }

        /// <summary>Whether the axis is shown in the timeline. Valid for the current frame only.</summary>
        public bool IsVisible { get { CheckFresh(); return _api->IsAxisVisible(_api->Ctx, (int)Role) == 1; } }
        /// <summary>Whether the axis is locked (UI edits blocked). Valid for the current frame only.</summary>
        public bool IsLocked { get { CheckFresh(); return _api->IsAxisLocked(_api->Ctx, (int)Role) == 1; } }

        /// <summary>The axis display name, e.g. "L0 (Stroke)". Valid for the current frame only.</summary>
        public string Name
        {
            get
            {
                CheckFresh();
                byte* buf = stackalloc byte[128]; // axis names are short and fixed
                _api->GetAxisName(_api->Ctx, (int)Role, buf, 128);
                return Marshal.PtrToStringUTF8((IntPtr)buf) ?? string.Empty;
            }
        }

        internal void Stamp(int gen) => _gen = gen;

        // This Axis instance is cached by Axes and re-synced in place each time Axes[role] is read. Reading
        // it on a later frame would observe a different frame's data — throw rather than return stale rows.
        private void CheckFresh()
        {
            if (_gen != _host.FrameGen)
                throw new InvalidOperationException(
                    "Axis snapshot is from a previous frame. Re-read Axes[role] each frame; do not cache it.");
        }

        internal void SyncActions(ScriptAction[] buf, int count)
        {
            _actions.Clear();
            _actions.AddRange(buf.AsSpan(0, count));
        }

        internal void SyncSelection(ScriptAction[] buf, int count)
        {
            _selection.Clear();
            _selection.AddRange(buf.AsSpan(0, count));
        }

        /// <summary>Nearest action to <paramref name="time"/>; null if empty. Ties break earlier.</summary>
        public ScriptAction? ClosestTo(double time) { CheckFresh(); return AxisMath.ClosestTo(_actions, time); }

        /// <inheritdoc cref="ClosestTo(double)"/>
        public ScriptAction? ClosestTo(ScriptAction action) => ClosestTo(action.At);

        /// <summary>Last action with At strictly less than <paramref name="time"/>; null if none.</summary>
        public ScriptAction? Before(double time) { CheckFresh(); return AxisMath.Before(_actions, time); }

        /// <inheritdoc cref="Before(double)"/>
        public ScriptAction? Before(ScriptAction action) => Before(action.At);

        /// <summary>First action with At strictly greater than <paramref name="time"/>; null if none.</summary>
        public ScriptAction? After(double time) { CheckFresh(); return AxisMath.After(_actions, time); }

        /// <inheritdoc cref="After(double)"/>
        public ScriptAction? After(ScriptAction action) => After(action.At);

        /// <summary>True if an action at <paramref name="time"/> is in the selection.</summary>
        public bool IsSelected(double time)
        {
            CheckFresh();
            return _selection.BinarySearch(new ScriptAction(time, 0)) >= 0;
        }

        /// <inheritdoc cref="IsSelected(double)"/>
        public bool IsSelected(ScriptAction action) => IsSelected(action.At);

        /// <summary>Begins a buffered edit. Mutations apply as one undo step on Commit().
        /// Main-thread only.</summary>
        public AxisEdit Edit()
        {
            _host.AssertMainThread(nameof(Edit));
            CheckFresh();
            return new AxisEdit(_host, _api, Role, _actions, _selection);
        }
    }

    /// <summary>
    /// A buffered, mutable working copy of an axis. Mutators are fluent and accumulate
    /// locally — nothing is applied to the project until <see cref="Commit"/>. One Commit is
    /// one undo step. An uncommitted edit is a clean no-op.
    /// </summary>
    public sealed unsafe class AxisEdit
    {
        private readonly OfsHost _host;
        private readonly HostApi* _api;

        private readonly List<ScriptAction> _actions;
        private readonly List<ScriptAction> _selection;
        private bool _actionsDirty;
        private bool _selectionDirty;
        private bool _committed;
        // Host frame generation when this edit was begun. An AxisEdit buffers a whole-axis snapshot taken
        // at Edit() time; committing it on a LATER frame would replay that stale snapshot over whatever
        // changed in between (user edits, other plugins, async eval results), silently clobbering them
        // (last-writer-wins). So — like the Axis snapshot it came from — an edit is valid only for
        // its own frame, and a cross-frame mutate/commit throws instead of clobbering.
        private readonly int _editGen;

        internal AxisEdit(OfsHost host, HostApi* api, StandardAxis role,
            List<ScriptAction> srcActions, List<ScriptAction> srcSelection)
        {
            _host = host;
            _api = api;
            Role = role;
            _actions = new List<ScriptAction>(srcActions);
            _selection = new List<ScriptAction>(srcSelection);
            _actionsView = _actions.AsReadOnly();
            _editGen = host.FrameGen;
        }

        /// <summary>Which axis this edit targets.</summary>
        public StandardAxis Role { get; }

        // Read-only wrapper over the buffer, handed out instead of the backing list so a downcast can't
        // bypass the mutators (and their sort-order/dirty bookkeeping). Reflects buffered mutations in place.
        private readonly IReadOnlyList<ScriptAction> _actionsView;

        /// <summary>The buffered actions, reflecting mutations made on this edit so far (pre-Commit).</summary>
        public IReadOnlyList<ScriptAction> Actions => _actionsView;
        /// <summary>Last buffered action with At strictly less than <paramref name="time"/>; null if none.</summary>
        public ScriptAction? Before(double time) => AxisMath.Before(_actions, time);
        /// <inheritdoc cref="Before(double)"/>
        public ScriptAction? Before(ScriptAction action) => Before(action.At);
        /// <summary>First buffered action with At strictly greater than <paramref name="time"/>; null if none.</summary>
        public ScriptAction? After(double time) => AxisMath.After(_actions, time);
        /// <inheritdoc cref="After(double)"/>
        public ScriptAction? After(ScriptAction action) => After(action.At);

        // Guards every mutator and Commit. Two ways an edit becomes unusable, both of which would
        // otherwise corrupt silently:
        //   • already committed — the local lists can never reach the project again (a second Commit
        //     throws), so a further mutation would just vanish.
        //   • begun on an earlier frame — committing its buffered whole-axis snapshot now would clobber
        //     everything that changed since (see _editGen).
        // Fail loud in both cases; the author must begin a fresh Edit() on the frame they commit.
        private void EnsureUsable(string op)
        {
            _host.AssertMainThread(op); // every mutator + Commit routes through here, so the whole edit is main-thread-guarded
            if (_committed)
                throw new InvalidOperationException(
                    $"AxisEdit.{op} called after Commit(); the edit is spent. Begin a new Edit().");
            if (_host.FrameGen != _editGen)
                throw new InvalidOperationException(
                    $"AxisEdit.{op} on an edit begun on a previous frame; an AxisEdit is valid only for the " +
                    "frame it was created (it buffers a whole-axis snapshot that a later commit would replay " +
                    "over intervening changes). Re-read Axes[role] and Edit() on the frame you commit.");
        }

        // --- Buffered action mutation (fluent) ---

        /// <summary>Insert the action, replacing any existing one at the same time — actions are unique by
        /// time, so the axis behaves like the core <c>VectorSet</c> keyed on <see cref="ScriptAction.At"/>.</summary>
        public AxisEdit Add(double at, int pos)
        {
            EnsureUsable(nameof(Add));
            UpsertAction(new ScriptAction(at, pos));
            _actionsDirty = true;
            return this;
        }

        /// <inheritdoc cref="Add(double, int)"/>
        public AxisEdit Add(ScriptAction action) => Add(action.At, action.Pos);

        /// <summary>Inserts every action, each replacing any existing one at its time. One buffered mutation
        /// regardless of count — cheaper and terser than repeated <see cref="Add(double, int)"/>.</summary>
        public AxisEdit AddRange(IEnumerable<ScriptAction> actions)
        {
            EnsureUsable(nameof(AddRange));
            foreach (var a in actions)
                UpsertAction(a);
            _actionsDirty = true;
            return this;
        }

        private void UpsertAction(ScriptAction action)
        {
            int idx = _actions.BinarySearch(action);
            if (idx >= 0) _actions[idx] = action;
            else _actions.Insert(~idx, action);
        }

        /// <summary>Removes the action at <paramref name="at"/>, if any.</summary>
        public AxisEdit RemoveAt(double at)
        {
            EnsureUsable(nameof(RemoveAt));
            int idx = _actions.BinarySearch(new ScriptAction(at, 0));
            if (idx >= 0) { _actions.RemoveAt(idx); _actionsDirty = true; }
            return this;
        }

        /// <inheritdoc cref="RemoveAt(double)"/>
        public AxisEdit RemoveAt(ScriptAction action) => RemoveAt(action.At);

        /// <summary>Removes every action with <c>start &lt;= At &lt;= end</c>.</summary>
        public AxisEdit RemoveRange(double start, double end)
        {
            EnsureUsable(nameof(RemoveRange));
            int removed = _actions.RemoveAll(a => a.At >= start && a.At <= end);
            if (removed > 0) _actionsDirty = true;
            return this;
        }

        /// <summary>Removes every action on the axis.</summary>
        public AxisEdit Clear()
        {
            EnsureUsable(nameof(Clear));
            if (_actions.Count > 0) { _actions.Clear(); _actionsDirty = true; }
            return this;
        }

        // --- Buffered selection mutation ---

        /// <summary>Adds the action at <paramref name="at"/> to the selection.</summary>
        public AxisEdit Select(double at)
        {
            EnsureUsable(nameof(Select));
            SelectAt(at);
            return this;
        }

        /// <inheritdoc cref="Select(double)"/>
        public AxisEdit Select(ScriptAction action) => Select(action.At);

        /// <summary>Adds every action with <c>start &lt;= At &lt;= end</c> to the selection, in one buffered
        /// mutation. Times with no action are unaffected — selection keys on the axis's real actions.</summary>
        public AxisEdit SelectRange(double start, double end)
        {
            EnsureUsable(nameof(SelectRange));
            foreach (var a in _actions) // _actions is sorted by time: skip up to start, stop past end
            {
                if (a.At < start) continue;
                if (a.At > end) break;
                SelectAt(a.At);
            }
            return this;
        }

        private void SelectAt(double at)
        {
            var key = new ScriptAction(at, 0);
            int idx = _selection.BinarySearch(key);
            if (idx < 0) { _selection.Insert(~idx, key); _selectionDirty = true; }
        }

        /// <summary>Removes the action at <paramref name="at"/> from the selection.</summary>
        public AxisEdit Deselect(double at)
        {
            EnsureUsable(nameof(Deselect));
            int idx = _selection.BinarySearch(new ScriptAction(at, 0));
            if (idx >= 0) { _selection.RemoveAt(idx); _selectionDirty = true; }
            return this;
        }

        /// <inheritdoc cref="Deselect(double)"/>
        public AxisEdit Deselect(ScriptAction action) => Deselect(action.At);

        /// <summary>Removes every selected action with <c>start &lt;= At &lt;= end</c> from the selection,
        /// in one buffered mutation. The actions themselves are kept.</summary>
        public AxisEdit DeselectRange(double start, double end)
        {
            EnsureUsable(nameof(DeselectRange));
            int removed = _selection.RemoveAll(a => a.At >= start && a.At <= end);
            if (removed > 0) _selectionDirty = true;
            return this;
        }

        /// <summary>Clears the selection (the actions themselves are kept).</summary>
        public AxisEdit ClearSelection()
        {
            EnsureUsable(nameof(ClearSelection));
            if (_selection.Count > 0) { _selection.Clear(); _selectionDirty = true; }
            return this;
        }

        /// <summary>Applies every buffered mutation as ONE undo step. No-op if nothing changed.
        /// The edit is spent afterwards.</summary>
        public void Commit()
        {
            EnsureUsable(nameof(Commit)); // asserts main thread + spent/stale-frame guards
            _committed = true;

            if (_actionsDirty)
            {
                var span = CollectionsMarshal.AsSpan(_actions);
                fixed (ScriptAction* p = span)
                    _api->CommitAxisActions(_api->Ctx, (int)Role, p, span.Length);
            }

            if (_selectionDirty)
            {
                if (_selection.Count == 0)
                {
                    _api->ClearAxisSelection(_api->Ctx, (int)Role);
                }
                else
                {
                    var span = CollectionsMarshal.AsSpan(_selection);
                    fixed (ScriptAction* p = span)
                        _api->SetAxisSelection(_api->Ctx, (int)Role, p, span.Length);
                }
            }
        }
    }

    // Shared sorted-list lookups used by both Axis and AxisEdit.
    internal static class AxisMath
    {
        public static ScriptAction? ClosestTo(List<ScriptAction> a, double time)
        {
            if (a.Count == 0) return null;
            int idx = a.BinarySearch(new ScriptAction(time, 0));
            if (idx >= 0) return a[idx];
            int ins = ~idx;
            if (ins == 0) return a[0];
            if (ins == a.Count) return a[^1];
            var before = a[ins - 1];
            var after = a[ins];
            return (time - before.At) <= (after.At - time) ? before : after;
        }

        public static ScriptAction? Before(List<ScriptAction> a, double time)
        {
            int idx = a.BinarySearch(new ScriptAction(time, 0));
            int ins = idx >= 0 ? idx : ~idx;
            return ins > 0 ? a[ins - 1] : null;
        }

        public static ScriptAction? After(List<ScriptAction> a, double time)
        {
            int idx = a.BinarySearch(new ScriptAction(time, 0));
            int ins = idx >= 0 ? idx + 1 : ~idx;
            return ins < a.Count ? a[ins] : null;
        }
    }
}
