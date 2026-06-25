using System;
using System.Collections.Generic;
using System.Text.Json;
using System.Threading.Tasks;
using Ofs;

namespace Ofs.ProbePlugin;

// TState for the "stateoffset" node — covers a scalar, an enum (string-persisted), and another plain
// field. Every field round-trips through nodeState and is visible to eval.
public enum OffsetMode { Add = 0, Sub = 1 }

public struct OffsetState
{
    public float Offset;
    public OffsetMode Mode;
    public float Scratch; // persisted and seen by eval like any other field
}

// Stateless generator state for the registration round-trip probe (pnode).
public struct ProbeGenState { }

// State for the "prepmod" factory probe — a single field the factory reads when it builds its closure.
public struct PrepProbeState { public float Base; }

// State for the "custombump" probe, mutated only through a custom `ui` callback. The
// dispatch test never runs the ui callback (no ImGui in plugin-tests); it verifies that a ui-bearing
// registration still captures and evals its JSON state (Bumps) on the worker path.
public struct BumpState { public int Bumps; }

// Stateless state for the "cancelprobe" node — a discrete modifier whose eval blocks on the worker and
// polls NodeContext.IsCancelled, so a C++ test can cancel the in-flight job and observe the plugin
// noticing. Discrete on purpose: only the discrete eval path carries the cancel token.
public struct CancelProbeState { }

// Per-project data probe. PUBLIC FIELDS (no properties), the same shape the shipped CorePlugin.Settings
// uses, so it proves ProjectScoped<T> serializes public fields. A class (not a struct) so
// ProjectScoped's `Value.N = ...` mutates the stored instance in place.
public sealed class DataProbe
{
    public int N;
    public bool Flag;
}

// Observability probe for the real-.NET dispatch tests (tests/plugins/test_plugin_dispatch.cpp).
//
// A C++ unit test cannot peek into managed memory, so this plugin makes every host->plugin callback
// observable by translating it into a Host.Player.Seek(value) — which the host turns into a
// SeekEvent the test captures. The seek "value" is an encoded channel (see the *Base constants):
// the integer thousands pick the callback, the remainder carries the argument. Counting captured
// seeks in a channel's range proves how many times (and with what data) a callback fired.
//
// Unlike Ofs.TestPlugin, this plugin is only ever staged for the dispatch test's own pref dir, so
// its always-on event->seek subscriptions never perturb the ui-tests.
public sealed class ProbePlugin : OfsPlugin
{
    // Lifecycle / event channels (callback fired -> Seek(base + arg)).
    public const double PlayBase = 1000;    // + (playing ? 1 : 0)
    public const double SpeedBase = 2000;   // + speed
    public const double MediaBase = 3000;   // + mediaPath.Length
    public const double ProjectMark = 4000; // exact (no arg)
    public const double AxisModBase = 5000; // + role
    public const double ActiveBase = 6000;  // + role (Active is never null)
    public const double TimeBase = 7000;    // + time
    public const double UpdateMark = 8000;  // exact each frame (count = onUpdate cadence)
    public const double NullRole = 99;      // "no video" sentinel (Player.VideoWidth); see StVideoDims

    // "state" command read-back channels (one Seek per host value read through the managed wrappers).
    public const double StTime = 10000;       // + Host.Player.Time
    public const double StPlaying = 11000;    // + (IsPlaying ? 1 : 0)
    public const double StSpeed = 12000;      // + round(Speed * 100)
    public const double StExisting = 13000;   // + Axes.Existing.Count
    public const double StActionCount = 14000;// + Axes[L0].Actions.Count
    public const double StActive = 15000;     // + Active (never null)
    public const double StFirstPos = 16000;   // + Axes[L0].Actions[0].Pos
    public const double StSecondPos = 17000;  // + Axes[L0].Actions[1].Pos

    // Cross-frame AxisEdit staleness probe (B4): commit of an edit begun on an earlier frame.
    public const double StaleRejected = 41000;  // a cross-frame AxisEdit.Commit threw (expected)
    public const double StaleCommitted = 41001; // a cross-frame commit unexpectedly succeeded (bug)

    // Command markers.
    public const double PingMark = 22000;        // firePluginCommand reached onCommand
    public const double WorkerOk = 23000;        // host read from a worker thread unexpectedly succeeded
    public const double WorkerRejected = 23001;  // host read from a worker thread was rejected (expected)

    // "state2" command read-back channels for the axis/project/player additions (one Seek per value).
    public const double StVisible = 24000;    // + (Axes[L0].IsVisible ? 1 : 0)
    public const double StLocked = 25000;     // + (Axes[L0].IsLocked ? 1 : 0)
    public const double StName = 27000;       // + Axes[L0].Name.Length
    public const double StVolume = 28000;     // + round(Player.Volume * 100)
    public const double StFps = 29000;        // + (Player.Fps ?? 0)
    public const double StVideoDims = 30000;  // + (Player.VideoWidth ?? NullRole)  — NullRole proves "no video"
    public const double StDirty = 31000;          // + (Project.IsDirty ? 1 : 0)
    public const double StChapters = 32000;       // + Project.Chapters.Count
    public const double StBookmarks = 33000;      // + Project.Bookmarks.Count
    public const double StRegions = 34000;        // + Project.Regions.Count
    public const double StTitle = 35000;          // + Project.Metadata["title"].GetString().Length
    public const double StTagCount = 36000;       // + Project.Metadata["tags"].GetArrayLength()
    public const double StPerformerCount = 37000; // + Project.Metadata["performers"].GetArrayLength()
    public const double StCustomCount = 38000;    // + count of non-standard top-level metadata keys

    // "cancelreport" command channels — both are monotonic counters so a test can baseline-and-wait
    // (they survive across fixtures if the managed ALC hasn't unloaded yet, so absolute values mean nothing).
    public const double CancelStartedBase = 39000;  // + times the cancelprobe eval entered its loop
    public const double CancelObservedBase = 40000; // + times the cancelprobe eval saw ctx.IsCancelled and bailed

