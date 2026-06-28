# Script Nodes

A **script node** is a small C# file you drop into a processing region's graph. No DLL, no project,
no `OfsPlugin` class — just a fragment of code with a short header. The host compiles it at runtime
(Roslyn), renders any declared knobs on the node body, and runs the body on a worker thread as part
of region evaluation.

> **Script node** vs **plugin node.** A script node is the lightweight path: a `.cs` *fragment* with
> a header-declared pin shape, scalar knobs, and a compute body — no custom UI, no DLL. A **plugin
> node** (see [`Nodes`](xref:Ofs.Nodes) / [`Node`](xref:Ofs.Node)) ships in a plugin DLL and adds its
> own UI and arbitrary typed state. Reach for a plugin only when a script can't express it (a button,
> a file dialog, a color, non-scalar state). For a formula — a leaky integrator, a custom wave, a
> remap curve — write six lines of C#.

The types your body touches (`NodeContext`, `DiscreteReader`, `DiscreteWriter`, `ScriptAction`) are
documented in the [API Reference](xref:Ofs.NodeContext); this page is the authoring guide. For the
rules every node body must respect — the no-capture / no-`Host` worker-thread contract, cancellation,
and the discrete-vs-functional choice — read [Node Best Practices](node-best-practices.md); they apply
to script nodes and plugin nodes alike.

## Create one

Right-click the processing-graph canvas to open the **Add node** menu. You can either:

- **Pick a ready-made library node** — the shipped `Generate / Modify / Combine` entries (Sine, Scale,
  Invert, …) are themselves script nodes. Adding one embeds its source into the node, read-only. Use
  **Save to scripts folder** to fork it into an editable copy.
- **New script…** — prefills a *working* template (header + a minimal body) for the signal, input
  count, and output count you choose, and offers to open it in your editor.

Your own scripts live as plain `.cs` files under `<prefPath>/scripts/` (the same pref folder that
holds `plugins/`). A node references a script **by file name**, so many nodes and projects can reuse
one file, and an external editor is first-class — reference `Ofs.Api` from the file for IntelliSense
on `NodeContext` &c. Edits hot-reload (on editor save, focus-gain, explicit Reload, or the per-node
**Watch** toggle).

## The header

A script is **self-describing**: a few `// !ofs:` directive lines at the top tell the host what kind
of node it is and what pins and knobs to draw. The `!` is what distinguishes a directive from an
ordinary comment — a plain `// …` line (including a double-commented `// // !ofs:…` example) is never
parsed as one.

