using System;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Numerics;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using Ofs;

namespace SamplePlugin
{
    /// <summary>
    /// Demonstrates every feature of the idiomatic API: player properties/commands/events,
    /// axis read + buffered Edit/Commit, all Ui widgets, and custom nodes —
    /// including a generator with custom node UI that loads a model file and runs slow "inference"
    /// in eval on the worker (so the processing region shows its busy indicator).
    /// </summary>
    public sealed class SamplePlugin : OfsPlugin
    {
        public override string Name => "Sample Plugin";

        private float _targetSpeed = 1.0f;
        private float _targetVolume = 1.0f;
        private int _targetPos = 50;
        private bool _logEvents = true;
        private string _lastEvent = "(none)";

        // Widget-gallery state.
        private Vector4 _tint = new(0.40f, 0.70f, 1.00f, 1.00f);
        private string _notes = "Multi-line notes.\nEdit me.";
        private float _progress = 0.35f;

        protected override void OnLoad()
        {
            // Commands are registered with their handlers inline — no central dispatch switch.
            Host.Commands.Register("seek-start", "Seek to Start", () => Host.Player.Seek(0));
            Host.Commands.Register("reverse-active-axis", "Reverse Active Axis", ReverseActiveAxis);

            Host.Player.TimeChanged += t => Note($"time = {t:F3}s");
            Host.Player.PlayingChanged += p => Note($"playing = {p}");
            Host.Player.SpeedChanged += s => Note($"speed = {s:F2}x");
            Host.Player.MediaChanged += m => Note($"media = {Path.GetFileName(m)}");
            Host.Axes.Modified += r => Note($"axis modified: {r}");
            Host.Axes.ProjectChanged += () => Note("project changed");

            RegisterNodes();
        }

        protected override void OnRenderUi(Ui ui)
        {
            ui.Section("Player State", () =>
            {
                ui.Label($"Time:     {Host.Player.Time:F3}s");
                ui.Label($"Duration: {Host.Player.Duration:F3}s");
                ui.Label($"Playing:  {(Host.Player.IsPlaying ? "yes" : "no")}");
                ui.Label($"Speed:    {Host.Player.Speed:F2}x");
                ui.Label($"Volume:   {Host.Player.Volume:P0}");
                // Fps / VideoWidth / VideoHeight are null until media with a video stream is loaded.
                ui.Label($"Fps:      {Host.Player.Fps?.ToString("F2", CultureInfo.InvariantCulture) ?? "(no media)"}");
                ui.Label($"Frame:    {(Host.Player.VideoWidth is { } w && Host.Player.VideoHeight is { } h ? $"{w}x{h}" : "(no video)")}");
                ui.Label($"Media:    {Path.GetFileName(Host.Player.MediaPath)}");

                var active = Host.Axes.Active; // always valid — there is always an active axis
                int count = Host.Axes[active].Actions.Count;
                ui.Label($"Active:   {active} ({count} actions)");
            });

            ui.Section("Events", () =>
            {
                ui.Checkbox("Log events", ref _logEvents);
                ui.Label($"Last: {_lastEvent}");
            });

            ui.Section("Player Commands", () =>
            {
                if (ui.Button("Seek to 0")) Host.Player.Seek(0);
                if (ui.Button("Toggle Play")) Host.Player.TogglePlay();
                if (ui.Slider("Speed", ref _targetSpeed, 0.1f, 3.0f))
                    Host.Player.Speed = _targetSpeed;
                if (ui.Slider("Volume", ref _targetVolume, 0f, 1f))
                    Host.Player.Volume = _targetVolume;
            });

            ui.Section("Axis Edit", () =>
            {
                ui.Slider("Action Position", ref _targetPos, 0, 100);
                if (ui.Button("Add action at current time")) AddActionAtPlayhead();
                if (ui.Button("Reverse active axis")) ReverseActiveAxis();
            });

            ui.Section("Widget Gallery", () =>
            {
                if (ui.ColorEdit("Tint", ref _tint)) Note($"tint = {_tint}");

                ui.InputTextMultiline("Notes", ref _notes, maxBytes: 1024, heightLines: 3);

                if (ui.Slider("Progress", ref _progress, 0f, 1f)) { }
                ui.ProgressBar(_progress, $"{_progress * 100f:F0}%");
            });

            ui.Section("Axes", () =>
            {
                // Exercises the axis-state host reads (name / actions / visibility / lock) per axis.
                foreach (var role in Host.Axes.Existing.OrderBy(a => (int)a))
                {
                    var axis = Host.Axes[role];
                    ui.Row(axis.Name, () =>
                        ui.Label($"{axis.Actions.Count} actions" +
                                 $"{(axis.IsVisible ? "" : ", hidden")}" +
                                 $"{(axis.IsLocked ? ", locked" : "")}"));
                }

                if (ui.Button("Focus L0")) Host.Axes.SetActive(StandardAxis.L0);
            });

            ui.Section("Project", () =>
            {
                var meta = Host.Project.Metadata;
                string title = meta.ValueKind == JsonValueKind.Object && meta.TryGetProperty("title", out var t) ? t.GetString() ?? "" : "";
                string creator = meta.ValueKind == JsonValueKind.Object && meta.TryGetProperty("creator", out var c) ? c.GetString() ?? "" : "";
                ui.Label($"Title:   {(title.Length > 0 ? title : "(untitled)")}");
                ui.Label($"Creator: {(creator.Length > 0 ? creator : "(none)")}");
                ui.Label($"Dirty:   {(Host.Project.IsDirty ? "yes" : "no")}");
                ui.Label($"Regions: {Host.Project.Regions.Count}, " +
                         $"Chapters: {Host.Project.Chapters.Count}, " +
                         $"Bookmarks: {Host.Project.Bookmarks.Count}");
            });

            ui.Section("Notifications", () =>
            {
                if (ui.Button("Info")) Host.NotifyInfo("An informational message.");
                if (ui.Button("Success")) Host.NotifySuccess("That worked.");
                if (ui.Button("Warning")) Host.NotifyWarning("Heads up.");
                if (ui.Button("Error")) Host.NotifyError("Something went wrong.");
            });

            ui.Section("Dialogs", () =>
            {
                // The dialogs are non-blocking: kick them off and await the result. The await resumes on the
                // main thread a few frames later (discard the Task so an unobserved fault can't crash us).
                if (ui.Button("Open file…")) _ = OpenFileAsync();
                if (ui.Button("Save file as…")) _ = SaveFileAsync();
                if (ui.Button("Pick folder…")) _ = PickFolderAsync();
                if (ui.Button("Confirm…")) _ = ConfirmAsync();
            });
        }