    // Per-project data store channel (Host.Project.Scoped).
    public const double StScopedN = 45000;  // + ProjectScoped<DataProbe>("scoped").Value.N — reload/reset per project
    // App settings store channel (Host.AppScoped) — global, persisted to <pref>/plugin_settings/<plugin>.json.
    public const double StAppScopedN = 68000; // + AppScoped<DataProbe>("appscoped").Value.N

    // ── Host-surface probes (Host.cs): one Seek per value read through the managed host wrappers ──
    public const double IsMainMark = 54000;      // + (Host.IsMainThread ? 1 : 0)
    public const double UnloadLive = 55000;      // + (UnloadToken not yet cancelled ? 1 : 0)
    public const double RunFire = 56000;         // RunOnMainThread fire-and-forget marker (runs on the next pump)
    public const double RunAsyncAction = 57000;  // RunOnMainThreadAsync(Action) work marker
    public const double RunAsyncFunc = 58000;    // RunOnMainThreadAsync(Func<T>) work marker
    public const double CultureInvariant = 59000;// + (Host.Culture is InvariantCulture ? 1 : 0)

    // ── Player additions (Player.cs) — getters/setters the "state" probes don't touch ──
    public const double StDuration2 = 60000; // + Host.Player.Duration
    public const double StMediaLen = 61000;  // + Host.Player.MediaPath.Length
    public const double StVideoH = 62000;    // + (Host.Player.VideoHeight ?? NullRole)

    // ── Dialogs additions (Dialogs.cs) ──
    public const double Dialogs2Ran = 63000; // dialogs2 command ran to completion (SaveFile + PickFolder queued)

    // ── Axis read/edit API (Axes.cs: AxisMath lookups + buffered AxisEdit) ──
    public const double AxisApiOk = 64000; // + count of "axisapi" sub-checks that passed (expect AxisApiChecks)
    public const int AxisApiChecks = 11;   // total sub-checks the "axisapi" command runs
    // Cross-frame Axis snapshot staleness (Axis.CheckFresh): reading a stashed Axis a frame later must throw.
    public const double FreshRejected = 65000; // a cross-frame Axis.Actions read threw (expected)
    public const double FreshLived = 65001;    // a cross-frame Axis read unexpectedly succeeded (bug)
    // Reading an axis with more actions than the scratch buffer holds (4096) must grow it and still
    // return every row (Axes.EnsureScratch).
    public const double ScratchOk = 66000; // + (Axes[L0].Actions.Count > 4096 ? 1 : 0)
    // A plugin may only touch existing axes: indexing or activating an absent role must throw, not return
    // an empty view / silently no-op.
    public const double AbsentRejected = 67000; // + count of absent-axis sub-checks that threw (expect AbsentChecks)
    public const int AbsentChecks = 2;          // Axes[absent] and SetActive(absent)

    // ── Axis ScriptAction-overload + range mutators (Axes.cs) ──
    // The convenience overloads that take a ScriptAction (delegating to the double counterpart) and the
    // buffered SelectRange/DeselectRange; "axisapi2" reports a passed-check count, "axisselrange" commits a
    // selection edit whose net result the test reads off the SetAxisSelectionEvent.
    public const double AxisApi2Ok = 69000; // + count of "axisapi2" sub-checks that passed (expect AxisApi2Checks)
    public const int AxisApi2Checks = 7;    // ScriptAction overloads (Axis + AxisEdit) + buffered RemoveAt

    // ── EditIntent factories + Editing.StepTime (Editing.cs) ──
    public const double EditIntentsOk = 70000; // + count of "editintents" factory checks that passed (expect EditIntentsChecks)
    public const int EditIntentsChecks = 14;   // every EditIntent.* factory (incl. ScriptAction overloads)
    public const double StepTimeRan = 71000;   // "editintents" read Host.Editing.StepTime without throwing

    public override string Name => "Probe Plugin";

