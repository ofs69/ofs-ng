using System;
using System.Collections.Generic;
using Ofs;

namespace Ofs.Core
{
    public sealed class CorePlugin : OfsPlugin
    {
        public enum Mode { None, Alternating, ShapedApproach }

        // Which stroke extrema the "Peaks only" selection mode keeps. Both is the default — selecting only
        // the tops grabs every *other* turning point, which surprised users who expected the whole stroke
        // skeleton.
        public enum PeakKind { Both, Tops, Bottoms }

        // How moving a point carries the rest of its stroke — both the run leading into it and the run
        // leading out. Rigid slides every carried point by the full delta (shape kept exactly); Taper and
        // Smooth fade the influence to zero at each turning point so they stay pinned and the stroke bends
        // organically — Smooth eases at both ends. (Moving only the point is the native edit mode; this
        // mode always carries the stroke.) See MoveStrokeWith.
        public enum StrokeMoveMode { Rigid, Taper, Smooth }

        // The whole editable panel state, persisted per-project as one object via Host.Project.Scoped. The
        // UI binds straight to these public fields (ref), the editing handlers read them, and the field
        // initializers are the defaults a project with nothing stored falls back to. Public fields (not a
        // record) so widgets can bind by ref and the JSON includes them.
        public sealed class Settings
        {
            public Mode Mode;
            // Independent of Mode: how moving a point carries the stroke running through it (the points
            // before and after it). All modes carry organically; see MoveStrokeWith.
            public StrokeMoveMode MoveMode = StrokeMoveMode.Taper;
            // --- Alternating ---
            // No persistent top/bottom toggle: alternation is always derived from the previous point (the
            // curve is the state), so it stays correct across seeks and undo with nothing to reset.
            public bool UseFixedRange;
            public int FixedBottom;
            public int FixedTop = 100;
            // --- Shaped Approach ---
            // The injected points shape the approach to the value you press. ApproachShapeIdx picks a named
            // easing preset (see kShapeGammas); ApproachSteps is how many points form it. Default to Ease
            // Out, not Linear: Linear injects collinear points identical to a plain insert, so the shaping
            // would be invisible on first use — the whole point of the mode missed.
            public int ApproachShapeIdx = 1; // Ease Out
            public int ApproachSteps = 2;
            // --- Peaks only (selection mode) ---
            // Which extrema a box-drag keeps. Default Both (every turning point); a flat-topped peak is
            // recognised, not dropped — see OnSelectPeaks.
            public PeakKind PeakSelect = PeakKind.Both;
        }

        // Str.* resolves in ofs-ng's active UI language (the base keeps the resx culture synced). Returning
        // it here makes the plugin's name in the plugin list follow the language — the host re-pulls Name on
        // switch.
        public override string Name => Str.PluginName;

        // Per-project panel state. Wired in OnLoad; auto-reloaded on each project open, saved on change.
        private ProjectScoped<Settings> _settings = null!;
        private Settings S => _settings.Value;

        // Gamma per shape preset, in shapeLabels order (built in DrawSettings). Replaces the old opaque
        // -1..1 slider: these are 4^{-1,-0.5,0,+0.5,+1}, the same [1/4, 4] gamma range as named stops.
        private static readonly double[] kShapeGammas = { 0.25, 0.5, 1.0, 2.0, 4.0 };

        private static readonly int[] ActionPositions =
            { 0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };

        protected override void OnLoad()
        {
            // Alternating / Shaped-Approach overrides the native "add a point" gesture. Registering
            // publishes the mode in the footer selector; the user activates it, and then OnEditIntent
            // reshapes each AddPoint per the panel's Mode. When the mode is inactive — or its Mode is
            // None — the native insert happens unchanged. onUi draws this mode's options in the docked Tool
            // Options panel; the host shows it only while "shape" is active, so there is no hand-gating.
            Host.Editing.RegisterMode("shape", Str.ShapeModeName, OnEditIntent, onUi: DrawEditOptions);

            // Peaks-only selection. Registering publishes it in the footer's Select selector; once the
            // user activates it, a box selection keeps only the stroke's turning points (tops, bottoms, or
            // both — see Settings.PeakSelect) instead of every in-range action. A stateless resolver (no
            // onEnter/onExit) — it only redefines what a gesture selects, never edits. Other gestures
            // (select-all, click) Pass through to native. onUi draws the shared peak settings.
            Host.Selection.RegisterMode("peak-select", Str.SelectModePeaksName, OnSelectPeaks, onUi: DrawPeakOptions);

            // Peak navigator. Registering publishes it in the footer's Step selector; once active, the
            // active-axis prev/next-action keys (↓/↑) step between the stroke's turning points instead of
            // every action. Which extrema count as peaks is the shared Settings.PeakSelect (same concept as
            // the Peaks-only selection mode). The other step channels (overlay grid, all-axes) Pass to native.
            // onUi draws the same peak settings the selection mode does (its own popover, off the Step selector).
            Host.Navigation.RegisterMode("peak-nav", Str.NavPeaksName, OnPeakStep, onUi: DrawPeakOptions);

            // Editing commands ported from classic OFS. All are offered for binding (inRebindList) so the user
            // can assign a shortcut, and they also appear in the command palette. The title comes from Str.* so
            // it follows ofs-ng's UI language — the host reloads the plugin on a language switch, re-running
            // OnLoad (and so this registration) in the new language.
            Host.Commands.Register("equalize", Str.CmdEqualize, Equalize, inRebindList: true);
            Host.Commands.Register("invert", Str.CmdInvert, Invert, inRebindList: true);
            Host.Commands.Register("isolate", Str.CmdIsolate, Isolate, inRebindList: true);
            Host.Commands.Register("repeat-stroke", Str.CmdRepeatStroke, RepeatStroke, inRebindList: true);

            RegisterNodes();

            // The panel state lives in the project: one handle that loads now, reloads on every project
            // open, and saves on change (see OnRenderUi's Sync). No manual subscribe/load/reset wiring.
            _settings = Host.Project.Scoped<Settings>("settings");
        }

