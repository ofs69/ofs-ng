using System;
using System.Collections;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.Json.Serialization.Metadata;

namespace Ofs
{
    // ── Plugin-node shape + eval delegates ──────────────────
    // A node declares N input pins and M output pins via NodeShape. One delegate shape per signal kind
    // serves any (N, M): eval reads the input span and writes the output span. A node's parameters are
    // the fields of an author-defined value type (TState), taken by `in` (a private read-only copy).

    /// <summary>A node's pin shape: the input and output pin names. Each array's length is that
    /// direction's pin count. Pin names double as labels (and, for scripts, injected locals). Construct
    /// with named pins, e.g. <c>new NodeShape(inputs: ["a", "b"], outputs: ["mix"])</c>.</summary>
    public readonly struct NodeShape
    {
        /// <summary>Input pin names; length 0..16.</summary>
        public IReadOnlyList<string> Inputs { get; }
        /// <summary>Output pin names; length 1..16.</summary>
        public IReadOnlyList<string> Outputs { get; }

        /// <summary>Build a pin shape. Throws if the counts are out of range (≤16 inputs, 1..16 outputs)
        /// or if a name repeats within a direction (pin names double as link-target labels and injected
        /// script locals, so a direction's names must be distinct). The arrays are cloned, so the shape is
        /// fully immutable once built.</summary>
        public NodeShape(string[] inputs, string[] outputs)
        {
            var ins = inputs is { Length: > 0 } ? (string[])inputs.Clone() : Array.Empty<string>();
            var outs = outputs is { Length: > 0 } ? (string[])outputs.Clone() : Array.Empty<string>();
            if (ins.Length > 16)
                throw new ArgumentException("a node may declare at most 16 inputs", nameof(inputs));
            if (outs.Length < 1 || outs.Length > 16)
                throw new ArgumentException("a node must declare 1..16 outputs", nameof(outputs));
            RejectDuplicateNames(ins, nameof(inputs), "input");
            RejectDuplicateNames(outs, nameof(outputs), "output");
            Inputs = ins;
            Outputs = outs;
        }

        // Pin names are distinct within a direction (a name with two source pins, or two locals injected
        // under the same identifier into a script, is ambiguous). Duplicates across directions are fine.
        private static void RejectDuplicateNames(string[] names, string paramName, string kind)
        {
            var seen = new HashSet<string>(StringComparer.Ordinal);
            foreach (string name in names)
                if (!seen.Add(name))
                    throw new ArgumentException($"duplicate {kind} pin name '{name}'", paramName);
        }
    }

    /// <summary>The glyph a node shows in the add-node palette and on its title bar — a curated subset of
    /// the host's icons, chosen by value so a plugin never needs the host font's codepoints. Pass one to
    /// <see cref="Nodes.AddNode{TState}(string, string, NodeShape, EvalFunctional{TState}, NodeUi{TState}, string, NodeIcon, string)"/>.
    /// <see cref="Default"/> lets the host pick by arity (Generate / Modify / Combine). The list is
    /// append-only; an older host that doesn't know a newer value falls back to <see cref="Default"/>.</summary>
    public enum NodeIcon
    {
        /// <summary>Host picks by arity (Generate / Modify / Combine). The default.</summary>
        Default = 0,
        Waveform = 1,
        Sliders = 2,
        Filter = 3,
        Curve = 4,
        Activity = 5,
        Gauge = 6,
        Math = 7,
        Function = 8,
        Merge = 9,
        Split = 10,
        Blend = 11,
        Combine = 12,
        Scale = 13,
        Ruler = 14,
        Magnet = 15,
        Percent = 16,
        Repeat = 17,
        Shuffle = 18,
        Random = 19,
        Wand = 20,
        Trend = 21,
        Zap = 22,
        Bars = 23,
        Move = 24,
    }

    /// <summary>Functional eval (worker thread): given time <c>t</c>, read the N input values in
    /// <paramref name="ins"/> and write the M output values (0..100) into <paramref name="outs"/>.</summary>
    public delegate void EvalFunctional<TState>(double t, ReadOnlySpan<float> ins, in TState s, NodeContext ctx,
                                                Span<float> outs);
    /// <summary>Discrete eval (worker thread): read the N input action lists and write the M output action
    /// lists once per region.</summary>
    public delegate void EvalDiscrete<TState>(ReadOnlySpan<DiscreteReader> ins, in TState s, NodeContext ctx,
                                              ReadOnlySpan<DiscreteWriter> outs);

    /// <summary>The per-sample closure a functional factory returns: read the N input values, write the M
    /// output values. Built once per region eval, sampled per output sample.</summary>
    public delegate void FunctionalSample(double t, ReadOnlySpan<float> ins, Span<float> outs);
    /// <summary>Functional factory (worker thread): runs once per region eval and returns the per-sample
    /// closure. Build a non-serializable artifact (LUT, interpolator, loaded dataset) here rather than
    /// rebuilding it every sample.</summary>
    public delegate FunctionalSample PrepareFunctional<TState>(in TState s, NodeContext ctx);

    // ── Stateless eval delegates ────────────────────────────
    // The same three eval shapes with the TState parameter dropped, for a node that carries no params/state.
    // Used by the non-generic Nodes.AddNode overloads: no JSON state is persisted or captured, and the node
    // has no body UI (there is nothing to edit). Pass-through math, min/max combiners, and the like.

    /// <summary>Functional eval (worker thread) for a stateless node: given time <c>t</c>, read the N input
    /// values in <paramref name="ins"/> and write the M output values (0..100) into <paramref name="outs"/>.</summary>
    public delegate void EvalFunctional(double t, ReadOnlySpan<float> ins, NodeContext ctx, Span<float> outs);
    /// <summary>Discrete eval (worker thread) for a stateless node: read the N input action lists and write
    /// the M output action lists once per region.</summary>
    public delegate void EvalDiscrete(ReadOnlySpan<DiscreteReader> ins, NodeContext ctx,
                                      ReadOnlySpan<DiscreteWriter> outs);
    /// <summary>Functional factory (worker thread) for a stateless node: runs once per region eval and
    /// returns the per-sample closure.</summary>
    public delegate FunctionalSample PrepareFunctional(NodeContext ctx);

    /// <summary>The node's body UI (main thread). Draws the node's widgets — sliders/checkboxes for its
    /// <c>TState</c> fields, buttons, file pickers, status labels. <paramref name="s"/> is the live state
    /// by <c>ref</c>: mutate it synchronously, replacing reference fields with new arrays/lists rather than
    /// mutating in place. For a deferred/async write (across an <c>await</c>) grab a capture-safe handle
    /// with <see cref="Ui.Node"/> and call <see cref="Node.Update{TState}"/> when the work completes — a
    /// value type cannot be captured by <c>ref</c> across an await.</summary>
    public delegate void NodeUi<TState>(Ui ui, ref TState s) where TState : struct;

    /// <summary>A deferred mutation applied on the main thread to a node's live state, via <see cref="Node.Update{TState}"/>.</summary>
    public delegate void StateMutator<TState>(ref TState s);