    protected override void OnLoad()
    {
        // Every host event -> an encoded seek.
        Host.Player.PlayingChanged += p => Host.Player.Seek(PlayBase + (p ? 1 : 0));
        Host.Player.SpeedChanged += s => Host.Player.Seek(SpeedBase + s);
        Host.Player.MediaChanged += m => Host.Player.Seek(MediaBase + (m?.Length ?? 0));
        Host.Player.TimeChanged += t => Host.Player.Seek(TimeBase + t);
        Host.Axes.ProjectChanged += () => Host.Player.Seek(ProjectMark);
        Host.Axes.Modified += r => Host.Player.Seek(AxisModBase + (int)r);
        Host.Axes.ActiveChanged += r => Host.Player.Seek(ActiveBase + (int)r); // Active is never null

        // Reads live host state through the managed Player/Axes wrappers and reports each value.
        Host.Commands.Register("state", "Probe State", ProbeState);

        // Reads the axis/project/player ADDITIONS through their managed wrappers and reports each value.
        Host.Commands.Register("state2", "Probe State 2", ProbeState2);

        // Axis writes: each pushes a host event the dispatch test captures.
        Host.Commands.Register("setactive", "Probe SetActive", () => Host.Axes.SetActive(StandardAxis.R0));

        // Exercises the managed AxisEdit -> CommitAxisActions path; the test captures CommitAxisActionsEvent.
        Host.Commands.Register("commit", "Probe Commit", () =>
            Host.Axes[StandardAxis.L0].Edit().Add(0.5, 10).Add(1.5, 90).Commit());

        // B4 staleness probe: "editstash" begins+keeps an edit (no commit); "commitstash" tries to commit
        // it later. With a frame advanced in between, the commit must be rejected (the edit is stale) so it
        // can't replay its buffered snapshot over intervening changes.
        Host.Commands.Register("editstash", "Probe Edit Stash", () =>
            _stashedEdit = Host.Axes[StandardAxis.L0].Edit().Add(0.5, 10));
        Host.Commands.Register("commitstash", "Probe Commit Stash", () =>
        {
            try
            {
                _stashedEdit?.Commit();
                Host.Player.Seek(StaleCommitted);
            }
            catch (InvalidOperationException) { Host.Player.Seek(StaleRejected); }
        });

        // Observable target for the firePluginCommand dispatch test.
        Host.Commands.Register("ping", "Probe Ping", () => Host.Player.Seek(PingMark));

        // A host read marshalled onto a worker thread must be rejected by the managed main-thread guard.
        // The marker seek itself runs back on the main thread (after Wait), since Seek is guarded too.
        Host.Commands.Register("worker", "Probe Worker", () =>
        {
            bool rejected = false;
            Task.Run(() =>
            {
                try { _ = Host.Player.Time; }
                catch (InvalidOperationException) { rejected = true; }
            }).Wait();
            Host.Player.Seek(rejected ? WorkerRejected : WorkerOk);
        });

        // A command listed on no surface (neither rebind list nor palette) is undiscoverable: Register must throw.
        try
        {
            Host.Commands.Register("dead", "Dead", () => { }, inRebindList: false, inPalette: false);
            _deadRegistered = true; // should be unreachable
        }
        catch (ArgumentException) { /* expected — command not registered */ }

        // Localization probes: a command title and a node display name built from the active language code
        // at OnLoad. The host reloads the plugin on a UI-language switch, so OnLoad re-runs and these
        // re-register — a C++ test reads the registry and sees "…-en" become "…-ja". Host.Language is read
        // on the main thread at registration.
        Host.Commands.Register("langcmd", $"Cmd-{Host.Language}", () => { });
        Host.Nodes.AddNode<ProbeGenState>("langnode", $"Node-{Host.Language}",
            new NodeShape(inputs: [], outputs: ["out"]),
            static (double t, ReadOnlySpan<float> ins, in ProbeGenState s, NodeContext ctx, Span<float> outs) =>
                outs[0] = 0.5f);

        // A custom node so the test can confirm registerNode round-trips into the native effect registry —
        // including the author-declared group + icon + description (group/icon override the default
        // plugin-name bucket / arity icon; description is the add-menu hover tooltip).
        Host.Nodes.AddNode<ProbeGenState>("pnode", "Probe Gen",
            new NodeShape(inputs: [], outputs: ["out"]),
            static (double t, ReadOnlySpan<float> ins, in ProbeGenState s, NodeContext ctx, Span<float> outs) =>
                outs[0] = 0.5f,
            group: "Probes", icon: NodeIcon.Waveform, description: "Probe node description");

        // A NodeShape with a repeated input name is ambiguous: the constructor must throw, so the node is
        // never registered (a C++ test asserts "dupinnode" is absent from the effect registry).
        try
        {
            Host.Nodes.AddNode<ProbeGenState>("dupinnode", "Dup In",
                new NodeShape(inputs: ["x", "x"], outputs: ["out"]),
                static (double t, ReadOnlySpan<float> ins, in ProbeGenState s, NodeContext ctx, Span<float> outs) =>
                    outs[0] = 0.5f);
            _dupNodeRegistered = true; // should be unreachable
        }
        catch (ArgumentException) { /* expected — duplicate input pin name */ }

        // A TState modifier exercising the JSON codec end to end (test_plugin_dispatch.cpp). The eval reads
        // a float, an enum (persisted by name), and another field — all round-trip through nodeState and
        // reach the worker. output = input ± (Offset + Scratch).
        Host.Nodes.AddNode<OffsetState>("stateoffset", "State Offset",
            new NodeShape(inputs: ["in"], outputs: ["out"]),
            static (double t, ReadOnlySpan<float> ins, in OffsetState s, NodeContext ctx, Span<float> outs) =>
            {
                float d = s.Offset + s.Scratch;
                outs[0] = s.Mode == OffsetMode.Sub ? ins[0] - d : ins[0] + d;
            });

        // A factory (prepare) modifier proving the "runs once per region eval" contract to a C++
        // test (test_plugin_dispatch.cpp). The factory bakes a process-wide build counter into its
        // closure and the closure returns that constant for EVERY sample, ignoring the input. So within
        // one eval all outputs are identical (the factory ran once, not once per sample), and the value
        // increments by exactly one per eval (it re-runs once each eval). output = Base + buildCount.
        Host.Nodes.AddNode<PrepProbeState>("prepmod", "Prepare Probe",
            new NodeShape(inputs: ["in"], outputs: ["out"]),
            prepare: static (in PrepProbeState s, NodeContext ctx) =>
            {
                int build = System.Threading.Interlocked.Increment(ref s_prepareBuilds);
                float baked = s.Base + build;
                return (double t, ReadOnlySpan<float> ins, Span<float> outs) => outs[0] = baked;
            });

        // A TState modifier with a custom `ui` callback — its onNodeUi hook exists because of the
        // ui callback. The dispatch test confirms the registration still captures and
        // evals its JSON state (output = input + Bumps); the ui callback (drawn label/buttons, with a
        // deferred Node.Update) is never invoked without an ImGui context.
        Host.Nodes.AddNode<BumpState>("custombump", "Custom Bump",
            new NodeShape(inputs: ["in"], outputs: ["out"]),
            static (double t, ReadOnlySpan<float> ins, in BumpState s, NodeContext ctx, Span<float> outs) =>
                outs[0] = ins[0] + s.Bumps,
            (Ui ui, ref BumpState s) =>
            {
                ui.Label($"bumps: {s.Bumps}");
                if (ui.Button("Bump")) s.Bumps++;
                if (ui.Button("Async Bump")) _ = AsyncBump(ui.Node());
            });

        // Cooperative-cancellation probe (test_plugin_dispatch.cpp). A discrete modifier whose eval blocks
        // on the worker thread and polls ctx.IsCancelled at every loop turn, bailing the instant the host
        // cancels the job. It bumps s_cancelStarted on entry and s_cancelObserved when it sees cancellation;
        // the "cancelreport" command surfaces both counters so the C++ test can baseline-wait on them. The
        // wall-clock cap bounds the eval if the test never cancels (the assertion then fails, not hangs).
        Host.Nodes.AddNode<CancelProbeState>("cancelprobe", "Cancel Probe",
            new NodeShape(inputs: ["in"], outputs: ["out"]),
            static (ReadOnlySpan<DiscreteReader> ins, in CancelProbeState s, NodeContext ctx,
                    ReadOnlySpan<DiscreteWriter> outs) =>
            {
                System.Threading.Interlocked.Increment(ref s_cancelStarted);
                var sw = System.Diagnostics.Stopwatch.StartNew();
                while (!ctx.IsCancelled && sw.ElapsedMilliseconds < 5000)
                    System.Threading.Thread.Sleep(5);
                if (ctx.IsCancelled)
                    System.Threading.Interlocked.Increment(ref s_cancelObserved);
                foreach (var a in ins[0]) outs[0].Add(a); // passthrough so an uncancelled eval still resolves
            });

        // Surfaces the cancellation-probe counters as encoded seeks (read on the main thread; the worker
        // writes them with Interlocked, so a Volatile.Read here sees the latest value).
        Host.Commands.Register("cancelreport", "Probe Cancel Report", () =>
        {
            Host.Player.Seek(CancelStartedBase + System.Threading.Volatile.Read(ref s_cancelStarted));
            Host.Player.Seek(CancelObservedBase + System.Threading.Volatile.Read(ref s_cancelObserved));
        });

        // Per-project data store probe (test_plugin_dispatch.cpp).
        // ProjectScoped: load now, reload on each project open, save on change.
        _data = Host.Project.Scoped<DataProbe>("scoped");
        Host.Commands.Register("scopedread", "Probe Scoped Read", () => Host.Player.Seek(StScopedN + _data.Value.N));
        Host.Commands.Register("scopededit", "Probe Scoped Edit", () => { _data.Value.N = 3; });

        // AppScoped: load now (from this plugin's global settings file), save on change. Unlike scoped, it
        // does not reset/reload per project — it's app-global.
        _appData = Host.AppScoped<DataProbe>("appscoped");
        Host.Commands.Register("appscopedread", "Probe App Scoped Read",
            () => Host.Player.Seek(StAppScopedN + _appData.Value.N));
        Host.Commands.Register("appscopededit", "Probe App Scoped Edit", () => { _appData.Value.N = 5; });

        // Interaction extension points (test_plugin_dispatch.cpp drives these through the routers).
        // Edit mode: AddPoint → Replace with the same point shifted +1 in position (an observable
        // transform); RemovePoint → Drop (nothing applied); everything else → Pass (native). onEnter/
        // onExit bump counters a test can read via the "intentlifecycle" command.
        Host.Editing.RegisterMode("intentmode", "Probe Intent Mode", intent =>
        {
            return intent.Kind switch
            {
                EditIntentKind.AddPoint => EditResult.Replace(
                    EditIntent.AddPoint(intent.Axis, intent.Time, intent.Pos + 1)),
                EditIntentKind.RemovePoint => EditResult.Drop,
                _ => EditResult.Pass,
            };
        },
        onEnter: () => s_modeEnters++,
        onExit: () => s_modeExits++);

        Host.Commands.Register("intentlifecycle", "Probe Intent Lifecycle", () =>
        {
            Host.Player.Seek(ModeEnterBase + System.Threading.Volatile.Read(ref s_modeEnters));
            Host.Player.Seek(ModeExitBase + System.Threading.Volatile.Read(ref s_modeExits));
            Host.Player.Seek(NavEnterBase + System.Threading.Volatile.Read(ref s_navEnters));
            Host.Player.Seek(NavExitBase + System.Threading.Volatile.Read(ref s_navExits));
            Host.Player.Seek(SelEnterBase + System.Threading.Volatile.Read(ref s_selEnters));
            Host.Player.Seek(SelExitBase + System.Threading.Volatile.Read(ref s_selExits));
        });

        // Per-axis edit mode: AddPoint → ReplacePerAxis with a pos computed from the (retargeted) axis
        // (L0→80, R0→20, else its index). The host re-consults this mode once per editable axis with that
        // axis substituted, so a C++ test sees each axis get its own value — not the lead's projected one.
        // Everything else → Pass.
        Host.Editing.RegisterMode("peraxismode", "Probe PerAxis Mode", intent =>
        {
            if (intent.Kind != EditIntentKind.AddPoint) return EditResult.Pass;
            int pos = intent.Axis == StandardAxis.L0 ? 80
                    : intent.Axis == StandardAxis.R0 ? 20
                    : (int)intent.Axis;
            return EditResult.ReplacePerAxis(EditIntent.AddPoint(intent.Axis, intent.Time, pos));
        });

        // Multi-intent edit mode: one AddPoint click → a Replace emitting THREE points on the *same* axis
        // (the click plus two trailing points). The host resolves each emitted intent in turn, so a C++
        // test confirms a Replace can carry several same-axis intents: all apply, in submission order, and
        // the host stamps undo coalescing across them (snapshot true on the first, false on the rest — a
        // single undo step). Other kinds → Pass.
        Host.Editing.RegisterMode("multimode", "Probe Multi Intent", intent =>
        {
            if (intent.Kind != EditIntentKind.AddPoint) return EditResult.Pass;
            return EditResult.Replace(
                EditIntent.AddPoint(intent.Axis, intent.Time, 10),
                EditIntent.AddPoint(intent.Axis, intent.Time + 1.0, 20),
                EditIntent.AddPoint(intent.Axis, intent.Time + 2.0, 30));
        });

        // Navigator: redefine the Frame channel (next steps to 42s, prev to 7s, so a C++ test reads the
        // resolved SeekEvent back unambiguously) and the ActionAllAxes channel (seek to 55s AND activate
        // R0, exercising the explicit axis-activation result field). Pass every other granularity, proving
        // the per-granularity native fallback works. onEnter/onExit bump counters read via "intentlifecycle".
        Host.Navigation.RegisterMode("fixedstep", "Probe Fixed Step",
            step => step.Granularity switch
            {
                NavGranularity.Frame => Nav.Seek(step.Direction == StepDirection.Forward ? 42.0 : 7.0),
                NavGranularity.ActionAllAxes => Nav.Seek(55.0, StandardAxis.R0),
                _ => Nav.Pass,
            },
            onEnter: () => s_navEnters++,
            onExit: () => s_navExits++);

        // Selection mode: on a Box gesture, Replace with only the in-range actions whose position is >= 50
        // (the plugin enumerates candidates itself off Host.Axes), so a C++ test sees the low points dropped
        // vs. native's "select everything". All → Drop (select nothing, an observable difference from native);
        // every other gesture → Pass (native). onEnter/onExit bump counters read via "intentlifecycle".
        Host.Selection.RegisterMode("probeselect", "Probe Select High", req =>
        {
            switch (req.Gesture)
            {
                case SelectGesture.Box:
                    var kept = new List<ScriptAction>();
                    foreach (var a in Host.Axes[req.Axis].Actions)
                        if (a.At >= req.StartTime && a.At <= req.EndTime && a.Pos >= 50)
                            kept.Add(a);
                    return SelectResult.Replace(kept);
                case SelectGesture.All:
                    return SelectResult.Drop;
                default:
                    return SelectResult.Pass;
            }
        },
        onEnter: () => s_selEnters++,
        onExit: () => s_selExits++);

        // Surfaces IsActive for all three extension points so a C++ test can confirm each query tracks
        // only its own active selection (the host prepends the plugin name to each local id).
        Host.Commands.Register("intentactive", "Probe Interaction Active", () =>
        {
            Host.Player.Seek(EditActiveBase + (Host.Editing.IsActive("intentmode") ? 1 : 0));
            Host.Player.Seek(NavActiveBase + (Host.Navigation.IsActive("fixedstep") ? 1 : 0));
            Host.Player.Seek(SelActiveBase + (Host.Selection.IsActive("probeselect") ? 1 : 0));
        });

        // ── Host-surface probes (Host.cs): toasts, misc host services, main-thread marshaling, culture ──
        Host.Commands.Register("notify", "Probe Notify", () =>
        {
            Host.NotifyInfo("probe info");
            Host.NotifySuccess("probe ok");
            Host.NotifyWarning("probe warn");
            Host.NotifyError("probe err");
            Host.Notify(NotifyLevel.Info, "probe raw");
        });

        Host.Commands.Register("hostmisc", "Probe Host Misc", () =>
        {
            Host.Player.Seek(IsMainMark + (Host.IsMainThread ? 1 : 0));
            Host.Player.Seek(UnloadLive + (Host.UnloadToken.IsCancellationRequested ? 0 : 1));
            Host.Log("probe log line");                    // Log(string) → Log(Info, …)
            Host.Log(LogLevel.Warning, "probe log warn");
        });

        // RunOnMainThread + both RunOnMainThreadAsync overloads. The work runs when the host pumps the
        // queue at the next OnUpdate, so a C++ test fires this then ticks a frame to observe the markers.
        Host.Commands.Register("runmain", "Probe Run Main", () =>
        {
            Host.RunOnMainThread(() => Host.Player.Seek(RunFire));
            _ = Host.RunOnMainThreadAsync(() => Host.Player.Seek(RunAsyncAction));
            _ = Host.RunOnMainThreadAsync(() => { Host.Player.Seek(RunAsyncFunc); return 5; });
        });

        Host.Commands.Register("culture", "Probe Culture", () =>
            Host.Player.Seek(CultureInvariant +
                (Equals(Host.Culture, System.Globalization.CultureInfo.InvariantCulture) ? 1 : 0)));

        // ── Player additions (Player.cs): getters/setters + TogglePlay the "state" probes don't hit ──
        Host.Commands.Register("player2", "Probe Player2", () =>
        {
            Host.Player.Seek(StDuration2 + Host.Player.Duration);
            Host.Player.Seek(StMediaLen + Host.Player.MediaPath.Length);
            Host.Player.Seek(StVideoH + (Host.Player.VideoHeight ?? (int)NullRole));
            Host.Player.IsPlaying = true;
            Host.Player.Speed = 1.25f;
            Host.Player.Volume = 0.5f;
            Host.Player.TogglePlay();
        });

        // ── Dialogs additions (Dialogs.cs): SaveFile + PickFolder queue a modal and return a Task. In the
        // headless test no one answers, but the managed marshaling runs; the trailing Seek confirms no throw.
        Host.Commands.Register("dialogs2", "Probe Dialogs2", () =>
        {
            _ = Host.Dialogs.SaveFile("Save", "out.funscript", "*.funscript", "Funscript");
            _ = Host.Dialogs.PickFolder("Pick Folder");
            Host.Player.Seek(Dialogs2Ran);
        });

        // Exercises the pure-logic axis read/edit surface (Axes.cs): AxisMath.ClosestTo across its
        // branches, IsSelected on a miss, the buffered AxisEdit lookups/mutators (Add(ScriptAction),
        // Actions/Before/After, Clear), the empty-selection commit (→ ClearAxisSelection), and the
        // spent-edit guard. L0 is seeded {1.0,40},{2.0,60} by the test. Reports the passed-check count.
        Host.Commands.Register("axisapi", "Probe Axis API", () =>
        {
            int ok = 0;
            var ax = Host.Axes[StandardAxis.L0];

            // ClosestTo: exact-At hit, before the first, after the last, and the two nearer-neighbour ties.
            if (ax.ClosestTo(1.0)?.Pos == 40) ok++;
            if (ax.ClosestTo(0.0)?.Pos == 40) ok++;
            if (ax.ClosestTo(9.0)?.Pos == 60) ok++;
            if (ax.ClosestTo(1.4)?.Pos == 40) ok++; // 0.4 vs 0.6 → nearer the earlier
            if (ax.ClosestTo(1.7)?.Pos == 60) ok++; // 0.7 vs 0.3 → nearer the later
            if (!ax.IsSelected(1.0)) ok++;          // nothing selected → lookup misses

            // Buffered edit: never committed, so it's a clean no-op on the project.
            var e = ax.Edit();
            e.Add(new ScriptAction(3.0, 70)); // Add(ScriptAction) → Set
            if (e.Actions.Count == 3) ok++;   // buffered view reflects the insert
            if (e.Before(2.0)?.Pos == 40) ok++;
            if (e.After(1.0)?.Pos == 60) ok++;
            e.Clear();
            if (e.Actions.Count == 0) ok++;

            // A selection edit whose net selection is empty but dirty → Commit takes the ClearAxisSelection
            // branch (emits SetAxisSelectionEvent with an empty selection). The edit is spent afterwards, so
            // a further mutator must throw.
            var e2 = ax.Edit();
            e2.Select(1.0).Deselect(1.0);
            e2.Commit();
            try { e2.Add(5.0, 50); }
            catch (InvalidOperationException) { ok++; }

            Host.Player.Seek(AxisApiOk + ok);
        });

        // Cross-frame Axis staleness: stash the snapshot now, read it a frame later — Axis.CheckFresh
        // must throw rather than hand back another frame's rows.
        Host.Commands.Register("freshstash", "Probe Fresh Stash", () => _stashedAxis = Host.Axes[StandardAxis.L0]);
        Host.Commands.Register("freshcheck", "Probe Fresh Check", () =>
        {
            try
            {
                _ = _stashedAxis!.Actions;
                Host.Player.Seek(FreshLived);
            }
            catch (InvalidOperationException) { Host.Player.Seek(FreshRejected); }
        });

        // Reads a deliberately over-large L0 (seeded by the test) so the indexer's scratch buffer has to
        // grow; the read must still surface every action.
        Host.Commands.Register("scratchgrow", "Probe Scratch Grow", () =>
            Host.Player.Seek(ScratchOk + (Host.Axes[StandardAxis.L0].Actions.Count > 4096 ? 1 : 0)));

        // Absent axis: a plugin can only touch axes that exist (it cannot create one). Both indexing and
        // activating a role that doesn't exist (S0 in the test project — no data, not shown, not locked)
        // must throw InvalidOperationException instead of yielding an empty view / silent no-op.
        Host.Commands.Register("absentaxis", "Probe Absent Axis", () =>
        {
            int threw = 0;
            try { _ = Host.Axes[StandardAxis.S0]; } catch (InvalidOperationException) { threw++; }
            try { Host.Axes.SetActive(StandardAxis.S0); } catch (InvalidOperationException) { threw++; }
            Host.Player.Seek(AbsentRejected + threw);
        });

        // Exercises the ScriptAction-typed convenience overloads (Axes.cs) — each just delegates to the
        // double counterpart, so it must agree with the matching "axisapi" check. Plus the buffered
        // AxisEdit.RemoveAt(ScriptAction). L0 is seeded {1.0,40},{2.0,60} by the test (same as axisapi).
        Host.Commands.Register("axisapi2", "Probe Axis API 2", () =>
        {
            int ok = 0;
            var ax = Host.Axes[StandardAxis.L0];
            var a1 = new ScriptAction(1.0, 0); // pos is irrelevant to the lookups (keyed on At)
            var a2 = new ScriptAction(2.0, 0);

            if (ax.ClosestTo(a1)?.Pos == 40) ok++; // ClosestTo(ScriptAction)
            if (ax.Before(a2)?.Pos == 40) ok++;    // Before(ScriptAction)
            if (ax.After(a1)?.Pos == 60) ok++;     // After(ScriptAction)
            if (!ax.IsSelected(a1)) ok++;          // IsSelected(ScriptAction) on a miss

            var e = ax.Edit();
            if (e.Before(a2)?.Pos == 40) ok++; // AxisEdit.Before(ScriptAction)
            if (e.After(a1)?.Pos == 60) ok++;  // AxisEdit.After(ScriptAction)
            e.RemoveAt(a2);                    // AxisEdit.RemoveAt(ScriptAction)
            if (e.Actions.Count == 1) ok++;
            // uncommitted → a clean no-op on the project (only the buffered view changed)

            Host.Player.Seek(AxisApi2Ok + ok);
        });

        // Commits a buffered selection edit built with the range + ScriptAction-overload selection mutators
        // (SelectRange/DeselectRange/Select(ScriptAction)/Deselect(ScriptAction)). The net selection is read
        // off the resulting SetAxisSelectionEvent by the test. L0 is seeded {1.0,40},{2.0,60},{3.0,55}.
        Host.Commands.Register("axisselrange", "Probe Axis Sel Range", () =>
        {
            var e = Host.Axes[StandardAxis.L0].Edit();
            e.SelectRange(1.0, 3.0);              // selects 1.0, 2.0, 3.0
            e.Select(new ScriptAction(1.0, 0));   // already selected → overload is a no-op
            e.DeselectRange(2.5, 3.5);            // drops 3.0
            e.Deselect(new ScriptAction(1.0, 0)); // drops 1.0 via the overload → net {2.0}
            e.Commit();
        });

        // Builds an EditIntent through every static factory (Editing.cs) and verifies the field the factory
        // sets, counting passes. Pure construction — no host routing needed. Also reads Editing.StepTime.
        Host.Commands.Register("editintents", "Probe Edit Intents", () =>
        {
            int ok = 0;
            var sa = new ScriptAction(4.0, 33);

            var add = EditIntent.AddPoint(StandardAxis.L0, 1.0, 50);
            if (add.Kind == EditIntentKind.AddPoint && add.Time == 1.0 && add.Pos == 50) ok++;
            var addSa = EditIntent.AddPoint(StandardAxis.L0, sa); // ScriptAction overload
            if (addSa.Time == 4.0 && addSa.Pos == 33) ok++;
            var ahp = EditIntent.AddPointAtPlayhead(70);
            if (ahp.Kind == EditIntentKind.AddPointAtPlayhead && ahp.Pos == 70) ok++;

            var mv = EditIntent.MovePoint(StandardAxis.R0, 1.0, 2.0, 60);
            if (mv.Kind == EditIntentKind.MovePoint && mv.FromTime == 1.0 && mv.Time == 2.0 && mv.Pos == 60) ok++;
            var mvSa = EditIntent.MovePoint(StandardAxis.R0, 1.0, sa); // ScriptAction overload
            if (mvSa.FromTime == 1.0 && mvSa.Time == 4.0 && mvSa.Pos == 33) ok++;

            var rm = EditIntent.RemovePoint(StandardAxis.L0, 3.0);
            if (rm.Kind == EditIntentKind.RemovePoint && rm.Time == 3.0) ok++;
            var rmSa = EditIntent.RemovePoint(StandardAxis.L0, sa); // ScriptAction overload
            if (rmSa.Time == 4.0) ok++;

            var rmSel = EditIntent.RemoveSelected(StandardAxis.L0);
            if (rmSel.Kind == EditIntentKind.RemoveSelected && rmSel.Axis == StandardAxis.L0) ok++;

            var mvPos = EditIntent.MoveSelectionByPos(StandardAxis.L0, -5);
            if (mvPos.Kind == EditIntentKind.MoveSelection && mvPos.Direction == StepDirection.None && mvPos.Pos == -5) ok++;
            var mvFwd = EditIntent.MoveSelectionByTime(StandardAxis.L0, StepDirection.Forward, 3, seekAfter: true);
            if (mvFwd.Direction == StepDirection.Forward && mvFwd.Reps == 3 && mvFwd.SeekAfter) ok++;
            var mvBack = EditIntent.MoveSelectionByTime(StandardAxis.L0, StepDirection.Backward);
            if (mvBack.Direction == StepDirection.Backward && mvBack.Reps == 1) ok++;
            var mvClamp = EditIntent.MoveSelectionByTime(StandardAxis.L0, StepDirection.None, reps: 0);
            if (mvClamp.Direction == StepDirection.Forward && mvClamp.Reps == 1) ok++; // None→Forward, reps clamped to ≥1

            var paste = EditIntent.Paste(2.5, exact: true);
            if (paste.Kind == EditIntentKind.Paste && paste.Time == 2.5 && paste.Exact) ok++;
            var pasteSa = EditIntent.Paste(sa); // ScriptAction overload, default exact=false
            if (pasteSa.Time == 4.0 && !pasteSa.Exact) ok++;

            Host.Player.Seek(EditIntentsOk + ok);
            _ = Host.Editing.StepTime; // read the host-backed getter (no throw on the main thread)
            Host.Player.Seek(StepTimeRan);
        });

        // Edit modes returning the IEnumerable (non-params) Replace / ReplacePerAxis overloads — the params
        // overloads are covered by multimode/peraxismode; these drive the List-taking siblings. An AddPoint
        // gesture routed through each must apply the listed intents.
        Host.Editing.RegisterMode("listmode", "Probe List Replace", intent =>
        {
            if (intent.Kind != EditIntentKind.AddPoint) return EditResult.Pass;
            var intents = new List<EditIntent>
            {
                EditIntent.AddPoint(intent.Axis, intent.Time, 11),
                EditIntent.AddPoint(intent.Axis, intent.Time + 1.0, 22),
            };
            return EditResult.Replace(intents); // IEnumerable overload
        });
        Host.Editing.RegisterMode("peraxislistmode", "Probe PerAxis List", intent =>
        {
            if (intent.Kind != EditIntentKind.AddPoint) return EditResult.Pass;
            var intents = new List<EditIntent> { EditIntent.AddPoint(intent.Axis, intent.Time, (int)intent.Axis + 1) };
            return EditResult.ReplacePerAxis(intents); // IEnumerable overload
        });

        // ── Node-shape coverage (Nodes.cs): a params-reading modifier, a discrete modifier that reads its
        // input by Count/index and writes via Add(at,pos), functional + factory combiners, a factory
        // generator, and a discrete generator/combiner. Evaluated by the NodeStateFixture dispatch cases.
        Host.Nodes.AddNode<ProbeGenState>("paramprobe", "Probe Params",
            new NodeShape(inputs: ["in"], outputs: ["out"]),
            static (double t, ReadOnlySpan<float> ins, in ProbeGenState s, NodeContext ctx, Span<float> outs) =>
                outs[0] = ins[0] + ctx.Params.Length + ctx.Param(0, 0f)); // plugin nodes carry no scalar params → +0

        Host.Nodes.AddNode<ProbeGenState>("discmodcount", "Probe Disc Mod Count",
            new NodeShape(inputs: ["in"], outputs: ["out"]),
            static (ReadOnlySpan<DiscreteReader> ins, in ProbeGenState s, NodeContext ctx,
                    ReadOnlySpan<DiscreteWriter> outs) =>
            {
                var input = ins[0];
                for (int i = 0; i < input.Count; i++) // exercises Count + the indexer + Add(at,pos)
                {
                    var act = input[i];
                    outs[0].Add(act.At, act.Pos);
                }
            });

        Host.Nodes.AddNode<ProbeGenState>("combo", "Probe Combiner",
            new NodeShape(inputs: ["a", "b"], outputs: ["out"]),
            static (double t, ReadOnlySpan<float> ins, in ProbeGenState s, NodeContext ctx, Span<float> outs) =>
                outs[0] = (ins[0] + ins[1]) / 2f);

        Host.Nodes.AddNode<ProbeGenState>("prepcomb", "Probe Prepare Combiner",
            new NodeShape(inputs: ["a", "b"], outputs: ["out"]),
            prepare: static (in ProbeGenState s, NodeContext ctx) =>
                (double t, ReadOnlySpan<float> ins, Span<float> outs) => outs[0] = (ins[0] + ins[1]) / 2f);

        Host.Nodes.AddNode<ProbeGenState>("prepgen", "Probe Prepare Gen",
            new NodeShape(inputs: [], outputs: ["out"]),
            prepare: static (in ProbeGenState s, NodeContext ctx) =>
                (double t, ReadOnlySpan<float> ins, Span<float> outs) => outs[0] = 0.5f);

        Host.Nodes.AddNode<ProbeGenState>("discgen2", "Probe Disc Gen",
            new NodeShape(inputs: [], outputs: ["out"]),
            static (ReadOnlySpan<DiscreteReader> ins, in ProbeGenState s, NodeContext ctx,
                    ReadOnlySpan<DiscreteWriter> outs) => outs[0].Add(0.0, 50));

        Host.Nodes.AddNode<ProbeGenState>("disccomb", "Probe Disc Combiner",
            new NodeShape(inputs: ["a", "b"], outputs: ["out"]),
            static (ReadOnlySpan<DiscreteReader> ins, in ProbeGenState s, NodeContext ctx,
                    ReadOnlySpan<DiscreteWriter> outs) =>
            {
                foreach (var act in ins[0]) outs[0].Add(act); // Add(ScriptAction) overload
                foreach (var act in ins[1]) outs[0].Add(act);
            });
    }