        // ── Commands ──────────────────────────────────────────────────────────

        private void AddActionAtPlayhead()
        {
            var role = Host.Axes.Active;
            Host.Axes[role].Edit()
                           .Add(Host.Player.Time, _targetPos)
                           .Commit();
        }

        private void ReverseActiveAxis()
        {
            var role = Host.Axes.Active;
            var edit = Host.Axes[role].Edit();
            var flipped = edit.Actions.Select(a => a with { Pos = 100 - a.Pos }).ToArray();
            edit.Clear();
            edit.AddRange(flipped);
            edit.Commit();   // whole reverse = one undo step
        }

        // ── Nodes ─────────────────────────────────────────────────────────────

        // Node state types — the node's parameters are the fields of its TState, rendered by the
        // node's `ui` callback. Field initializers + an explicit parameterless ctor supply non-zero
        // defaults, which `new TState()` (and the empty-JSON decode) honor.
        public struct OffsetState
        {
            public float Offset;
        }

        public struct BlendState
        {
            public float Mix = 0.5f;
            public BlendState() { }
        }

        public struct ThinState
        {
            public float MinGap = 0.05f;
            public ThinState() { }
        }

        // Fake "AI" motion generator. The two scalars are live knobs (drawn in the node's `ui` callback);
        // ModelPath + Envelope describe the loaded model; Status is UI feedback. Every field is part of the
        // node's state — it persists, is visible to eval, and is undoable.
        public struct AiGenState
        {
            public float Intensity = 80f;      // peak stroke height
            public float StrokeMs = 300f;      // time between strokes (live)
            public string? ModelPath;          // the loaded file — a reference, not bulk data
            public float[]? Envelope;          // the loaded model: per-stroke intensity coefficients in [0,1] (persisted)
            public string Status;              // UI feedback; like every TState field it persists with the node
            public AiGenState() { Status = "No model loaded"; }
        }