    // Type-erased eval delegates the slot table stores (the boxed TState replaces the typed `in TState`).
    // Spans are ref structs, so these can't be Func/Action generics — they are concrete delegate types.
    internal delegate void FunctionalEvalErased(double t, ReadOnlySpan<float> ins, object? box, NodeContext ctx,
                                                Span<float> outs);
    internal delegate void DiscreteEvalErased(ReadOnlySpan<DiscreteReader> ins, object? box, NodeContext ctx,
                                              ReadOnlySpan<DiscreteWriter> outs);
    internal delegate FunctionalSample PrepareErased(object? box, NodeContext ctx);

    // Script-node delegate shapes (no TState): the compiled OfsScript.Eval binds to one of these.
    internal delegate void ScriptFunctionalEval(double t, ReadOnlySpan<float> ins, NodeContext ctx, Span<float> outs);
    internal delegate void ScriptDiscreteEval(ReadOnlySpan<DiscreteReader> ins, NodeContext ctx,
                                              ReadOnlySpan<DiscreteWriter> outs);

    /// <summary>
    /// A capture-safe handle to a plugin node, obtained from <see cref="Ui.Node"/> inside the node's
    /// <see cref="NodeUi{TState}"/> callback. A <c>TState</c> value cannot be captured by <c>ref</c> across
    /// an <c>await</c>, so async work captures this reference instead and calls <see cref="Update{TState}"/>
    /// when it completes. The mutation is queued and applied on the main thread on the node's next UI pass
    /// — replace reference fields, don't mutate them in place.
    /// </summary>
    public sealed class Node
    {
        private readonly long _key; // (regionId << 32) | nodeId — the host's stable node identity

        internal Node(long key) => _key = key;

        /// <summary>Queue a state mutation, applied on the main thread on this node's next UI pass.
        /// <typeparamref name="TState"/> is inferred from the mutator's parameter.</summary>
        public void Update<TState>(StateMutator<TState> mutator) where TState : struct
        {
            if (mutator == null || _key < 0) return;
            Nodes.EnqueueNodeUpdate(_key, box =>
            {
                TState s = Nodes.Unbox<TState>(box);
                mutator(ref s);
                return s!;
            });
        }
    }

    /// <summary>Evaluation context passed to every node callback. Pure value — safe on any thread.
    /// The params buffer is valid only for the duration of the call; do not store it.</summary>
    public readonly struct NodeContext
    {
        private readonly float[] _params;
        private readonly int _count;
        private readonly int _gen; // generation captured at hand-out; compared against Nodes.CurrentEvalGen

        /// <summary>Start time of the region being evaluated, in seconds.</summary>
        public double RegionStart { get; }
        /// <summary>End time of the region being evaluated, in seconds.</summary>
        public double RegionEnd { get; }

        internal NodeContext(double start, double end, float[] p, int count, int gen)
        {
            RegionStart = start;
            RegionEnd = end;
            _params = p;
            _count = count;
            _gen = gen;
        }

        // The params buffer is per-call [ThreadStatic] scratch, reused by the next node evaluation on this
        // worker thread. Storing a NodeContext beyond its callback and reading it later would silently
        // observe another node's params — throw loudly instead.
        private void CheckFresh()
        {
            if (_gen != Nodes.CurrentEvalGen)
                throw new InvalidOperationException(
                    "NodeContext was stored beyond the node callback that produced it. Its params are " +
                    "per-call scratch — read them inside the callback, do not cache the context.");
        }

        /// <summary>The node parameter values. Valid only during the node callback.</summary>
        public ReadOnlySpan<float> Params { get { CheckFresh(); return _params.AsSpan(0, _count); } }

        /// <summary>Returns param <paramref name="i"/>, or <paramref name="fallback"/> if out of range.</summary>
        public float Param(int i, float fallback = 0f)
        {
            CheckFresh();
            return (uint)i < (uint)_count ? _params[i] : fallback;
        }

        /// <summary>True once the host has cancelled this eval (a newer edit superseded it). A long discrete
        /// generator/modifier should poll this at loop boundaries and return early — the host discards a
        /// cancelled eval's output, so bailing only frees the worker thread sooner. Always false when the
        /// host supplies no cancellation (e.g. a preview eval).</summary>
        public bool IsCancelled { get { CheckFresh(); return Nodes.CurrentEvalCancelled(); } }
    }

    /// <summary>Read-only view of a discrete node input.</summary>
    public readonly struct DiscreteReader : IReadOnlyList<ScriptAction>
    {
        private readonly double[] _times;
        private readonly int[] _positions;
        private readonly int _count;
        private readonly int _gen; // generation captured at hand-out; compared against Nodes.CurrentEvalGen

        internal DiscreteReader(double[] times, int[] positions, int count, int gen)
        {
            _times = times; _positions = positions; _count = count; _gen = gen;
        }

        // Backed by per-call [ThreadStatic] scratch, reused by the next evaluation — see NodeContext.CheckFresh.
        private void CheckFresh()
        {
            if (_gen != Nodes.CurrentEvalGen)
                throw new InvalidOperationException(
                    "DiscreteReader was stored beyond its node callback. Read it inside the callback, do not cache.");
        }

        /// <summary>Number of input actions.</summary>
        public int Count { get { CheckFresh(); return _count; } }
        /// <summary>The input action at index <paramref name="i"/> (actions are ordered by time).</summary>
        public ScriptAction this[int i] { get { CheckFresh(); return new ScriptAction(_times[i], _positions[i]); } }

        /// <summary>Enumerates the input actions in time order.</summary>
        public IEnumerator<ScriptAction> GetEnumerator()
        {
            CheckFresh();
            for (int i = 0; i < _count; i++)
                yield return new ScriptAction(_times[i], _positions[i]);
        }

        IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();
    }

    /// <summary>Write sink for a discrete node callback.</summary>
    public sealed class DiscreteWriter
    {
        internal readonly List<ScriptAction> Actions = new();
        internal void Clear() => Actions.Clear();

        /// <summary>Appends an output action at <paramref name="at"/> seconds with position
        /// <paramref name="pos"/> (the host sorts the output and clamps positions to 0..100).</summary>
        public void Add(double at, int pos) => Actions.Add(new ScriptAction(at, pos));
        /// <summary>Appends an output action (the host sorts the output and clamps positions to 0..100).</summary>
        public void Add(ScriptAction action) => Actions.Add(action);
    }

    /// <summary>
    /// Registration of custom processing nodes. A node declares a <see cref="NodeShape"/> (N named input
    /// pins, M named output pins) and one eval delegate; the signal kind (functional vs discrete) is chosen
    /// by the delegate shape.
    /// </summary>
    /// <remarks>Node callbacks may run on worker threads: they receive only their inputs and
    /// parameters and must not touch Host, the player, or axes.</remarks>
    public sealed unsafe class Nodes
    {
        private readonly OfsHost _host;
        private readonly HostApi* _api;