    // onEnter/onExit counters for all three extension points, surfaced by the "intentlifecycle" command.
    public const double ModeEnterBase = 46000;   // + times the probe edit mode's onEnter ran
    public const double ModeExitBase = 47000;    // + times the probe edit mode's onExit ran
    public const double NavEnterBase = 51000;    // + times the probe navigator's onEnter ran
    public const double NavExitBase = 52000;     // + times the probe navigator's onExit ran
    public const double SelEnterBase = 53000;    // + times the probe selection mode's onEnter ran
    public const double SelExitBase = 54000;     // + times the probe selection mode's onExit ran
    private static int s_modeEnters;
    private static int s_modeExits;
    private static int s_navEnters;
    private static int s_navExits;
    private static int s_selEnters;
    private static int s_selExits;

    // IsActive(localId) for each extension point, surfaced by the "intentactive" command (+ 1 if the
    // probe's own mode/navigator/selection mode is the one the user has active, else + 0).
    public const double EditActiveBase = 48000;
    public const double NavActiveBase = 49000;
    public const double SelActiveBase = 50000;

    // Per-project scoped state for the data-store dispatch tests; wired in OnLoad.
    private ProjectScoped<DataProbe> _data = null!;

    // App-global scoped state (Host.AppScoped) for the app-settings dispatch tests; wired in OnLoad.
    private AppScoped<DataProbe> _appData = null!;