| Directive | Value | Purpose |
|-----------|-------|---------|
| `// !ofs:signal` | `functional` \| `discrete` | how the node produces its output (see below) |
| `// !ofs:input` | a C# identifier | declares **one** input pin named that identifier — repeat for more (0..16 pins) |
| `// !ofs:output` | a C# identifier | declares **one** output pin — repeat for more (1..16 pins) |
| `// !ofs:name` | free text | display name in the Add menu (defaults to the file name) |
| `// !ofs:description` | free text | tooltip in the Add menu |
| `// !ofs:param` | `<name> <default> [min] [max] [type]` | one editable knob (repeatable) — see [Parameters](#parameters) |

A node declares **one `// !ofs:input` line per input pin** and **one `// !ofs:output` line per output
pin** — the directive count *is* the pin count. The pin's value is a single C# identifier, which
becomes both the pin's label *and* an injected local in your body (see [The body](#the-body)).

- **No `// !ofs:input` line ⇒ a generator** (0 inputs).
- **No `// !ofs:output` line ⇒ a single implicit output named `out`** — the common case needs no
  directive (a functional body `return`s; a discrete body writes `outp`).
- The parser is **tolerant**: a malformed directive is dropped with a warning (the compiler logs it),
  the value doesn't vanish silently. A duplicate pin name within a direction keeps the first; pins past
  the 16-per-direction cap are dropped. Omitting `// !ofs:signal` defaults to `functional`.

Pin **count and signal are re-read on every save**, so changing the header reshapes the node — its
pins and signal kind — in place; you never recreate it.

## The body

Your file's code *is* the method body — the host wraps it into the compiled `Eval`. Which names are in
scope depends on `signal`, the declared pins, and the params:

| In scope | When | Is |
|----------|------|----|
| `t` | functional only | `double` — the time being sampled |
| *(each input name)* | per `// !ofs:input` | functional: the input value (`float`, 0..100); discrete: a [`DiscreteReader`](xref:Ofs.DiscreteReader) — `foreach` it, or index `[i]` for `.At` / `.Pos` |
| `ins` | always | the raw input span (`ins[k]`) — `ReadOnlySpan<float>` (functional) or `ReadOnlySpan<DiscreteReader>` (discrete); for advanced/dynamic indexing |
| `outp` | discrete, single output | the output sink for the implicit `out` pin — `outp.Add(at, pos)` appends an action |
| *(each output name)* | discrete, multi-output | a [`DiscreteWriter`](xref:Ofs.DiscreteWriter) per pin — `<name>.Add(at, pos)` |
| *(each output name)* | functional, multi-output | a `float` per pin you assign (`<name> = …`); each starts at the neutral `50` |
| `outs` | always | the raw output span (`outs[k]`); for advanced/dynamic shapes |
| `ctx` | always | [`NodeContext`](xref:Ofs.NodeContext) — `ctx.RegionStart`, `ctx.RegionEnd`, `ctx.Param(i)`, `ctx.Params`, `ctx.IsCancelled` |
| *(each param name)* | per `// !ofs:param` | an injected typed local (see [Parameters](#parameters)) |

How you write the output depends on the shape:

| Shape | Single output | Multiple outputs |
|-------|---------------|------------------|
| **Functional** | `return <float>;` | assign each named output local (`left = …; right = …;`) — no `return` |
| **Discrete** | `outp.Add(at, pos);` | write each named [`DiscreteWriter`](xref:Ofs.DiscreteWriter) (`left.Add(…); right.Add(…);`) |

- **Functional** runs **once per output sample**: produce a `float` in `0..100` (out-of-range values
  are clamped). Don't keep state across samples — samples may run on different worker threads.
- **Discrete** runs **once per region**: iterate the input reader(s) with your own accumulators and
  `Add` to the output sink(s). The host sorts your output and clamps `pos` to `0..100`. This is where
  ordered, stateful logic belongs (integrators, smoothing, edge detection) — and where a long body
  should poll [`ctx.IsCancelled`](xref:Ofs.NodeContext.IsCancelled) at loop boundaries and return
  early (see [Node Best Practices](node-best-practices.md)).

> An input or output name that is a C# **keyword** (e.g. `out`, `in`) can't be injected as a local —
> reach the pin through the raw `ins[k]` / `outs[k]` span instead. The implicit single output is named
> `out` for exactly this reason; that's why single-output bodies use `return` / `outp`, never a local
> called `out`.

## Parameters

Each `// !ofs:param` becomes an editable widget on the node **and** a typed local injected into your
body — so you write `gain`, not `ctx.Param(0)`. Four scalar types, all float-backed:

| Type | Header | Widget | Injected local |
|------|--------|--------|----------------|
| Float *(default)* | `// !ofs:param gain 1.0 0 4` | drag / slider (slider when min≠max) | `float` |
| Int | `// !ofs:param steps 8 1 64 int` | drag / slider int | `int` |
| Bool | `// !ofs:param enabled 1 bool` | checkbox | `bool` |
| Enum | `// !ofs:param mode 0 enum:Sine,Square,Saw` | combo (stored value = index) | `int` (0-based index) |

`min`/`max` are optional; equal min and max (or omitted) means unbounded. Params reconcile **by
index** on every recompile and on load — kept where the slot survives, seeded to the default for a
new slot, clamped to range — so adding or reordering params won't silently corrupt a saved value.
A script node's params can only be these scalars; for a string, path, or color, use a plugin node.

## Examples

A **functional generator** — a sine wave (`Sine.cs`, shipped). No `// !ofs:input` ⇒ 0 inputs; no
`// !ofs:output` ⇒ the single implicit `out`, so the body `return`s:

```csharp
// !ofs:signal functional
// !ofs:name Sine Wave
// !ofs:description Generates a smooth sinusoidal oscillation.
// !ofs:param amplitude 25 int
// !ofs:param period 1.0
// !ofs:param phase 0.0
// !ofs:param center 50 int
double per = period == 0f ? 1.0 : period;
double angle = 2.0 * Math.PI * ((t - ctx.RegionStart) / per + phase);
return center + amplitude * (float)Math.Sin(angle);
```

A **functional modifier** — gain + offset (`Scale.cs`, shipped). The `a` local is the upstream value
at `t`, named by the `// !ofs:input a` directive:

```csharp
// !ofs:signal functional
// !ofs:input a
// !ofs:name Scale
// !ofs:param gain 1.0
// !ofs:param offset 0.0
return a * gain + offset;
```

A **discrete modifier** — mirror every action around a center (`Invert.cs`, shipped). The `a` local
is now a [`DiscreteReader`](xref:Ofs.DiscreteReader) over the input actions; `outp` is the implicit
output sink:

```csharp
// !ofs:signal discrete
// !ofs:input a
// !ofs:name Invert
// !ofs:param center 50 0 100 int
foreach (var p in a)
    outp.Add(p.At, 2 * center - p.Pos);
```

A **multi-input combiner** — a weighted average of three inputs (`Blend3.cs`, shipped). Each
`// !ofs:input` line adds a pin and a same-named local; an unwired input feeds the neutral `50`:

```csharp
// !ofs:signal functional
// !ofs:input in0
// !ofs:input in1
// !ofs:input in2
// !ofs:name Blend 3
// !ofs:param weightA 1.0 0 4
// !ofs:param weightB 1.0 0 4
// !ofs:param weightC 1.0 0 4
float wsum = weightA + weightB + weightC;
return wsum == 0f ? 50f : (in0 * weightA + in1 * weightB + in2 * weightC) / wsum;
```

A **multi-output node** — split one motion into a primary output and its reflection (`Mirror.cs`,
shipped). Two `// !ofs:output` directives ⇒ two output locals; a multi-output functional body
**assigns** each instead of returning:

```csharp
// !ofs:signal functional
// !ofs:input  stroke
// !ofs:output main
// !ofs:output mirror
// !ofs:name Mirror
// !ofs:param Center 50 0 100 int
main = stroke;
mirror = Center * 2f - stroke; // reflected about Center → wire to an opposing axis
```

## Sharing & trust

A plain project references scripts **by name**, so opening one lists which scripts will run before it
compiles. A **graph preset** (`<prefPath>/graphs/`) is meant to be shareable, so it **embeds** each
referenced script's full source; loading one surfaces the embedded code behind a trust warning and,
once accepted, materializes the files into your scripts folder. Shipped library scripts are
auto-trusted (their source is byte-identical to what ships with the app), so a graph built only from
them never prompts. Compiled scripts run with full trust — no sandbox — so treat a shared graph like
any other code you'd run.

## See also

- [Node Best Practices](node-best-practices.md) — the worker-thread contract, `static` eval, `IsCancelled`,
  and functional-vs-discrete; mandatory reading for any node body.
- [Debugging Plugins](plugin-debugging.md) — where compile errors and dropped-directive warnings surface.
- [`NodeContext`](xref:Ofs.NodeContext) — region bounds, param access, and cancellation inside a body.
- [`DiscreteReader`](xref:Ofs.DiscreteReader) / [`DiscreteWriter`](xref:Ofs.DiscreteWriter) /
  [`ScriptAction`](xref:Ofs.ScriptAction) — the discrete input/output surface.
- [`Nodes`](xref:Ofs.Nodes) — the plugin-side node API, for when a script node isn't enough.