        // State slots THIS plugin instance registered, tracked so ReleaseOwnedSlots() can clear them when the
        // plugin unloads. Without this the shared slot table (SlotTable<StateSlot>) would hold the plugin's
        // delegates — and thus its AssemblyLoadContext — for the entire process lifetime, so the plugin would
        // never truly unload and its DLL would stay locked. Script nodes (the Append* path) are not tracked
        // here; they are released separately via ReleaseScript.
        private readonly OwnedSlots<StateSlot> _slots = new();

        internal Nodes(OfsHost host, HostApi* api)
        {
            _host = host;
            _api = api;
        }

        // Clear every slot this plugin registered (null the delegate) so its assembly can be collected.
        // OwnedSlots locks the shared table so this doesn't race the script-node Append* path that grows it;
        // the eval trampolines read slots without the lock and null-check, so a released slot is a safe no-op.
        internal void ReleaseOwnedSlots() => _slots.Release();

        // ── Registration ───────────────────
        // The node's parameters are the fields of TState (a struct). eval reads a private value copy by
        // `in`; the host owns one TState per node, persists it as JSON, and value-copies it to the worker.
        //
        // The `name` is the node's display name, rendered verbatim. To localize it, build it from your own
        // catalog at OnLoad (e.g. Str.MyNode): the host reloads every plugin on a UI-language switch, so
        // OnLoad re-runs and re-registers the node in the new language. The node body UI is whatever the
        // optional `ui` callback draws; with no `ui` callback the node shows no body widgets.

        /// <summary>Registers a functional node: its <paramref name="eval"/> reads the input values at each
        /// sampled time and writes the output values, 0..100.</summary>
        /// <typeparam name="TState">A struct whose fields are the node's state and parameters.</typeparam>
        /// <param name="id">Stable id, unique within the plugin; combined with the plugin name as "&lt;plugin&gt;.&lt;id&gt;".</param>
        /// <param name="name">Display name shown in the add-node menu, rendered verbatim.</param>
        /// <param name="shape">The node's input/output pin names and counts.</param>
        /// <param name="eval">Worker-thread compute. Reads a private copy of the state by <c>in</c>.</param>
        /// <param name="ui">Optional body UI drawn on the main thread; omit for a headless node.</param>
        /// <param name="group">Optional add-node menu group header; defaults to the plugin name. Pass the same
        /// string from several nodes (or plugins) to collect them under one heading.</param>
        /// <param name="icon">Optional palette/title-bar glyph; defaults to the node's arity icon.</param>
        /// <param name="description">Optional add-node menu hover tooltip describing the node; omit for none.</param>
        public void AddNode<TState>(string id, string name, NodeShape shape, EvalFunctional<TState> eval,
            NodeUi<TState>? ui = null, string? group = null, NodeIcon icon = NodeIcon.Default,
            string? description = null) where TState : struct
            => RegisterFunctional(id, name, shape, eval, ui, group, icon, description);

        /// <summary>Registers a discrete node: its <paramref name="eval"/> reads the input action lists and
        /// writes the output action lists, once per region.</summary>
        /// <inheritdoc cref="AddNode{TState}(string, string, NodeShape, EvalFunctional{TState}, NodeUi{TState}, string, NodeIcon, string)"/>
        public void AddNode<TState>(string id, string name, NodeShape shape, EvalDiscrete<TState> eval,
            NodeUi<TState>? ui = null, string? group = null, NodeIcon icon = NodeIcon.Default,
            string? description = null) where TState : struct
            => RegisterDiscrete(id, name, shape, eval, ui, group, icon, description);

        /// <summary>Registers a functional node whose <paramref name="prepare"/> factory runs once per region
        /// eval and returns the per-sample closure — build a LUT or other artifact there rather than
        /// rebuilding it every sample.</summary>
        /// <inheritdoc cref="AddNode{TState}(string, string, NodeShape, EvalFunctional{TState}, NodeUi{TState}, string, NodeIcon, string)"/>
        public void AddNode<TState>(string id, string name, NodeShape shape, PrepareFunctional<TState> prepare,
            NodeUi<TState>? ui = null, string? group = null, NodeIcon icon = NodeIcon.Default,
            string? description = null) where TState : struct
            => RegisterFactory(id, name, shape, prepare, ui, group, icon, description);

        /// <summary>Registers a stateless functional node — one with no parameters or persisted state and no
        /// body UI. Its <paramref name="eval"/> reads the input values at each sampled time and writes the
        /// output values, 0..100.</summary>
        /// <param name="id">Stable id, unique within the plugin; combined with the plugin name as "&lt;plugin&gt;.&lt;id&gt;".</param>
        /// <param name="name">Display name shown in the add-node menu, rendered verbatim.</param>
        /// <param name="shape">The node's input/output pin names and counts.</param>
        /// <param name="eval">Worker-thread compute. A stateless node has no <c>TState</c>.</param>
        /// <param name="group">Optional add-node menu group header; defaults to the plugin name.</param>
        /// <param name="icon">Optional palette/title-bar glyph; defaults to the node's arity icon.</param>
        /// <param name="description">Optional add-node menu hover tooltip describing the node; omit for none.</param>
        public void AddNode(string id, string name, NodeShape shape, EvalFunctional eval,
            string? group = null, NodeIcon icon = NodeIcon.Default, string? description = null)
            => RegisterFunctionalStateless(id, name, shape, eval, group, icon, description);

        /// <summary>Registers a stateless discrete node: its <paramref name="eval"/> reads the input action
        /// lists and writes the output action lists, once per region.</summary>
        /// <inheritdoc cref="AddNode(string, string, NodeShape, EvalFunctional, string, NodeIcon, string)"/>
        public void AddNode(string id, string name, NodeShape shape, EvalDiscrete eval,
            string? group = null, NodeIcon icon = NodeIcon.Default, string? description = null)
            => RegisterDiscreteStateless(id, name, shape, eval, group, icon, description);

        /// <summary>Registers a stateless functional node whose <paramref name="prepare"/> factory runs once
        /// per region eval and returns the per-sample closure — build a LUT or other artifact there rather
        /// than rebuilding it every sample.</summary>
        /// <inheritdoc cref="AddNode(string, string, NodeShape, EvalFunctional, string, NodeIcon, string)"/>
        public void AddNode(string id, string name, NodeShape shape, PrepareFunctional prepare,
            string? group = null, NodeIcon icon = NodeIcon.Default, string? description = null)
            => RegisterFactoryStateless(id, name, shape, prepare, group, icon, description);

        // ── Trampoline infrastructure ──────────────────────────────
        //
        // Static slot table indexed by the userData value passed to native. Slots only ever grow; plugin
        // reload nulls a slot in place (ReleaseOwnedSlots) but never shrinks the list — a trampoline that
        // already fetched a delegate completes; a later eval reads null and degrades to a passthrough no-op.

        // Per-thread scratch so BorrowParams never allocates after first use per thread.
        [ThreadStatic] private static float[]? _paramBuf;