        // ── Processing nodes ────────────────────────────────────────────────────

        // State for the Shape Curve node. Only the two scalars persist; the heavy artifact (a gamma
        // lookup table) is rebuilt from them in `prepare`, never stored — see RegisterNodes.
        public struct ShapeState
        {
            public float Gamma = 1.0f; // <1 boosts lows, >1 boosts highs
            public bool Invert;        // mirror the curve (100 - shaped)
            public ShapeState() { }
        }

        private void RegisterNodes()
        {
            // Shape Curve: remap input position [0,100] through a gamma curve. Computing pow() per output
            // sample would be wasteful, so this is a *factory* node: `prepare` runs
            // ONCE per region eval on the worker, builds a 256-entry LUT from the captured state, and
            // returns a closure that only does a cheap linear-interpolated table lookup per sample. The LUT
            // is non-serializable working data that lives for one eval; only Gamma/Invert persist.
            // Name and the body widgets go through Str.* so the node localizes with the app: the host
            // reloads the plugin on a language switch (re-running this registration), and the `ui` callback
            // re-reads its labels every frame. Gamma/Invert persist; the LUT is rebuilt each eval from them.
            // The eval (the `prepare` factory) is a `static` lambda: node eval runs on worker threads, so it
            // must read everything from its TState and capture no plugin state — `static` makes the compiler
            // enforce that. The inner per-sample closure it returns may capture freely (locals, the LUT).
            Host.Nodes.AddNode<ShapeState>("shape", Str.NodeShapeCurve,
                new NodeShape(inputs: ["in"], outputs: ["out"]),
                prepare: static (in ShapeState s, NodeContext ctx) =>
                {
                    float gamma = Math.Clamp(s.Gamma, 0.1f, 4.0f);
                    bool invert = s.Invert;
                    const int N = 256;
                    var lut = new float[N + 1];
                    for (int i = 0; i <= N; i++)
                    {
                        float y = (float)(Math.Pow(i / (double)N, gamma) * 100.0);
                        lut[i] = invert ? 100f - y : y;
                    }
                    return (double t, ReadOnlySpan<float> ins, Span<float> outs) =>
                    {
                        float x = Math.Clamp(ins[0], 0f, 100f) / 100f * N;
                        int i = (int)x;
                        if (i >= N) { outs[0] = lut[N]; return; }
                        float frac = x - i;
                        outs[0] = lut[i] * (1f - frac) + lut[i + 1] * frac; // linear interpolation between entries
                    };
                },
                ui: (Ui ui, ref ShapeState s) =>
                {
                    ui.Slider(Str.ParamGamma, ref s.Gamma, 0.1f, 4.0f);
                    ui.Checkbox(Str.ParamInvert, ref s.Invert);
                },
                description: Str.NodeShapeCurveDesc);
        }

        // Continue the motion past the playhead. A port of classic OFS GetLastStroke + repeatLastStroke,
        // made smarter in two ways:
        //   * Motif repeat: when the recent points form a repeating multi-point motif (a rhythm richer
        //     than a plain up/down, e.g. a double-tap), replay the WHOLE motif forward so the pattern
        //     continues faithfully instead of collapsing to a single half-stroke.
        //   * Plateau-robust: a hold (equal-position neighbours) is no longer read as a direction change,
        //     so an anchor on or near a flat segment still continues the real motion, holds and all.
        // A plain oscillation has no motif beyond the trivial up/down, so it falls through to the classic
        // single-stroke continuation. The continuation is applied directly as one undo step and the new
        // points are selected (classic OFS behaviour — it just lands; undo to discard).
        private void RepeatStroke()
        {
            var role = Host.Axes.Active;
            var axis = Host.Axes[role];
            var actions = axis.Actions;
            if (actions.Count < 2) return;

            // Anchor: the last action at or before the playhead — the point the repeat continues from.
            double time = Host.Player.Time;
            int anchor = -1;
            for (int i = 0; i < actions.Count && actions[i].At <= time; i++)
                anchor = i;
            if (anchor < 1) return;

            var stroke = RepeatMotif(actions, anchor) ?? RepeatLastStroke(actions, anchor);
            if (stroke is null || stroke.Length == 0) return;

            // Overwrite whatever lived in the new time-span, splice in the continuation, select it.
            var edit = axis.Edit();
            edit.RemoveRange(stroke[0].At, stroke[^1].At);
            foreach (var s in stroke) edit.Add(s.At, s.Pos);
            edit.ClearSelection();
            foreach (var s in stroke) edit.Select(s.At);
            edit.Commit();

            // Advance the playhead to the end of the continuation so the next repeat picks up from here.
            Host.Player.Seek(stroke[^1].At);
        }