    // Cancellation-probe counters (process-wide): loop entries, and times the eval observed cancellation.
    private static int s_cancelStarted;
    private static int s_cancelObserved;

    // Holds an uncommitted edit between the "editstash" and "commitstash" commands (B4 staleness probe).
    private AxisEdit? _stashedEdit;

    // Holds an axis snapshot between "freshstash" and "freshcheck" (cross-frame Axis.CheckFresh probe).
    private Axis? _stashedAxis;

    // Set only if the dead-command Register failed to throw — asserts the guard never silently passes.
    private bool _deadRegistered;
    public bool DeadRegistered => _deadRegistered;

    // Set only if the duplicate-input NodeShape failed to throw — asserts the guard never silently passes.
    private bool _dupNodeRegistered;
    public bool DupNodeRegistered => _dupNodeRegistered;

    // How many times the "prepmod" factory has built its closure this process — one per region eval if
    // the once-per-eval caching holds. Baked into the closure output for the dispatch test to read.
    private static int s_prepareBuilds;

    private void ProbeState()
    {
        Host.Player.Seek(StTime + Host.Player.Time);
        Host.Player.Seek(StPlaying + (Host.Player.IsPlaying ? 1 : 0));
        Host.Player.Seek(StSpeed + Math.Round(Host.Player.Speed * 100));
        Host.Player.Seek(StExisting + Host.Axes.Existing.Count);

        var l0 = Host.Axes[StandardAxis.L0];
        Host.Player.Seek(StActionCount + l0.Actions.Count);
        if (l0.Actions.Count >= 2)
        {
            Host.Player.Seek(StFirstPos + l0.Actions[0].Pos);
            Host.Player.Seek(StSecondPos + l0.Actions[1].Pos);
        }
        Host.Player.Seek(StActive + (int)Host.Axes.Active); // Active is never null
    }