        // Per-worker-thread evaluation generation, bumped once at the top of every trampoline and captured
        // by the NodeContext/DiscreteReader handed to the callback. Reading those views after the callback
        // returns (when the [ThreadStatic] scratch has been reused) then fails CheckFresh loudly. Worker
        // threads evaluate nodes serially, so a fresh bump per call cleanly invalidates the previous call.
        [ThreadStatic] private static int _evalGen;
        // The eval's cancel flag, captured from OfsEvalCtx alongside the generation bump so a node callback
        // can check it through NodeContext.IsCancelled without threading the ctx pointer around. null when
        // the host supplies no cancellation (a one-shot functional sample / preview).
        [ThreadStatic] private static int* _cancelFlag;
        private static int BeginEval(OfsEvalCtx* ctx)
        {
            _cancelFlag = ctx->CancelFlag;
            return ++_evalGen;
        }
        internal static int CurrentEvalGen => _evalGen;

        // True once the host cancelled the in-flight eval; false when the host supplies no cancellation.
        internal static bool CurrentEvalCancelled() =>
            _cancelFlag != null && System.Threading.Volatile.Read(ref *_cancelFlag) != 0;

        private static float[] BorrowParams(OfsEvalCtx* ctx)
        {
            int n = ctx->ParamCount;
            if (_paramBuf == null || _paramBuf.Length < n)
                _paramBuf = new float[Math.Max(n, 8)];
            // Always copy: native may reuse the same params buffer address across different nodes with the
            // same param count, so a pointer-equality skip would feed a node stale values. The copy is a
            // handful of floats — negligible next to the managed↔native transition that happens per sample.
            for (int i = 0; i < n; i++)
                _paramBuf[i] = ctx->Params[i];
            return _paramBuf;
        }

        // Live HostApi for the discrete I/O accessors (NodeInputCount/Time/Position, NodeAddAction — all
        // ctx-free host functions). Refreshed to the current plugin's api on every node registration, NOT
        // latched on first use: a HostApi struct is per-plugin and freed on unload, so a once-set pointer
        // dangles after that plugin unloads while another keeps evaluating discrete nodes. Refresh-on-register
        // keeps it pointing at a loaded plugin; the trampolines' released-slot null-check covers the gap when
        // no plugin is loaded. Set on the main thread during OnLoad; pointer-sized writes are atomic on x64.
        private static HostApi* s_discApi;

        // The script host's app-lifetime HostApi, seeded once at script-host init (SetScriptHostApi) and
        // never repointed. Script nodes are never released the way a plugin unload releases its slots, so
        // their eval-time I/O must not deref a freed plugin's api. The discrete I/O accessors are ctx-free
        // and identical across hosts, so reading them through this stable struct is equivalent and never
        // dangles. null only when no script host initialized; then s_discApi is the sole provider.
        private static HostApi* s_stableDiscApi;

        // The api for discrete eval I/O: the stable script-host api when present, else the last registrant.
        private static HostApi* DiscIoApi => s_stableDiscApi != null ? s_stableDiscApi : s_discApi;

        // Per-input read buffers and the reader/writer scratch arrays, grown to the node's pin counts.
        [ThreadStatic] private static double[][]? _inTimeBufs;
        [ThreadStatic] private static int[][]? _inPosBufs;
        [ThreadStatic] private static DiscreteReader[]? _readerScratch;
        [ThreadStatic] private static DiscreteWriter[]? _writerScratch;

        private static void EnsureInBufs(int count)
        {
            if (_inTimeBufs == null || _inTimeBufs.Length < count)
            {
                Array.Resize(ref _inTimeBufs, count);
                Array.Resize(ref _inPosBufs, count);
            }
            if (_readerScratch == null || _readerScratch.Length < count)
                _readerScratch = new DiscreteReader[Math.Max(count, 1)];
        }

        private static void EnsureWriters(int count)
        {
            if (_writerScratch == null || _writerScratch.Length < count)
            {
                int old = _writerScratch?.Length ?? 0;
                Array.Resize(ref _writerScratch, Math.Max(count, 1));
                for (int i = old; i < _writerScratch.Length; i++)
                    _writerScratch[i] = new DiscreteWriter();
            }
        }

        private static DiscreteReader ReadInputAt(void* inp, int idx)
        {
            HostApi* api = DiscIoApi;
            int n = api->NodeInputCount(inp);
            double[]? tb = _inTimeBufs![idx];
            int[]? pb = _inPosBufs![idx];
            if (tb == null || tb.Length < n) { tb = new double[Math.Max(n, 64)]; _inTimeBufs[idx] = tb; }
            if (pb == null || pb.Length < n) { pb = new int[Math.Max(n, 64)]; _inPosBufs[idx] = pb; }
            for (int i = 0; i < n; i++)
            {
                tb[i] = api->NodeInputTime(inp, i);
                pb[i] = api->NodeInputPosition(inp, i);
            }
            // Stamp with the current eval generation (the trampoline already called BeginEval()).
            return new DiscreteReader(tb, pb, n, _evalGen);
        }

        private static void Flush(void* outp, DiscreteWriter w)
        {
            HostApi* api = DiscIoApi;
            foreach (var a in w.Actions)
                api->NodeAddAction(outp, a.At, a.Pos);
        }

        // Safe default when a slot is released or its eval throws: pass each input channel through to the
        // matching output (0 where there is no matching input).
        private static void Passthrough(float* ins, int inCount, float* outs, int outCount)
        {
            for (int k = 0; k < outCount; k++)
                outs[k] = k < inCount ? ins[k] : 0f;
        }