        // Smart path: detect the shortest repeating point-motif ending at the anchor and append one more
        // period after it. Returns null when there's no motif richer than a plain oscillation (period ≤ 2
        // points) — that case is left to RepeatLastStroke, preserving the simple feel.
        private static ScriptAction[]? RepeatMotif(IReadOnlyList<ScriptAction> a, int anchor)
        {
            const int posTol = 2;           // positions are 0..100 ints; tolerate hand-authoring jitter
            const double gapTolFrac = 0.20; // inter-point time gaps may drift 20% and still count as periodic
            const int maxPeriod = 32;       // bound the search; longer "motifs" aren't perceived as a repeat

            // Require two full repetitions of a period P as evidence, so the search window is 2*P points.
            int maxP = Math.Min(maxPeriod, anchor / 2);
            int period = 0;
            for (int p = 2; p <= maxP; p++)
                if (IsPeriodic(a, anchor, p, posTol, gapTolFrac)) { period = p; break; }
            if (period < 3) return null; // none found, or a plain ABAB oscillation → classic path

            // Append one period after the anchor: copy points (anchor-period+1 .. anchor) shifted forward
            // by the period's time span. Each copy lands strictly after the anchor (the first one gap past
            // it), so the motif continues the motion rather than restating an existing point.
            double span = a[anchor].At - a[anchor - period].At;
            var motif = new ScriptAction[period];
            for (int k = 0; k < period; k++)
            {
                var src = a[anchor - period + 1 + k];
                motif[k] = new ScriptAction(src.At + span, src.Pos);
            }
            return motif;
        }

        // True if the last 2*P points up to and including the anchor repeat with period P: each point
        // matches the one P back in position, and each inter-point gap matches the gap P back.
        private static bool IsPeriodic(IReadOnlyList<ScriptAction> a, int anchor, int p, int posTol, double gapTolFrac)
        {
            for (int i = anchor; i > anchor - p; i--) // one full period's worth of points
            {
                int j = i - p;
                if (j < 1) return false; // not enough history to confirm a second repetition
                if (Math.Abs(a[i].Pos - a[j].Pos) > posTol) return false;
                double g1 = a[i].At - a[i - 1].At;
                double g2 = a[j].At - a[j - 1].At;
                if (Math.Abs(g1 - g2) > Math.Max(0.01, gapTolFrac * Math.Max(g1, g2))) return false;
            }
            return true;
        }

        // Classic path: continue the motion by replaying the previous opposite-direction stroke, shifted so
        // its first point lands on the anchor (connecting to the existing action rather than floating at the
        // playhead). Plateau-robust: a hold is not a reversal, so flat segments don't fragment the stroke.
        private static ScriptAction[]? RepeatLastStroke(IReadOnlyList<ScriptAction> a, int anchor)
        {
            int dir = DirInto(a, anchor); // direction of the run leading into the anchor, ignoring holds
            if (dir == 0) return null;    // anchor sits on a pure plateau — no motion to continue

            // Walk back through the current run (same direction, or a hold) to its turning point `mid`.
            int mid = anchor - 1;
            while (mid > 0 && Continues(a, mid, dir))
                mid--;
            if (mid < 1) return null; // no stroke before the current run to repeat

            // Previous stroke: the opposite-direction run ending at `mid`. Walk back to its turning point.
            // [begin..mid] is the stroke we replay; `begin` shares the anchor's extreme, so it continues cleanly.
            int prevDir = DirInto(a, mid);
            if (prevDir == 0) return null;
            int begin = mid - 1;
            while (begin > 0 && Continues(a, begin, prevDir))
                begin--;

            int count = mid - begin + 1;
            if (count < 2) return null;

            double offset = a[anchor].At - a[begin].At;
            var shifted = new ScriptAction[count];
            for (int i = 0; i < count; i++)
            {
                var src = a[begin + i];
                shifted[i] = new ScriptAction(Math.Max(0.0, src.At + offset), src.Pos);
            }
            return shifted;
        }

        // Sign of the most recent real (non-flat) step ending at or before index i; 0 only if every step
        // from 1..i is a hold. Lets the stroke walk see through plateaus to the true direction.
        private static int DirInto(IReadOnlyList<ScriptAction> a, int i)
        {
            for (int k = i; k >= 1; k--)
            {
                int s = Math.Sign(a[k].Pos - a[k - 1].Pos);
                if (s != 0) return s;
            }
            return 0;
        }

        // True if the step ending at index i keeps the run going in direction `dir` — same sign, or a hold.
        private static bool Continues(IReadOnlyList<ScriptAction> a, int i, int dir)
        {
            int s = Math.Sign(a[i].Pos - a[i - 1].Pos);
            return s == 0 || s == dir;
        }

        // The plugin's options live entirely in the docked Tool Options panel (the onUi callbacks), drawn by
        // the host only while the relevant mode is active. It therefore publishes no window of its own — not
        // overriding OnRenderUi is what tells the host "no window" (see OfsPlugin.OnRenderUi).