        private void RegisterNodes()
        {
            // Shifts the input up or down by a fixed offset. The `ui` callback draws its knob — node body
            // widgets are explicit (Ui.Slider/…), so their labels follow ofs-ng's UI language like any
            // per-frame plugin UI. (This sample is single-language, so the labels are plain literals.)
            Host.Nodes.AddNode<OffsetState>("offset_mod", "Offset",
                new NodeShape(inputs: ["in"], outputs: ["out"]),
                static (double t, ReadOnlySpan<float> ins, in OffsetState s, NodeContext ctx, Span<float> outs) =>
                    outs[0] = Math.Clamp(ins[0] + s.Offset, 0f, 100f),
                (Ui ui, ref OffsetState s) => ui.Slider("Offset", ref s.Offset, -50f, 50f));

            // Weighted blend: result = a*(1-mix) + b*mix.
            Host.Nodes.AddNode<BlendState>("blend_comb", "Blend",
                new NodeShape(inputs: ["a", "b"], outputs: ["out"]),
                static (double t, ReadOnlySpan<float> ins, in BlendState s, NodeContext ctx, Span<float> outs) =>
                    outs[0] = ins[0] * (1f - s.Mix) + ins[1] * s.Mix,
                (Ui ui, ref BlendState s) => ui.Slider("Mix", ref s.Mix, 0f, 1f));

            Host.Nodes.AddNode("max_comb", "Maximum",
                new NodeShape(inputs: ["a", "b"], outputs: ["out"]),
                static (double t, ReadOnlySpan<float> ins, NodeContext ctx, Span<float> outs) =>
                    outs[0] = MathF.Max(ins[0], ins[1]));
            Host.Nodes.AddNode("min_comb", "Minimum",
                new NodeShape(inputs: ["a", "b"], outputs: ["out"]),
                static (double t, ReadOnlySpan<float> ins, NodeContext ctx, Span<float> outs) =>
                    outs[0] = MathF.Min(ins[0], ins[1]));

            // Removes actions closer in time than MinGap to their predecessor.
            Host.Nodes.AddNode<ThinState>("thin", "Thin",
                new NodeShape(inputs: ["in"], outputs: ["out"]),
                static (ReadOnlySpan<DiscreteReader> ins, in ThinState s, NodeContext ctx,
                        ReadOnlySpan<DiscreteWriter> outs) =>
                {
                    double minGap = s.MinGap;
                    double last = double.NegativeInfinity;
                    foreach (var a in ins[0])
                        if (a.At - last >= minGap) { outs[0].Add(a); last = a.At; }
                },
                (Ui ui, ref ThinState s) => ui.Slider("Min Gap", ref s.MinGap, 0.005f, 2.0f));

            // Fake AI motion generator (0 inputs → generator, discrete signal). The custom UI loads a model
            // file; the slow "inference" runs here in eval, on the JobSystem worker — that is the whole
            // point of an AI node. Because the cost lives in eval rather than the load coroutine, re-evaluation
            // runs off the frame thread and the processing region shows its busy indicator while we work. The
            // motion is rendered from the loaded envelope across the region, using the live Intensity/Stroke-ms
            // params and the region bounds from NodeContext.
            Host.Nodes.AddNode<AiGenState>("ai_motion", "AI Motion (demo)",
                new NodeShape(inputs: [], outputs: ["motion"]),
                eval: static (ReadOnlySpan<DiscreteReader> ins, in AiGenState s, NodeContext ctx,
                              ReadOnlySpan<DiscreteWriter> outs) =>
                {
                    DiscreteWriter outp = outs[0];
                    if (s.Envelope is not { Length: > 0 } env) return; // no model loaded yet → empty output
                    double half = Math.Max(0.01, s.StrokeMs / 1000.0) * 0.5; // half-period: trough→peak→trough

                    // Simulate per-stroke model inference. Budget the latency so a single uninterrupted eval
                    // stays bounded (~1.2s) regardless of stroke count; a newer edit that supersedes this job
                    // is handled by the ctx.IsCancelled poll below, which lets a stale eval bail immediately.
                    // A real node would run the actual model in this loop.
                    int strokes = (int)Math.Max(1, (ctx.RegionEnd - ctx.RegionStart) / Math.Max(half * 2, 1e-6));
                    int perStrokeMs = (int)Math.Clamp(1200.0 / strokes, 0, 40);

                    int i = 0;
                    for (double t = ctx.RegionStart; t <= ctx.RegionEnd && i < 100_000; t += half, i++)
                    {
                        if ((i & 1) == 0) // one inference step per stroke (trough→peak pair)
                        {
                            if (ctx.IsCancelled) return; // superseded by a newer edit → drop this (discarded) output
                            if (perStrokeMs > 0) Thread.Sleep(perStrokeMs);
                        }
                        bool peak = (i & 1) == 1;
                        int pos = peak ? (int)Math.Clamp(s.Intensity * env[(i / 2) % env.Length], 0f, 100f) : 0;
                        outp.Add(t, pos);
                    }
                },
                ui: (Ui ui, ref AiGenState s) =>
                {
                    // Draw the live knobs, then the model-load button. The button kicks off the async model
                    // load; the result lands via Node.Update a few frames later, triggering the (slow) re-eval.
                    // A deferred write can't hold `ref s`, so grab a capture-safe handle from the Ui.
                    ui.Slider("Intensity", ref s.Intensity, 0f, 100f);
                    ui.Slider("Stroke ms", ref s.StrokeMs, 60f, 1000f);
                    if (ui.Button("Load model…")) _ = LoadModel(ui.Node());
                    ui.Label(s.Status ?? "No model loaded");
                });
        }