        private static readonly FunctionalSample s_passthroughSample =
            static (double t, ReadOnlySpan<float> ins, Span<float> outs) =>
            {
                for (int k = 0; k < outs.Length; k++)
                    outs[k] = k < ins.Length ? ins[k] : 0f;
            };

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void FunctionalTrampoline(double t, float* ins, int inCount, OfsEvalCtx* ctx, float* outs,
                                                 int outCount, void* ud)
        {
            var slot = SlotTable<StateSlot>.Slots[(int)(nint)ud];
            if (slot == null) { Passthrough(ins, inCount, outs, outCount); return; } // released by plugin unload
            if (slot.Prep != null) // factory node: build once per eval, then sample the cached closure
            {
                var fn = GetPrepared(slot, ctx);
                try { fn(t, new ReadOnlySpan<float>(ins, inCount), new Span<float>(outs, outCount)); }
                catch (Exception ex) { PluginGuard.Report("node:func", ex); Passthrough(ins, inCount, outs, outCount); }
                return;
            }
            if (slot.Func == null) { Passthrough(ins, inCount, outs, outCount); return; }
            int gen = BeginEval(ctx);
            var c = new NodeContext(ctx->RegionStart, ctx->RegionEnd, BorrowParams(ctx), ctx->ParamCount, gen);
            object? box = CapturedBox(ctx);
            // Inline try/catch (not PluginGuard.Run) on the per-sample hot path to avoid a closure allocation.
            try { slot.Func(t, new ReadOnlySpan<float>(ins, inCount), box, c, new Span<float>(outs, outCount)); }
            catch (Exception ex) { PluginGuard.Report("node:func", ex); Passthrough(ins, inCount, outs, outCount); }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void DiscreteTrampoline(void** ins, int inCount, OfsEvalCtx* ctx, void** outs, int outCount,
                                               void* ud)
        {
            var slot = SlotTable<StateSlot>.Slots[(int)(nint)ud];
            if (slot?.Disc == null) return; // released by plugin unload — emit nothing
            int gen = BeginEval(ctx);
            var c = new NodeContext(ctx->RegionStart, ctx->RegionEnd, BorrowParams(ctx), ctx->ParamCount, gen);
            EnsureInBufs(inCount);
            for (int i = 0; i < inCount; i++)
                _readerScratch![i] = ReadInputAt(ins[i], i);
            EnsureWriters(outCount);
            for (int k = 0; k < outCount; k++)
                _writerScratch![k].Clear();
            object? box = CapturedBox(ctx);
            bool ok;
            // Flush only on success: a throwing callback emits nothing rather than half-filled writers.
            try
            {
                slot.Disc(new ReadOnlySpan<DiscreteReader>(_readerScratch, 0, inCount), box, c,
                          new ReadOnlySpan<DiscreteWriter>(_writerScratch, 0, outCount));
                ok = true;
            }
            catch (Exception ex) { PluginGuard.Report("node:disc", ex); ok = false; }
            if (ok)
                for (int k = 0; k < outCount; k++)
                    Flush(outs[k], _writerScratch![k]);
        }

        // ── State-backed (TState) infrastructure ────
        //
        // A node's TState is decoded from its persisted JSON ONCE per axis eval (on the main thread, at
        // snapshot build) into a boxed value held by an int handle; the worker trampolines read that box
        // by handle and unbox a private copy. No state bytes cross the C ABI — only the int handle does.

        // Shared serializer options: public FIELDS are the params (IncludeFields); enums persist by member
        // NAME so reordering survives (JsonStringEnumConverter).
        private static readonly JsonSerializerOptions s_jsonOpts = new()
        {
            IncludeFields = true,
            Converters = { new JsonStringEnumConverter() },
        };

        // One registered node slot. For a plugin node exactly one of {Func, Disc, Prep} is set (its shape);
        // for a script node Func or Disc forwards to the compiled delegate and the codec/UI fields are null.
        private sealed class StateSlot
        {
            public FunctionalEvalErased? Func;
            public DiscreteEvalErased? Disc;
            public PrepareErased? Prep;

            public Func<string, object>? Decode;  // JSON → boxed TState; best-effort, never throws (null for scripts)
            public Func<object?, string>? Encode;  // boxed TState → JSON (null for scripts)

            // The node's `ui` callback: unboxes the current TState, runs the author delegate (which may
            // mutate it / call Node.Update), and returns the new boxed value plus whether it changed
            // (shallow value compare). Null when the node declared no `ui` callback.
            public Func<Ui, object, long, (object box, bool changed)>? UiCallback;
        }

        // Wrap an author `ui` callback into the type-erased closure StateSlot stores. The callback runs on
        // the boxed TState (unboxed to a local so the author gets a `ref`), then we shallow-compare to detect
        // a change. EqualityComparer<TState>.Default gives the spec's semantics for a struct: value fields by
        // value; reference fields (arrays/lists) by identity — so a replaced collection registers as changed
        // and an in-place mutation does not. The packed node `key` is unused here (deferred writes obtain a
        // handle via Ui.Node() inside the callback).
        private static Func<Ui, object, long, (object, bool)>? BuildUi<TState>(NodeUi<TState>? ui)
            where TState : struct
        {
            if (ui == null) return null;
            var cmp = EqualityComparer<TState>.Default;
            return (u, box, key) =>
            {
                TState s = Unbox<TState>(box);
                TState before = s;
                ui(u, ref s);
                return ((object)s!, !cmp.Equals(before, s));
            };
        }

        // The single slot table lives in SlotTable<StateSlot>, shared by plugin nodes (tracked per-instance
        // in _slots) and script nodes (released via ReleaseScript). A released slot is nulled in place; the
        // trampolines null-check. Grows only.

        // Deferred Node.Update mutations, keyed by the host's packed node identity. Enqueued from any thread
        // (an async continuation may resume off the main thread); drained on the main thread in
        // NodeUiTrampoline on the node's next UI pass. Packed keys (region<<32 | node) are reused across
        // project loads, so ClearPendingNodeUpdates() purges the whole map on project load.
        private static readonly ConcurrentDictionary<long, ConcurrentQueue<Func<object?, object>>> _pendingNodeUpdates = new();

        internal static void EnqueueNodeUpdate(long key, Func<object?, object> apply)
            => _pendingNodeUpdates.GetOrAdd(key, static _ => new ConcurrentQueue<Func<object?, object>>()).Enqueue(apply);

        // Drop every queued-but-undrained Node.Update. Called by the host on project load (main thread).
        internal static void ClearPendingNodeUpdates() => _pendingNodeUpdates.Clear();

        // Apply every queued mutation for `key` to `box` (in order), returning whether any ran.
        private static bool DrainNodeUpdates(long key, ref object box)
        {
            if (key < 0 || !_pendingNodeUpdates.TryGetValue(key, out var q)) return false;
            bool any = false;
            while (q.TryDequeue(out var apply)) { box = apply(box)!; any = true; }
            return any;
        }

        // Per-eval captures: handle → boxed TState. Written on the main thread (capture) and removed on the
        // main thread (release); read on worker threads (the trampolines) — a ConcurrentDictionary makes that
        // read lock-free. _captureGen / _nextStateHandle are touched only on the main thread.
        private static readonly ConcurrentDictionary<int, object?> _captures = new();
        private static readonly Dictionary<int, List<int>> _captureGen = new();
        private static int _nextStateHandle;

        // Per-eval prepared closures: stateHandle → the per-sample closure built by a factory node. Built
        // lazily on the first sample of a region eval (worker thread), reused for the rest, and dropped
        // alongside the capture in ReleaseStates.
        private static readonly ConcurrentDictionary<int, object?> _prepared = new();

        // Unbox a captured state value. TState is a struct, so the type test only fails for a null box —
        // a stateHandle of -1 (empty JSON, capture skipped) — which yields the whole-node default.
        internal static TState Unbox<TState>(object? box) where TState : struct => box is TState s ? s : new TState();

        // Build the JSON codec for a concrete TState. Decode is best-effort: empty/whitespace or a hard parse
        // failure yields a fresh default, so the worker always gets a valid value.
        private static (Func<string, object> decode, Func<object?, string> encode) MakeCodec<TState>() where TState : struct
        {
            Func<string, object> decode = json =>
            {
                if (string.IsNullOrWhiteSpace(json)) return new TState();
                try { object? o = JsonSerializer.Deserialize<TState>(json, s_jsonOpts); return o ?? new TState(); }
                catch { return new TState(); }
            };
            Func<object?, string> encode = box => JsonSerializer.Serialize(Unbox<TState>(box), s_jsonOpts);
            return (decode, encode);
        }

        private int AddStateSlot(StateSlot entry) => _slots.Add(entry);

        // Node eval (and a factory `prepare`) runs on worker threads, so it must read all its data from the
        // TState value and never capture plugin state — a captured `this`/local read off-thread is a data
        // race. Reject a capturing eval at registration (throw, so the plugin fails to load) rather than
        // letting a latent race ship: the fix is trivially a `static` lambda. (The `ui` callback is
        // main-thread and may capture, so it isn't checked.)
        private static void RejectCapturingEval(string id, Delegate eval)
        {
            object? target = eval.Target;
            if (target == null)
                return; // a static method group — no instance to capture through
            var captured = target.GetType().GetFields(
                BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
            if (captured.Length > 0)
                throw new InvalidOperationException(
                    $"Node '{id}' eval captures state — node eval runs on worker threads. Read everything from " +
                    "TState (build per-eval working data in a prepare factory) and make it a `static` lambda.");
        }

        private void RegisterFunctional<TState>(string id, string name, NodeShape shape, EvalFunctional<TState> eval,
            NodeUi<TState>? ui, string? group, NodeIcon icon, string? description) where TState : struct
        {
            _host.AssertMainThread("Nodes.AddNode");
            _host.AssertOnLoad("Nodes.AddNode");
            RejectCapturingEval(id, eval);
            EnsureHostApi(_api); // NodeUiTrampoline builds a Ui from this api pointer

            var (decode, encode) = MakeCodec<TState>();
            var entry = new StateSlot
            {
                Decode = decode,
                Encode = encode,
                UiCallback = BuildUi(ui),
                Func = (double t, ReadOnlySpan<float> ins, object? box, NodeContext ctx, Span<float> outs) =>
                {
                    TState s = Unbox<TState>(box);
                    eval(t, ins, in s, ctx, outs);
                },
            };
            int slot = AddStateSlot(entry);
            var tramp = (IntPtr)(delegate* unmanaged[Cdecl]<double, float*, int, OfsEvalCtx*, float*, int, void*, void>)
                &FunctionalTrampoline;
            SubmitNodeDef(id, name ?? string.Empty, OfsSignalKind.Functional, shape, tramp, (void*)(nint)slot,
                          hasState: 1, onNodeUi: NodeUiPtr(entry), group: group, icon: icon, description: description);
        }

        private void RegisterFactory<TState>(string id, string name, NodeShape shape, PrepareFunctional<TState> prepare,
            NodeUi<TState>? ui, string? group, NodeIcon icon, string? description) where TState : struct
        {
            _host.AssertMainThread("Nodes.AddNode");
            _host.AssertOnLoad("Nodes.AddNode");
            RejectCapturingEval(id, prepare);
            EnsureHostApi(_api);

            var (decode, encode) = MakeCodec<TState>();
            var entry = new StateSlot
            {
                Decode = decode,
                Encode = encode,
                UiCallback = BuildUi(ui),
                Prep = (object? box, NodeContext ctx) =>
                {
                    TState s = Unbox<TState>(box);
                    return prepare(in s, ctx);
                },
            };
            int slot = AddStateSlot(entry);
            var tramp = (IntPtr)(delegate* unmanaged[Cdecl]<double, float*, int, OfsEvalCtx*, float*, int, void*, void>)
                &FunctionalTrampoline;
            SubmitNodeDef(id, name ?? string.Empty, OfsSignalKind.Functional, shape, tramp, (void*)(nint)slot,
                          hasState: 1, onNodeUi: NodeUiPtr(entry), group: group, icon: icon, description: description);
        }

        private void RegisterDiscrete<TState>(string id, string name, NodeShape shape, EvalDiscrete<TState> eval,
            NodeUi<TState>? ui, string? group, NodeIcon icon, string? description) where TState : struct
        {
            _host.AssertMainThread("Nodes.AddNode");
            _host.AssertOnLoad("Nodes.AddNode");
            RejectCapturingEval(id, eval);
            EnsureHostApi(_api); // refresh discrete I/O api to this load's live HostApi

            var (decode, encode) = MakeCodec<TState>();
            var entry = new StateSlot
            {
                Decode = decode,
                Encode = encode,
                UiCallback = BuildUi(ui),
                Disc = (ReadOnlySpan<DiscreteReader> ins, object? box, NodeContext ctx,
                        ReadOnlySpan<DiscreteWriter> outs) =>
                {
                    TState s = Unbox<TState>(box);
                    eval(ins, in s, ctx, outs);
                },
            };
            int slot = AddStateSlot(entry);
            var tramp = (IntPtr)(delegate* unmanaged[Cdecl]<void**, int, OfsEvalCtx*, void**, int, void*, void>)
                &DiscreteTrampoline;
            SubmitNodeDef(id, name ?? string.Empty, OfsSignalKind.Discrete, shape, tramp, (void*)(nint)slot,
                          hasState: 1, onNodeUi: NodeUiPtr(entry), group: group, icon: icon, description: description);
        }

        // ── Stateless registration ──────────────────────────────
        // No TState: no JSON codec, no capture (hasState: 0 → the host never calls the state codec, the eval
        // sees stateHandle -1 and a null box), and no body UI (onNodeUi null). The erased slot delegate just
        // ignores the box and forwards to the author's stateless eval. The eval still runs on a worker thread,
        // so it is held to the same no-capture rule.

        private void RegisterFunctionalStateless(string id, string name, NodeShape shape, EvalFunctional eval,
            string? group, NodeIcon icon, string? description)
        {
            _host.AssertMainThread("Nodes.AddNode");
            _host.AssertOnLoad("Nodes.AddNode");
            RejectCapturingEval(id, eval);
            EnsureHostApi(_api);

            var entry = new StateSlot
            {
                Func = (double t, ReadOnlySpan<float> ins, object? _, NodeContext ctx, Span<float> outs) =>
                    eval(t, ins, ctx, outs),
            };
            int slot = AddStateSlot(entry);
            var tramp = (IntPtr)(delegate* unmanaged[Cdecl]<double, float*, int, OfsEvalCtx*, float*, int, void*, void>)
                &FunctionalTrampoline;
            SubmitNodeDef(id, name ?? string.Empty, OfsSignalKind.Functional, shape, tramp, (void*)(nint)slot,
                          hasState: 0, onNodeUi: default, group: group, icon: icon, description: description);
        }

        // A factory node memoizes its per-sample closure under the eval's stateHandle (see GetPrepared), so a
        // stateless factory still registers hasState:1 with a trivial codec — that earns a capture handle per
        // region eval, the only thing the memoization keys on. The decoded box is a shared sentinel the
        // factory ignores; there is no UI, so Encode never runs.
        private static readonly object s_statelessBox = new();

        private void RegisterFactoryStateless(string id, string name, NodeShape shape, PrepareFunctional prepare,
            string? group, NodeIcon icon, string? description)
        {
            _host.AssertMainThread("Nodes.AddNode");
            _host.AssertOnLoad("Nodes.AddNode");
            RejectCapturingEval(id, prepare);
            EnsureHostApi(_api);

            var entry = new StateSlot
            {
                Decode = static _ => s_statelessBox,
                Encode = static _ => "{}",
                Prep = (object? _, NodeContext ctx) => prepare(ctx),
            };
            int slot = AddStateSlot(entry);
            var tramp = (IntPtr)(delegate* unmanaged[Cdecl]<double, float*, int, OfsEvalCtx*, float*, int, void*, void>)
                &FunctionalTrampoline;
            SubmitNodeDef(id, name ?? string.Empty, OfsSignalKind.Functional, shape, tramp, (void*)(nint)slot,
                          hasState: 1, onNodeUi: default, group: group, icon: icon, description: description);
        }

        private void RegisterDiscreteStateless(string id, string name, NodeShape shape, EvalDiscrete eval,
            string? group, NodeIcon icon, string? description)
        {
            _host.AssertMainThread("Nodes.AddNode");
            _host.AssertOnLoad("Nodes.AddNode");
            RejectCapturingEval(id, eval);
            EnsureHostApi(_api);

            var entry = new StateSlot
            {
                Disc = (ReadOnlySpan<DiscreteReader> ins, object? _, NodeContext ctx,
                        ReadOnlySpan<DiscreteWriter> outs) => eval(ins, ctx, outs),
            };
            int slot = AddStateSlot(entry);
            var tramp = (IntPtr)(delegate* unmanaged[Cdecl]<void**, int, OfsEvalCtx*, void**, int, void*, void>)
                &DiscreteTrampoline;
            SubmitNodeDef(id, name ?? string.Empty, OfsSignalKind.Discrete, shape, tramp, (void*)(nint)slot,
                          hasState: 0, onNodeUi: default, group: group, icon: icon, description: description);
        }

        // The shared node-UI trampoline pointer, but only for a node that actually renders something — a node
        // with no `ui` callback gets a null hook so the host falls back to its width-anchor spacer instead of
        // calling an empty render path every frame.
        private static IntPtr NodeUiPtr(StateSlot entry) =>
            entry.UiCallback == null
                ? default
                : (IntPtr)(delegate* unmanaged[Cdecl]<void*, void*, int>)&NodeUiTrampoline;

        // Read the captured TState for this eval (or null if the handle is -1 / already released).
        private static object? CapturedBox(OfsEvalCtx* ctx)
        {
            if (ctx->StateHandle < 0) return null;
            _captures.TryGetValue(ctx->StateHandle, out var box);
            return box;
        }

        // Build-once-per-eval helper for factory nodes. The first sample of a region eval (a unique
        // stateHandle) runs the factory and caches its closure under that handle; later samples reuse it. A
        // factory fault falls back to a passthrough closure (still cached, so it isn't retried per sample).
        // When the handle is -1 (empty JSON, capture skipped) there's no per-eval key to cache under — build
        // fresh each call; correct, just not memoized.
        private static FunctionalSample GetPrepared(StateSlot slot, OfsEvalCtx* ctx)
        {
            int h = ctx->StateHandle;
            if (h >= 0 && _prepared.TryGetValue(h, out var cached) && cached is FunctionalSample c) return c;
            int gen = BeginEval(ctx);
            var nc = new NodeContext(ctx->RegionStart, ctx->RegionEnd, BorrowParams(ctx), ctx->ParamCount, gen);
            object? box = CapturedBox(ctx);
            var built = PluginGuard.Run<FunctionalSample?>("node:prepare", () => slot.Prep!(box, nc), null)
                        ?? s_passthroughSample;
            if (h >= 0) _prepared[h] = built;
            return built;
        }

        // Node-body UI pass (main thread). The host sets the "current node" before the call, so
        // nodeUiGetState/nodeUiSetState read and write *this* node's JSON. Decode → run the `ui` callback
        // (mutating the boxed TState) → if it changed, re-encode and hand the new JSON back; the host turns
        // that into a ModifyRegionEvent (re-eval + undo). Returns 1 on change.
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static int NodeUiTrampoline(void* ctx, void* ud)
        {
            StateSlot? slot;
            lock (SlotTable<StateSlot>.Lock) { int i = (int)(nint)ud; slot = (i >= 0 && i < SlotTable<StateSlot>.Slots.Count) ? SlotTable<StateSlot>.Slots[i] : null; }
            if (slot?.UiCallback == null || slot.Decode == null || slot.Encode == null || s_discApi == null) return 0;
            try
            {
                byte* p = s_discApi->NodeUiGetState(ctx);
                string json = p != null ? Marshal.PtrToStringUTF8((IntPtr)p) ?? "" : "";
                object box = slot.Decode(json);    // best-effort; empty/garbage → default TState
                long key = s_discApi->NodeUiCurrentKey(ctx);
                var uiObj = new Ui(s_discApi);     // s_discApi->Ctx == the ctx the host set current on this call
                bool changed = false;
                // Deferred Node.Update mutations land first, so the `ui` callback reflects the applied data.
                if (DrainNodeUpdates(key, ref box)) changed = true;
                uiObj.BeginPass();
                try
                {
                    var (newBox, uiChanged) = slot.UiCallback(uiObj, box, key);
                    box = newBox;
                    if (uiChanged) changed = true;
                }
                finally { uiObj.EndPass(); }
                if (!changed) return 0;
                byte[] buf = Encoding.UTF8.GetBytes(slot.Encode(box) + "\0");
                fixed (byte* bp = buf) s_discApi->NodeUiSetState(ctx, bp);
                return 1;
            }
            catch (Exception ex) { PluginGuard.Report("node:ui", ex); return 0; }
        }

        // ── State capture / release (called by the host on the main thread) ──────
        // Reached from native via PluginBootstrapper.{Capture,Release}NodeStateNative (this Ofs.Api is shared
        // across ALCs, so the slot/codec tables here are the ones the trampolines read).

        internal static int CaptureState(int slot, string json, int generation)
        {
            StateSlot? s;
            lock (SlotTable<StateSlot>.Lock) { s = (slot >= 0 && slot < SlotTable<StateSlot>.Slots.Count) ? SlotTable<StateSlot>.Slots[slot] : null; }
            if (s?.Decode == null) return -1;
            object box = s.Decode(json);     // best-effort decode → boxed TState (never throws)
            int h = _nextStateHandle++;
            _captures[h] = box;
            if (!_captureGen.TryGetValue(generation, out var list)) { list = new List<int>(); _captureGen[generation] = list; }
            list.Add(h);
            return h;
        }

        internal static void ReleaseStates(int generation)
        {
            if (!_captureGen.TryGetValue(generation, out var list)) return;
            foreach (int h in list)
            {
                _captures.TryRemove(h, out _);
                _prepared.TryRemove(h, out _); // drop any factory closure built for this handle
            }
            _captureGen.Remove(generation);
        }

        // ── Script-node hooks (Ofs.ScriptHost) ────────────────────────────────
        //
        // A compiled script reuses the exact same slot table and trampolines as a plugin node, but is NOT
        // registered with the host (it never enters effectReg.pluginNodes) and carries no TState (box stays
        // null, stateHandle -1). Ofs.ScriptHost compiles the source, binds the delegate, and calls one of
        // these to append it and get back { trampoline, slot } — handed to native as a CompiledScriptRef.
        // The append happens at runtime (a recompile), possibly on a worker thread, so it is serialized
        // (SlotTable<StateSlot>.Lock).

        // Make the discrete I/O accessors available to the trampolines even when no plugin has registered a
        // discrete node (e.g. script nodes). Refreshes to the caller's live api rather than latching the
        // first — a prior provider's HostApi struct may already be freed (see s_discApi).
        internal static void EnsureHostApi(HostApi* api)
        {
            lock (SlotTable<StateSlot>.Lock)
            {
                s_discApi = api;
            }
        }

        // Register the script host's app-lifetime HostApi (script-host init only). Unlike EnsureHostApi this
        // also latches s_stableDiscApi — the never-dangling api script nodes read I/O through, since their
        // slots outlive any plugin that might otherwise own s_discApi. Seeds s_discApi too so it is valid
        // before the first plugin registers.
        internal static void SetScriptHostApi(HostApi* api)
        {
            lock (SlotTable<StateSlot>.Lock)
            {
                s_stableDiscApi = api;
                s_discApi = api;
            }
        }

        // Collectible AssemblyLoadContext backing each appended script slot, keyed by slot index. Unlike a
        // plugin node (tracked per-instance and released on unload), a script's compiled assembly is
        // otherwise rooted by its slot delegate for the process lifetime — so without this an editing session
        // leaks one ALC + assembly + slot per distinct revision. ReleaseScript nulls the slot and unloads the
        // ALC once the host stops referencing the compiled artifact. The slot index is never reused (the list
        // only grows); a released slot stays null forever, which the trampolines treat as a passthrough no-op.
        private static readonly ConcurrentDictionary<int, AssemblyLoadContext> s_scriptAlcs = new();

        // Append a functional script delegate; returns its trampoline pointer and slot (as userData).
        internal static (IntPtr trampoline, IntPtr userData) AppendFunctional(ScriptFunctionalEval fn,
            AssemblyLoadContext alc)
        {
            lock (SlotTable<StateSlot>.Lock)
            {
                int slot = SlotTable<StateSlot>.Slots.Count;
                SlotTable<StateSlot>.Slots.Add(new StateSlot
                {
                    Func = (double t, ReadOnlySpan<float> ins, object? _, NodeContext ctx, Span<float> outs) =>
                        fn(t, ins, ctx, outs),
                });
                s_scriptAlcs[slot] = alc;
                var tramp = (IntPtr)(delegate* unmanaged[Cdecl]<double, float*, int, OfsEvalCtx*, float*, int, void*, void>)
                    &FunctionalTrampoline;
                return (tramp, (IntPtr)slot);
            }
        }

        // Append a discrete script delegate; returns its trampoline pointer and slot (as userData).
        internal static (IntPtr trampoline, IntPtr userData) AppendDiscrete(ScriptDiscreteEval fn,
            AssemblyLoadContext alc)
        {
            lock (SlotTable<StateSlot>.Lock)
            {
                int slot = SlotTable<StateSlot>.Slots.Count;
                SlotTable<StateSlot>.Slots.Add(new StateSlot
                {
                    Disc = (ReadOnlySpan<DiscreteReader> ins, object? _, NodeContext ctx,
                            ReadOnlySpan<DiscreteWriter> outs) => fn(ins, ctx, outs),
                });
                s_scriptAlcs[slot] = alc;
                var tramp = (IntPtr)(delegate* unmanaged[Cdecl]<void**, int, OfsEvalCtx*, void**, int, void*, void>)
                    &DiscreteTrampoline;
                return (tramp, (IntPtr)slot);
            }
        }

        // Release a previously appended script: null its slot delegate (so the trampoline degrades to a
        // passthrough no-op) and unload its collectible ALC so the compiled assembly can be collected. Safe
        // against an in-flight eval: a worker that already fetched the delegate completes its call (the ALC
        // unload is deferred until no frame executes in it), and any later eval reads the now-null slot.
        internal static void ReleaseScript(int slot)
        {
            lock (SlotTable<StateSlot>.Lock)
            {
                if (slot >= 0 && slot < SlotTable<StateSlot>.Slots.Count)
                    SlotTable<StateSlot>.Slots[slot] = null;
            }
            if (s_scriptAlcs.TryRemove(slot, out var alc))
                alc.Unload();
        }

        // ── Shared node-def marshaling ────────────────────────────────────────

        // The host copies the strings out of OfsNodeDef before returning, so the pinned buffers only need to
        // survive the RegisterNode call. Plugin nodes are TState-backed: there is no scalar param list —
        // every widget renders through onNodeUi.
        private void SubmitNodeDef(string id, string displayName, OfsSignalKind signal, NodeShape shape,
            IntPtr trampoline, void* userData, int hasState, IntPtr onNodeUi, string? group, NodeIcon icon,
            string? description)
        {
            var pins = new List<GCHandle>();
            GCHandle PinUtf8(string s)
            {
                var h = GCHandle.Alloc(Encoding.UTF8.GetBytes((s ?? string.Empty) + "\0"), GCHandleType.Pinned);
                pins.Add(h);
                return h;
            }
            // A null/empty group stays a null pointer (the host defaults it to the plugin name); only an
            // explicit group string is pinned and passed. A null/empty description likewise stays null (no tooltip).
            byte* GroupOrNull() => string.IsNullOrEmpty(group) ? null : (byte*)PinUtf8(group!).AddrOfPinnedObject();
            byte* DescriptionOrNull() =>
                string.IsNullOrEmpty(description) ? null : (byte*)PinUtf8(description!).AddrOfPinnedObject();
            try
            {
                var hId = PinUtf8(id);
                var hName = PinUtf8(displayName);

                var inPtrs = new IntPtr[shape.Inputs.Count];
                for (int i = 0; i < inPtrs.Length; i++)
                    inPtrs[i] = PinUtf8(shape.Inputs[i]).AddrOfPinnedObject();
                var outPtrs = new IntPtr[shape.Outputs.Count];
                for (int i = 0; i < outPtrs.Length; i++)
                    outPtrs[i] = PinUtf8(shape.Outputs[i]).AddrOfPinnedObject();
                var hIn = GCHandle.Alloc(inPtrs, GCHandleType.Pinned); pins.Add(hIn);
                var hOut = GCHandle.Alloc(outPtrs, GCHandleType.Pinned); pins.Add(hOut);

                var def = new OfsNodeDef
                {
                    Id = (byte*)hId.AddrOfPinnedObject(),
                    DisplayName = (byte*)hName.AddrOfPinnedObject(),
                    InputNames = inPtrs.Length > 0 ? (byte**)hIn.AddrOfPinnedObject() : null,
                    OutputNames = (byte**)hOut.AddrOfPinnedObject(),
                    Fn = trampoline,
                    UserData = userData,
                    OnNodeUi = onNodeUi,
                    Signal = (int)signal,
                    InputCount = shape.Inputs.Count,
                    OutputCount = shape.Outputs.Count,
                    HasState = hasState,
                    Group = GroupOrNull(),
                    Icon = (int)icon,
                    Description = DescriptionOrNull(),
                };
                _api->RegisterNode(_api->Ctx, &def);
            }
            finally
            {
                foreach (var h in pins) h.Free();
            }
        }
    }
}