        // Edit-mode options: the add/move-shaping settings. The host calls this only while the "edit" mode
        // is active, so no hand-gating is needed. Edits to _settings.Value persist automatically — the host
        // flushes scoped values once per frame (no-op when nothing changed, so an idle panel never dirties).
        private void DrawEditOptions(Ui ui)
        {
            DrawSettings(ui, S);
        }

        // Peak options, shared by the Peaks-only selection mode and the Peak navigator (each contributes its
        // own section in the Tool Options panel). The host calls this only while the owning mode is active.
        private void DrawPeakOptions(Ui ui)
        {
            DrawPeakSettings(ui, S);
        }

        private static void DrawSettings(Ui ui, Settings s)
        {
            // Independent of Mode: governs how a point move carries its stroke (drag and the move commands).
            ui.Combo(Str.StrokeMoveModeLabel, ref s.MoveMode, m => m switch
            {
                StrokeMoveMode.Rigid => Str.StrokeModeRigid,
                StrokeMoveMode.Taper => Str.StrokeModeTaper,
                StrokeMoveMode.Smooth => Str.StrokeModeSmooth,
                _ => m.ToString(),
            });

            ui.Separator();

            ui.Combo(Str.ModeLabel, ref s.Mode, m => m switch
            {
                Mode.None => Str.ModeNone,
                Mode.Alternating => Str.ModeAlternating,
                Mode.ShapedApproach => Str.ModeShapedApproach,
                _ => m.ToString(),
            });

            if (s.Mode == Mode.Alternating)
            {
                ui.Section(Str.SectionAlternating, () =>
                {
                    ui.Checkbox(Str.FixedRange, ref s.UseFixedRange);
                    if (s.UseFixedRange)
                    {
                        // Stacked, not a Row: InputInt's -/+ step buttons are fixed-size, so two of them
                        // side-by-side overflow a narrow panel. One per line gives each the full width.
                        bool edited = false;
                        edited |= ui.InputInt(Str.Bottom, ref s.FixedBottom);
                        edited |= ui.InputInt(Str.Top, ref s.FixedTop);
                        s.FixedBottom = Math.Clamp(s.FixedBottom, 0, 100);
                        s.FixedTop = Math.Clamp(s.FixedTop, 0, 100);
                        // Don't reorder while the user is mid-edit, only once they've moved on.
                        if (!edited && s.FixedBottom > s.FixedTop)
                            (s.FixedBottom, s.FixedTop) = (s.FixedTop, s.FixedBottom);
                    }
                });
            }

            if (s.Mode == Mode.ShapedApproach)
            {
                ui.Section(Str.SectionShapedApproach, () =>
                {
                    // Self-describing presets (in kShapeGammas order) instead of a bare number — each label
                    // says what the curve does, so there is nothing to interpret on first use.
                    string[] shapeLabels =
                        { Str.ShapeSnapEarly, Str.ShapeEaseOut, Str.ShapeLinear, Str.ShapeEaseIn, Str.ShapeSnapLate };
                    ui.Combo(Str.ApproachShape, ref s.ApproachShapeIdx, shapeLabels);
                    ui.InputInt(Str.ApproachSteps, ref s.ApproachSteps);
                    s.ApproachSteps = Math.Clamp(s.ApproachSteps, 1, 8);
                });
            }
        }

        private static void DrawPeakSettings(Ui ui, Settings s)
        {
            ui.Combo(Str.PeakSelectLabel, ref s.PeakSelect, k => k switch
            {
                PeakKind.Both => Str.PeakKindBoth,
                PeakKind.Tops => Str.PeakKindTops,
                PeakKind.Bottoms => Str.PeakKindBottoms,
                _ => k.ToString(),
            });
        }

        // The active edit mode's resolver. It reshapes the add-a-point gestures (per Mode) and the point-move
        // gestures (per MoveMode — mouse drag and the keyboard nudge commands); every other intent (remove,
        // paste, …) passes straight through to native. The returned intents flow back through the host's
        // normal mutation path, so undo coalescing and multi-axis group fan-out keep working — this plugin
        // never touches ScriptProject directly.
        private EditResult OnEditIntent(EditIntent intent)
        {
            // Stroke-carry overrides single-action moves only: a mouse drag or a single-action keyboard
            // nudge, both delivered as MovePoint. A multi-action selection nudge (MoveSelection) is left to
            // the host's native resolution and falls through below.
            if (intent.Kind == EditIntentKind.MovePoint)
                return MoveStrokeWith(intent);

            if (S.Mode == Mode.None) return EditResult.Pass;

            // Resolve the gesture's lead axis + time, and pass everything else through. AddPoint carries
            // an explicit axis/time; AddPointAtPlayhead carries only the position (axis = the active one,
            // time = the playhead).
            StandardAxis role;
            double time;
            switch (intent.Kind)
            {
                case EditIntentKind.AddPoint:
                    role = intent.Axis;
                    time = intent.Time;
                    break;
                case EditIntentKind.AddPointAtPlayhead:
                    role = Host.Axes.Active;
                    time = Host.Player.Time;
                    break;
                default:
                    return EditResult.Pass;
            }

            var pts = S.Mode == Mode.Alternating
                ? AlternatingPoints(role, time, intent.Pos)
                : ShapedApproachPoints(role, time, intent.Pos);
            return EditResult.Replace(pts);
        }

