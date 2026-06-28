# Node Best Practices

A processing node's evaluation runs on **worker threads**, off the main thread, and is re-run
constantly as the user edits. That single fact drives every rule below. They apply equally to a
[script node](script-nodes.md) (a `.cs` fragment) and a [plugin node](xref:Ofs.Nodes) (a DLL with
`AddNode`) — the body runs under the same evaluation contract either way.

## Eval runs on a worker thread — read only your inputs

A node callback receives exactly what it needs and nothing else: its **inputs**, its **parameters**
(or `TState`), and a [`NodeContext`](xref:Ofs.NodeContext). It must **not** touch `Host`, the player,
the project, the axes, or any plugin field. Those live on the main thread; reading them from a worker
is a data race.

- **Plugin nodes:** put every value the eval needs in the node's `TState` struct. The host serializes
  it, value-copies it to the worker, and hands it to your eval by `in`. The node's UI callback (which
  *is* on the main thread) is where you edit `TState`.
- **Script nodes:** read [`ctx.Param(i)`](xref:Ofs.NodeContext.Param*) (or the injected named param
  local) and your input pins. That's the whole input surface by construction.

## Make the eval a `static` lambda — no captures

The host **rejects a node whose eval captures state** at registration time (it throws, so the plugin
fails to load loudly rather than shipping a latent race). A captured `this`, local, or field read from
a worker thread is exactly the bug the worker-thread rule forbids.

The fix is almost always one keyword — `static`:

```csharp
// ❌ captures `_gain` — throws at registration
Host.Nodes.AddNode<Empty>("gain", "Gain", shape,
    (double t, ReadOnlySpan<float> ins, in Empty s, NodeContext ctx, Span<float> outs)
        => outs[0] = ins[0] * _gain);

// ✅ state travels in TState; the lambda captures nothing
Host.Nodes.AddNode<GainState>("gain", "Gain", shape,
    static (double t, ReadOnlySpan<float> ins, in GainState s, NodeContext ctx, Span<float> outs)
        => outs[0] = ins[0] * s.Gain);
```