    private void ProbeState2()
    {
        var l0 = Host.Axes[StandardAxis.L0];
        Host.Player.Seek(StVisible + (l0.IsVisible ? 1 : 0));
        Host.Player.Seek(StLocked + (l0.IsLocked ? 1 : 0));
        Host.Player.Seek(StName + l0.Name.Length);

        Host.Player.Seek(StVolume + Math.Round(Host.Player.Volume * 100));
        Host.Player.Seek(StFps + (Host.Player.Fps ?? 0));
        Host.Player.Seek(StVideoDims + (Host.Player.VideoWidth ?? (int)NullRole));

        Host.Player.Seek(StDirty + (Host.Project.IsDirty ? 1 : 0));
        Host.Player.Seek(StChapters + Host.Project.Chapters.Count);
        Host.Player.Seek(StBookmarks + Host.Project.Bookmarks.Count);
        Host.Player.Seek(StRegions + Host.Project.Regions.Count);

        var meta = Host.Project.Metadata;
        if (meta.ValueKind == JsonValueKind.Object)
        {
            Host.Player.Seek(StTitle + (meta.TryGetProperty("title", out var title)
                ? title.GetString()?.Length ?? 0 : 0));
            Host.Player.Seek(StTagCount + (meta.TryGetProperty("tags", out var tags)
                ? tags.GetArrayLength() : 0));
            Host.Player.Seek(StPerformerCount + (meta.TryGetProperty("performers", out var perf)
                ? perf.GetArrayLength() : 0));
            int custom = 0;
            foreach (var prop in meta.EnumerateObject())
                if (!IsStandardMetadataKey(prop.Name)) custom++;
            Host.Player.Seek(StCustomCount + custom);
        }
    }

    private static bool IsStandardMetadataKey(string key) =>
        key is "title" or "creator" or "scriptUrl" or "videoUrl" or
               "description" or "notes" or "tags" or "performers" or "license";

    // Deferred-write path for the custombump ui callback — present for compile/registration
    // coverage; never reached by the dispatch test (the ui callback isn't invoked without ImGui).
    private static async Task AsyncBump(Node node)
    {
        await Task.Yield();
        node.Update((ref BumpState s) => s.Bumps += 10);
    }

    // onUpdate fires every frame regardless of the time threshold — one marker per frame.
    protected override void OnUpdate(float deltaSeconds) => Host.Player.Seek(UpdateMark);
}