        // One captured point of the stroke being dragged: its position at the gesture's start and the share
        // of the anchor's total drag it takes (1 next to the anchor, fading toward the far turning point by mode).
        private struct StrokePoint
        {
            public double OrigAt;
            public int OrigPos;
            public double Weight;
        }

        // Per-drag capture for the stroke-move override. The plugin can't see gesture phase, so a fresh drag
        // is recognised by the anchor's fromTime not matching where the previous frame left it. Capturing
        // the stroke and its weights once — and computing ABSOLUTE targets from the original geometry each
        // frame — keeps the deformation drift-free (a per-frame weighted *increment* would lose the fractional
        // position share to integer rounding) and stable (no re-identifying the run as its shape changes).
        private StandardAxis _strokeAxis;
        private double _strokeExpectedAt = double.NaN; // anchor time expected next frame; NaN ⇒ no drag in flight
        private double _strokeAnchorAt;                // anchor's original time/pos at gesture start
        private int _strokeAnchorPos;
        private double _strokePrevAt;                  // time of the action just before the incoming run (a wall)
        private double _strokeNextAt;                  // time of the action just after the outgoing run (a wall)
        private double _strokeLastTotalDt;             // total time delta applied at the last committed frame
        private readonly List<StrokePoint> _strokePre = new();  // [incoming turning point .. anchor-1], time order
        private readonly List<StrokePoint> _strokePost = new(); // [anchor+1 .. outgoing turning point], time order

        // Drag-moves-stroke: dragging one point also drags the runs of points leading INTO it and OUT of it.
        // Both surrounding strokes follow — the points up to the incoming turning point and down to the
        // outgoing one — so the curve bends on both sides instead of only behind the dragged point. How much
        // each follows is the move mode: Rigid moves them all by the full delta (the strokes slide, shape
        // exact); Taper/Smooth fade the share to zero at each turning point so both stay pinned and the curve
        // bends. Targets are absolute (original spot + weighted share of the total drag), so the shape is
        // reconstructed each frame rather than accumulated.
        private EditResult MoveStrokeWith(EditIntent intent)
        {
            var actions = Host.Axes[intent.Axis].Actions;

            bool fresh = intent.Axis != _strokeAxis || intent.FromTime != _strokeExpectedAt;
            if (fresh && !CaptureStroke(intent, actions))
                return EditResult.Pass; // anchor not on the axis (drag desync) — let native resolve

            double totalDt = intent.Time - _strokeAnchorAt;
            int totalDp = intent.Pos - _strokeAnchorPos;

            // Reject the whole frame's move if the weighted targets — incoming run, anchor, outgoing run —
            // aren't strictly time-ordered or would touch the actions just outside the stroke (the only way
            // differing shifts could merge two actions onto one time). Walk the whole run left to right.
            double prev = _strokePrevAt;
            bool ok = true;
            for (int i = 0; i < _strokePre.Count && ok; i++)
            {
                double to = _strokePre[i].OrigAt + _strokePre[i].Weight * totalDt;
                if (to <= prev) ok = false;
                prev = to;
            }
            if (ok)
            {
                if (intent.Time <= prev) ok = false;
                prev = intent.Time;
            }
            for (int i = 0; i < _strokePost.Count && ok; i++)
            {
                double to = _strokePost[i].OrigAt + _strokePost[i].Weight * totalDt;
                if (to <= prev) ok = false;
                prev = to;
            }
            if (!ok || prev >= _strokeNextAt)
            {
                // Keep tracking the anchor by time so the drag survives; native moves it alone this frame and
                // the stroke catches here. (Pass, never Drop — Drop would desync the timeline's drag state.)
                _strokeExpectedAt = intent.Time;
                return EditResult.Pass;
            }

            // MoveActionEvents key on time, so emit in the order that never overwrites a not-yet-moved point:
            // build the run left to right, then reverse when this frame slides it later so the rightmost
            // moves first. A pinned (zero-weight) point never moves, so it is skipped entirely.
            var moves = new List<EditIntent>(_strokePre.Count + _strokePost.Count + 1);
            foreach (var p in _strokePre)
                if (p.Weight != 0.0) moves.Add(StrokeMove(intent.Axis, p, totalDt, totalDp));
            moves.Add(EditIntent.MovePoint(intent.Axis, intent.FromTime, intent.Time, intent.Pos));
            foreach (var p in _strokePost)
                if (p.Weight != 0.0) moves.Add(StrokeMove(intent.Axis, p, totalDt, totalDp));
            if (totalDt >= _strokeLastTotalDt)
                moves.Reverse(); // slides later — emit rightmost first

            _strokeLastTotalDt = totalDt;
            _strokeExpectedAt = intent.Time;
            return EditResult.Replace(moves);
        }