The **UI callback is exempt** — it runs on the main thread and may capture freely. Only the eval
(and a [`prepare`](#build-expensive-artifacts-once-with-a-factory) factory) is held to the no-capture
rule.

## Don't keep state between functional samples

A **functional** node's eval runs **once per output sample**, and successive samples may run on
*different* worker threads. There is no safe place to stash a running value between calls — a `static`
field would be shared across threads and across unrelated evaluations.

If you need ordered, stateful logic — an integrator, smoothing, hysteresis, edge detection — use a
**discrete** node instead. Its eval runs **once per region**, so you can loop the inputs in time order
and keep local accumulators across actions:

```csharp
// discrete: one pass over the region, local state is fine
int held = 50;
foreach (var p in a)
{
    held = (held + p.Pos) / 2;     // a simple running smooth
    outp.Add(p.At, held);
}
```

## Build expensive artifacts once with a factory

If a functional node needs a non-trivial precomputation (a lookup table, an interpolator, a parsed
dataset), don't rebuild it every sample and don't capture it. Register a
[`PrepareFunctional`](xref:Ofs.PrepareFunctional`1) factory: it runs **once per region eval**, returns
the per-sample [`FunctionalSample`](xref:Ofs.FunctionalSample) closure, and the host caches that
closure for the rest of the eval.

```csharp
Host.Nodes.AddNode<CurveState>("curve", "Curve", shape,
    static (in CurveState s, NodeContext ctx) =>
    {
        float[] lut = BuildLut(s);                 // once per region eval
        return (double t, ReadOnlySpan<float> ins, Span<float> outs)
            => outs[0] = Sample(lut, ins[0]);      // per sample
    });
```

## Honor cancellation in long bodies

When the user keeps editing, the host **cancels the in-flight eval** and starts a fresh one; a
cancelled eval's output is discarded. A long **discrete** generator/modifier should poll
[`ctx.IsCancelled`](xref:Ofs.NodeContext.IsCancelled) at natural loop boundaries and return early —
bailing doesn't change the result (it's thrown away regardless), it just frees the worker sooner.

```csharp
for (int i = 0; i < a.Count; i++)
{
    if ((i & 0x3FF) == 0 && ctx.IsCancelled) return; // every ~1024 actions
    // … heavy per-action work …
}
```

`IsCancelled` is always `false` when the host supplies no cancellation (e.g. a one-shot preview), so
the check is safe to leave in unconditionally.

## Don't cache `NodeContext` or `DiscreteReader`

Both are thin views over **per-call scratch** that the next evaluation on the same worker thread
reuses. Read them *inside* the callback; storing one in a field and reading it later throws
(`InvalidOperationException` — "stored beyond the node callback"). In particular,
[`ctx.Params`](xref:Ofs.NodeContext.Params) and a [`DiscreteReader`](xref:Ofs.DiscreteReader) are only
valid for the duration of the call.

## Outputs: ranges, sorting, and unwired inputs

- **Functional** output is a `float` in **0..100** written into `outs[k]`; values outside are clamped.
- **Discrete** output is appended via [`DiscreteWriter.Add`](xref:Ofs.DiscreteWriter.Add*); the host
  **sorts** each output by time and **clamps** positions to 0..100, so you needn't pre-sort.
- An **unwired input** feeds the neutral `50` (functional) / an empty reader (discrete). Don't assume a
  pin is connected; produce something sensible regardless.
- **Write every output pin** (see below). A functional output you leave unwritten holds whatever was in
  the span — not a defined value.

## Multi-output nodes

A node may declare **1..16 outputs**. The output names you pass to [`NodeShape`](xref:Ofs.NodeShape)
map **by index** to the `outs` span — `outs[0]` is the first declared name, `outs[1]` the second — and
each pin routes downstream independently. Write **every** output each eval; the host evaluates outputs
together, so a pin you skip is undefined (functional) or simply empty (discrete).

A **functional** combiner that splits one input into a primary and its reflection (the plugin-node
analogue of the shipped `Mirror` script):

```csharp
struct MirrorState { public float Center; }

Host.Nodes.AddNode<MirrorState>("mirror", "Mirror",
    new NodeShape(inputs: ["stroke"], outputs: ["main", "mirror"]),
    static (double t, ReadOnlySpan<float> ins, in MirrorState s, NodeContext ctx, Span<float> outs) =>
    {
        outs[0] = ins[0];                  // "main"   — pass the stroke through
        outs[1] = s.Center * 2f - ins[0];  // "mirror" — reflected about Center
    },
    ui: static (Ui ui, ref MirrorState s) => ui.DragFloat("Center", ref s.Center, 0.5f, 0f, 100f));
```

A **discrete** node that routes each action to one of two outputs by level — note each writer is sorted
and clamped on its own, and an output you never `Add` to stays empty:

```csharp
struct SplitState { public int Threshold; }

Host.Nodes.AddNode<SplitState>("split", "Split by level",
    new NodeShape(inputs: ["in"], outputs: ["low", "high"]),
    static (ReadOnlySpan<DiscreteReader> ins, in SplitState s, NodeContext ctx,
            ReadOnlySpan<DiscreteWriter> outs) =>
    {
        foreach (var p in ins[0])
            (p.Pos < s.Threshold ? outs[0] : outs[1]).Add(p.At, p.Pos);
    });
```

The same applies to the **stateless** and **factory** overloads — only the `outs` span widens; a
factory's per-sample closure receives the same multi-output `Span<float>`. Output pin **names must be
distinct within the node** ([`NodeShape`](xref:Ofs.NodeShape) throws otherwise), since they double as
link-target labels. For the script-node equivalent (named output locals via `// !ofs:output`), see
[Script Nodes](script-nodes.md).

## Prefer determinism

The host evaluates and re-evaluates freely and may cache results. Given the same inputs and params, a
node should produce the same output — no wall-clock time, no unseeded randomness. If you need
variation, drive it from a **seed parameter** (as the shipped `Noise` script does), so the result is
reproducible and a saved project re-evaluates identically.

## Stateless nodes for pure functions

A node with no parameters and no UI — `min`, `max`, `abs`, a passthrough — should register through a
**stateless** [`AddNode`](xref:Ofs.Nodes.AddNode*) overload (no `TState`). It persists no JSON, draws
no body widgets, and still gets the same input/output spans.

## The node UI callback

The optional `ui` callback ([`NodeUi<TState>`](xref:Ofs.NodeUi`1)) draws the node's body widgets and is
the **opposite** of the eval in every way that matters: it runs on the **main thread**, once per frame
while the node is visible, and it **may capture** (it is not held to the no-capture rule). It receives
`TState` by `ref` — edit it synchronously and the host does the rest: it shallow-compares the struct
and, on a change, persists the new state and re-evaluates the region — recorded as a single undo step.
A node with no `ui` callback simply shows no body widgets.

```csharp
struct GainState { public float Gain; public bool Invert; }

Host.Nodes.AddNode<GainState>("gain", "Gain",
    new NodeShape(inputs: ["in"], outputs: ["out"]),
    static (double t, ReadOnlySpan<float> ins, in GainState s, NodeContext ctx, Span<float> outs) =>
        outs[0] = (s.Invert ? 100f - ins[0] : ins[0]) * s.Gain,
    ui: static (Ui ui, ref GainState s) =>
    {
        ui.DragFloat("Gain", ref s.Gain, 0.01f, 0f, 4f);
        ui.Checkbox("Invert", ref s.Invert);   // mutate s by ref — the host detects the change
    });
```

Use the [`Ui`](xref:Ofs.Ui) builder's widgets (`Label`, `Button`, `Slider`, `DragFloat`, `Checkbox`,
`Combo`, `ColorEdit`, …); they mutate the `ref`-passed fields directly, so you rarely need the `bool`
they return. Two rules:

- **Replace reference fields, don't mutate them in place.** Change detection compares value fields by
  value but reference fields (arrays, lists) by **identity** — so assign a *new* array/list to register
  a change; an in-place edit is missed.
- **Don't make the `ui` callback `static` if it needs to capture** — unlike the eval, capturing here is
  fine. (It can still be `static` when it only touches `s`, as above.)

## Deferred UI writes (plugin nodes)

The `ref TState` is only valid for the duration of the synchronous callback, so you **cannot** hold it
across an `await`. For an async write — a file dialog, a background computation that finishes later —
grab a capture-safe [`Node`](xref:Ofs.Node) handle from [`Ui.Node`](xref:Ofs.Ui.Node*) *inside* the
callback, then call [`Node.Update`](xref:Ofs.Node.Update*) when the work completes. The mutation is
queued and applied on the main thread on the node's next UI pass.

```csharp
ui: static (Ui ui, ref PathState s) =>
{
    ui.Label(s.File.Length == 0 ? "(no file)" : s.File);
    if (ui.Button("Choose…"))
        _ = PickAsync(ui.Node());   // hand off the capture-safe handle, don't await here
}

// elsewhere in the plugin
static async Task PickAsync(Node node)
{
    string? path = await Host.Dialogs.OpenFile("Pick data file");
    if (path != null)
        node.Update((ref PathState s) => s.File = path); // applied on the main thread
}
```

The same identity rule applies inside `Node.Update`: replace reference fields rather than mutating them
in place.

## See also

- [Script Nodes](script-nodes.md) — the lightweight `.cs`-fragment node path.
- [`Nodes`](xref:Ofs.Nodes) — the plugin-side registration API and all `AddNode` overloads.
- [`NodeContext`](xref:Ofs.NodeContext) — region bounds, params, and `IsCancelled`.
- [`DiscreteReader`](xref:Ofs.DiscreteReader) / [`DiscreteWriter`](xref:Ofs.DiscreteWriter) — the
  discrete I/O surface.