        // Pick a model file and load it off the frame thread, then apply the result to the node's state. The
        // load is fast — the slow part is the per-stroke "inference", which runs in eval on the worker so the
        // processing region shows its busy indicator. A value-type TState can't be captured by `ref`
        // across an await, so the deferred write goes through the capture-safe Node handle; each Update is
        // applied on the main thread.
        private async Task LoadModel(Node node)
        {
            string? path = await Host.Dialogs.OpenFile("Choose a model file", "*.onnx;*.bin;*.txt;*.*", "Model files");
            if (path is null) return;

            node.Update((ref AiGenState s) => s.Status = $"Loading {Path.GetFileName(path)}…");

            float[]? envelope = null;
            try
            {
                byte[] data = await File.ReadAllBytesAsync(path);     // real file load (async IO)
                envelope = await Task.Run(() => DeriveEnvelope(data)); // decode the "model" off the frame thread
            }
            catch { /* unreadable file → envelope stays null, reported below */ }

            node.Update((ref AiGenState s) =>
            {
                s.ModelPath = path;
                s.Envelope = envelope; // replace-don't-mutate: a brand-new array, never an in-place edit
                s.Status = envelope is null
                    ? $"Failed to read {Path.GetFileName(path)}"
                    : $"Ready · {envelope.Length} strokes · {Path.GetFileName(path)}";
            });
        }

        // Deterministic stand-in for a loaded model: derive a per-stroke intensity envelope from the file's
        // bytes so the same file always yields the same motion. The slow inference itself happens in eval.
        private static float[] DeriveEnvelope(byte[] data)
        {
            int strokes = Math.Clamp(data.Length / 64, 8, 64);
            int seed = 17;
            foreach (byte b in data) seed = unchecked(seed * 31 + b);
            var rng = new Random(seed);
            var env = new float[strokes];
            for (int i = 0; i < strokes; i++) env[i] = 0.4f + 0.6f * (float)rng.NextDouble();
            return env;
        }

        // ── Async dialog flows ────────────────────────────────────────────────

        private async Task OpenFileAsync()
        {
            string? path = await Host.Dialogs.OpenFile("Choose a file", "*.funscript;*.json", "Scripts");
            if (path != null) Note($"opened {Path.GetFileName(path)}");
        }

        private async Task SaveFileAsync()
        {
            string? path = await Host.Dialogs.SaveFile("Save as", "untitled.funscript", "*.funscript", "Funscript");
            if (path != null) Note($"save target {Path.GetFileName(path)}");
        }

        private async Task PickFolderAsync()
        {
            string? dir = await Host.Dialogs.PickFolder("Choose a folder");
            if (dir != null) Note($"folder {dir}");
        }

        private async Task ConfirmAsync()
        {
            if (await Host.Dialogs.Confirm("Sample Plugin", "Proceed with the sample action?", ConfirmKind.YesNo))
                Host.NotifySuccess("You chose yes.");
        }

        // ── Helpers ─────────────────────────────────────────────────────────────

        private void Note(string what)
        {
            if (_logEvents) _lastEvent = what;
        }
    }
}