        // Capture the strokes running through the dragged point — the run leading into it and the run leading
        // out — and each point's weight for the active mode. Returns false if the anchor isn't on the axis.
        // Called on the first frame of a drag, when the axis still holds the original (un-moved) geometry.
        private bool CaptureStroke(EditIntent intent, IReadOnlyList<ScriptAction> actions)
        {
            int anchor = IndexAt(actions, intent.FromTime);
            if (anchor < 0)
                return false;

            int start = StrokeStartIndex(actions, anchor); // incoming turning point
            int end = StrokeEndIndex(actions, anchor);      // outgoing turning point

            _strokeAxis = intent.Axis;
            _strokeAnchorAt = actions[anchor].At;
            _strokeAnchorPos = actions[anchor].Pos;
            _strokeLastTotalDt = 0.0;
            _strokePrevAt = start > 0 ? actions[start - 1].At : double.NegativeInfinity;
            _strokeNextAt = end < actions.Count - 1 ? actions[end + 1].At : double.PositiveInfinity;

            _strokePre.Clear();
            double preSpan = anchor - start; // 0 only when there is no preceding point
            for (int i = start; i < anchor; i++)
            {
                double t = preSpan > 0 ? (i - start) / preSpan : 0.0; // 0 at the incoming turning point, →1 toward the anchor
                _strokePre.Add(new StrokePoint
                {
                    OrigAt = actions[i].At,
                    OrigPos = actions[i].Pos,
                    Weight = StrokeWeight(S.MoveMode, t),
                });
            }

            _strokePost.Clear();
            double postSpan = end - anchor; // 0 only when there is no following point
            for (int i = anchor + 1; i <= end; i++)
            {
                double t = postSpan > 0 ? (end - i) / postSpan : 0.0; // 0 at the outgoing turning point, →1 toward the anchor
                _strokePost.Add(new StrokePoint
                {
                    OrigAt = actions[i].At,
                    OrigPos = actions[i].Pos,
                    Weight = StrokeWeight(S.MoveMode, t),
                });
            }
            return true;
        }

        // One carried point's move to its absolute target: original spot plus its weighted share of the
        // anchor's total drag. fromAt re-derives where it currently sits (original + weight × the last
        // committed total) — the same expression that placed it, so it addresses the right action exactly.
        private EditIntent StrokeMove(StandardAxis axis, StrokePoint p, double totalDt, int totalDp)
        {
            double fromAt = p.OrigAt + p.Weight * _strokeLastTotalDt;
            double toAt = p.OrigAt + p.Weight * totalDt;
            int toPos = Math.Clamp((int)Math.Round(p.OrigPos + p.Weight * totalDp), 0, 100);
            return EditIntent.MovePoint(axis, fromAt, toAt, toPos);
        }

        // The share of the anchor's drag a stroke point takes, by mode, given its normalized distance t from
        // its near turning point (0) to the anchor (1). Rigid moves the whole stroke as one; Taper fades
        // linearly so the turning point stays pinned; Smooth (smoothstep) eases at both ends for the most
        // organic bend.
        private static double StrokeWeight(StrokeMoveMode mode, double t) => mode switch
        {
            StrokeMoveMode.Taper => t,
            StrokeMoveMode.Smooth => t * t * (3.0 - 2.0 * t),
            _ => 1.0, // Rigid
        };

        // First index of the run (same direction, holds included) leading into `anchor` — its turning point,
        // or 0 at the axis start. dir == 0 (anchor on a flat) degrades to the contiguous hold.
        private static int StrokeStartIndex(IReadOnlyList<ScriptAction> a, int anchor)
        {
            int dir = DirInto(a, anchor);
            int mid = anchor - 1;
            while (mid > 0 && Continues(a, mid, dir)) mid--;
            return mid < 0 ? 0 : mid; // anchor is the first action — no preceding stroke
        }

        // Last index of the run (same direction, holds included) leading out of `anchor` — its turning point,
        // or the last action at the axis end. Returns `anchor` itself when it is the last action (no run out).
        private static int StrokeEndIndex(IReadOnlyList<ScriptAction> a, int anchor)
        {
            if (anchor >= a.Count - 1) return anchor;
            int dir = DirFrom(a, anchor);
            int end = anchor + 1;
            while (end < a.Count - 1 && Continues(a, end + 1, dir)) end++;
            return end;
        }

        // Sign of the most recent real (non-flat) step starting at or after index i; 0 only if every step
        // from i..end is a hold. Lets the stroke walk see through plateaus to the true outgoing direction.
        private static int DirFrom(IReadOnlyList<ScriptAction> a, int i)
        {
            for (int k = i; k < a.Count - 1; k++)
            {
                int s = Math.Sign(a[k + 1].Pos - a[k].Pos);
                if (s != 0) return s;
            }
            return 0;
        }

        // Index of the action at exactly `at`, or -1. The drag feeds back the time it last moved the anchor
        // to (bit-identical to the stored action), so an exact match is correct here.
        private static int IndexAt(IReadOnlyList<ScriptAction> a, double at)
        {
            int lo = 0, hi = a.Count - 1;
            while (lo <= hi)
            {
                int m = (lo + hi) >> 1;
                double v = a[m].At;
                if (v == at) return m;
                if (v < at) lo = m + 1; else hi = m - 1;
            }
            return -1;
        }

        // The active selection mode's resolver. On a box it keeps only the stroke's turning points among
        // the in-range actions — tops, bottoms, or both per Settings.PeakSelect — so a marquee grabs the
        // stroke skeleton instead of every point. Select-all and click Pass through to native — there is
        // nothing peak-shaped to reinterpret about "everything" or a single clicked point. The host runs
        // this once per editable axis (group fan-out is always per-axis for selection), each call reading
        // that axis's own actions; it only ever selects existing points, never edits.
        private SelectResult OnSelectPeaks(SelectRequest req)
        {
            if (req.Gesture != SelectGesture.Box) return SelectResult.Pass;

            bool wantTops = S.PeakSelect != PeakKind.Bottoms;
            bool wantBottoms = S.PeakSelect != PeakKind.Tops;

            var actions = Host.Axes[req.Axis].Actions;
            var peaks = new List<ScriptAction>();
            // The whole axis is scanned (endpoints included — see IsTurningPoint); only the box's time
            // range gates what's kept. Neighbours may sit just outside the box, so the test stays correct
            // right at the selection's edges.
            for (int i = 0; i < actions.Count; i++)
            {
                var a = actions[i];
                if (a.At < req.StartTime || a.At > req.EndTime) continue;
                if (IsTurningPoint(actions, i, wantTops, wantBottoms))
                    peaks.Add(a);
            }
            return SelectResult.Replace(peaks);
        }

        // Whether action `i` is a wanted turning point. Compares against the nearest neighbours that
        // actually differ in position, so a flat-topped (or flat-bottomed) peak is recognised instead of
        // being dropped because an equal neighbour fails a strict `>` test — every point on the plateau
        // shares the same differing neighbours, so the whole flat peak is kept.
        // Endpoints are judged on the one side they have: the first/last action counts as a peak when it
        // stands above (top) or below (bottom) its single differing neighbour, so a script that opens at
        // the top or closes at the bottom still has that endpoint recognised. The only non-peak is a point
        // with no differing neighbour at all — i.e. a wholly flat axis.
        private static bool IsTurningPoint(IReadOnlyList<ScriptAction> a, int i, bool wantTops, bool wantBottoms)
        {
            int pos = a[i].Pos;
            int left = i - 1;
            while (left >= 0 && a[left].Pos == pos) left--;
            int right = i + 1;
            while (right < a.Count && a[right].Pos == pos) right++;

            bool hasLeft = left >= 0;
            bool hasRight = right < a.Count;
            if (!hasLeft && !hasRight) return false; // every action shares this position — flat axis, no peak

            // A missing side is treated as "agrees", so the verdict rests on the side(s) actually present;
            // an interior point still needs both sides to agree. isTop and isBottom are mutually exclusive
            // (a present neighbour differs, so it can't be both above and below).
            bool isTop = (!hasLeft || a[left].Pos < pos) && (!hasRight || a[right].Pos < pos);
            bool isBottom = (!hasLeft || a[left].Pos > pos) && (!hasRight || a[right].Pos > pos);
            return (isTop && wantTops) || (isBottom && wantBottoms);
        }

        // The active navigator's resolver. It redefines only the active-axis Action channel (the ↓/↑
        // prev/next-action keys): step to the next/previous turning point instead of the adjacent action,
        // using the same Settings.PeakSelect that the selection mode uses. The overlay-grid (Frame) and
        // all-axes channels Pass to native. Returns None (swallow) when there is no further peak that way,
        // so a key at the last peak doesn't fall back to native and jump to a non-peak.
        private Nav OnPeakStep(NavStep step)
        {
            if (step.Granularity != NavGranularity.Action) return Nav.Pass;

            bool wantTops = S.PeakSelect != PeakKind.Bottoms;
            bool wantBottoms = S.PeakSelect != PeakKind.Tops;

            var actions = Host.Axes[Host.Axes.Active].Actions;
            double target = Host.Player.Time;
            bool found = false;
            // A held-repeat burst stands for several rapid steps; walk that many peaks in one seek.
            for (int n = 0; n < step.Reps; n++)
            {
                int next = NextPeakIndex(actions, target, (int)step.Direction, wantTops, wantBottoms);
                if (next < 0) break; // no further peak that way — stop where the last step landed
                target = actions[next].At;
                found = true;
            }
            return found ? Nav.Seek(target) : Nav.None;
        }

        // Index of the first turning point strictly after (dir > 0) or before (dir < 0) `time`; -1 if none.
        private static int NextPeakIndex(IReadOnlyList<ScriptAction> a, double time, int dir,
                                         bool wantTops, bool wantBottoms)
        {
            if (dir > 0)
            {
                for (int i = 0; i < a.Count; i++)
                    if (a[i].At > time && IsTurningPoint(a, i, wantTops, wantBottoms))
                        return i;
            }
            else
            {
                for (int i = a.Count - 1; i >= 0; i--)
                    if (a[i].At < time && IsTurningPoint(a, i, wantTops, wantBottoms))
                        return i;
            }
            return -1;
        }

        // -----------------------------------------------------------------------

        private EditIntent[] AlternatingPoints(StandardAxis role, double time, int requestedPos)
        {
            // Always context-sensitive: the previous point is the state. A point in the bottom half is
            // followed by the top extreme and vice-versa; with no previous point, start at the top.
            bool nextIsTop = Host.Axes[role].Before(time) is not { } prev || prev.Pos <= 50;

            // The pressed key sets the stroke amplitude, mirrored around the midpoint: tapping 80 alternates
            // 80/20, 100 alternates 100/0, 50 holds at 50. A fixed range, when enabled, overrides the key.
            int top = S.UseFixedRange ? S.FixedTop : Math.Max(requestedPos, 100 - requestedPos);
            int bottom = S.UseFixedRange ? S.FixedBottom : Math.Min(requestedPos, 100 - requestedPos);

            return new[] { EditIntent.AddPoint(role, time, nextIsTop ? top : bottom) };
        }

        // Shape the approach to the value you pressed. Instead of a straight ramp from the previous
        // point to the target, inject ApproachSteps points along an eased position curve over linear
        // time, so the timeline script line actually bends toward the target.
        private EditIntent[] ShapedApproachPoints(StandardAxis role, double time, int requestedPos)
        {
            // No anchor to approach from, or a flat move: nothing to shape — just place the point.
            if (Host.Axes[role].Before(time) is not { } prev || prev.Pos == requestedPos)
                return new[] { EditIntent.AddPoint(role, time, requestedPos) };

            // gamma > 1 lingers near the start then snaps late (ease-in); gamma < 1 snaps early then
            // settles (ease-out); gamma == 1 is the straight ramp. Driven by the selected shape preset.
            double gamma = kShapeGammas[Math.Clamp(S.ApproachShapeIdx, 0, kShapeGammas.Length - 1)];
            int steps = Math.Clamp(S.ApproachSteps, 1, 8);
            var pts = new List<EditIntent>(steps + 1);
            for (int i = 1; i <= steps; i++)
            {
                double f = i / (double)(steps + 1); // evenly spaced fraction of the time span
                double at = prev.At + (time - prev.At) * f;
                int pos = (int)Math.Round(prev.Pos + (requestedPos - prev.Pos) * Math.Pow(f, gamma));
                pts.Add(EditIntent.AddPoint(role, at, Math.Clamp(pos, 0, 100)));
            }
            pts.Add(EditIntent.AddPoint(role, time, requestedPos));
            return pts.ToArray();
        }

        // ── Selection / editing commands ────────────────────────────────────────

        /// <summary>Redistributes the selected points evenly in time between the first and
        /// last, keeping their positions. With no selection, equalizes the point nearest the
        /// playhead together with its two neighbours.</summary>
        private void Equalize()
        {
            var role = Host.Axes.Active;
            var axis = Host.Axes[role];
            var sel = axis.Selection;

            if (sel.Count >= 3)
            {
                EqualizeActions(axis, sel, keepSelected: true);
            }
            else if (sel.Count == 0
                     && axis.ClosestTo(Host.Player.Time) is { } closest
                     && axis.Before(closest.At) is { } before
                     && axis.After(closest.At) is { } after)
            {
                EqualizeActions(axis, new[] { before, closest, after }, keepSelected: false);
            }
            // 1–2 points selected: nothing meaningful to equalize.
        }

        private static void EqualizeActions(Axis axis, IReadOnlyList<ScriptAction> pts, bool keepSelected)
        {
            int n = pts.Count;
            double first = pts[0].At, last = pts[n - 1].At;
            double step = (last - first) / (n - 1);

            // Endpoints stay put; only the interior points are respaced.
            var moved = new ScriptAction[n];
            for (int i = 0; i < n; i++)
            {
                double at = (i == 0 || i == n - 1) ? pts[i].At : first + i * step;
                moved[i] = new ScriptAction(at, pts[i].Pos);
            }

            var edit = axis.Edit();
            foreach (var p in pts) edit.RemoveAt(p.At);
            edit.AddRange(moved);
            if (keepSelected)
            {
                edit.ClearSelection();
                foreach (var m in moved) edit.Select(m.At);
            }
            edit.Commit();
        }

        /// <summary>Flips the position (100 − pos) of every selected point, or of the point
        /// nearest the playhead when nothing is selected. Selection is preserved.</summary>
        private void Invert()
        {
            var role = Host.Axes.Active;
            var axis = Host.Axes[role];
            var sel = axis.Selection;

            var edit = axis.Edit();
            if (sel.Count > 0)
            {
                foreach (var a in sel) edit.Add(a.At, 100 - a.Pos);
            }
            else if (axis.ClosestTo(Host.Player.Time) is { } closest)
            {
                edit.Add(closest.At, 100 - closest.Pos);
            }
            edit.Commit();
        }

        /// <summary>Removes the points immediately before and after the point nearest the
        /// playhead, leaving it isolated.</summary>
        private void Isolate()
        {
            var role = Host.Axes.Active;
            var axis = Host.Axes[role];
            if (axis.ClosestTo(Host.Player.Time) is not { } closest) return;

            var prev = axis.Before(closest.At);
            var next = axis.After(closest.At);
            if (prev is null && next is null) return;

            var edit = axis.Edit();
            if (prev is { } p) edit.RemoveAt(p.At);
            if (next is { } nx) edit.RemoveAt(nx.At);
            edit.Commit();
        }
    }
}
