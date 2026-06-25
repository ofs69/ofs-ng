using System.Numerics;
using System.Threading;
using System.Threading.Tasks;
using Ofs;

namespace Ofs.TestPlugin;

// Minimal plugin used by test_plugin_load.cpp to validate the real CoreCLR load
// path end to end: the host boots the runtime, reflects this type out of the DLL,
// instantiates it, and invokes OnLoad — which registers a command back through the
// native HostApi bridge. The test then observes "Ofs.TestPlugin.ping" in the
// native CommandRegistry, proving the full round-trip works.
public sealed class TestPlugin : OfsPlugin
{
    public override string Name => "Test Plugin";

    protected override void OnLoad()
    {
        Host.Commands.Register("ping", "Ping", () => { });

        // A bindable command so the binding round-trip test (ui-tests) can assign a trigger to a
        // plugin-created command and verify it survives save/reload and a disable/re-enable cycle.
        Host.Commands.Register("nudge", "Nudge", () => { }, inRebindList: true);

        // A command with an observable side-effect (seek to a distinct second) so a ui-test can invoke
        // it through the live command palette and confirm the managed handler actually ran.
        Host.Commands.Register("seekmark", "Seek Mark", () => Host.Player.Seek(7.0), inRebindList: true);

        // A bindable but palette-HIDDEN command (inPalette: false). A ui-test confirms its distinctive
        // title never surfaces in the command palette, yet a bound key still invokes it (seek to 6). The
        // "xyzzy" token is unique so a palette search for it matches nothing unless the filter regresses.
        Host.Commands.Register("hiddenseek", "Hidden Xyzzy", () => Host.Player.Seek(6.0),
            inRebindList: true, inPalette: false);

        // Register nodes too. Their delegates land in Ofs.Api's static slot tables, which used to
        // root this assembly for the whole process — so this is what makes the unload test meaningful:
        // if the slots aren't released on unload, the AssemblyLoadContext never collects and the DLL
        // stays locked. One functional and one discrete node cover both slot paths. Both use the
        // non-generic (stateless) AddNode overload, exercising the no-TState registration path.
        Host.Nodes.AddNode("fgen", "Test Functional Gen",
            new NodeShape(inputs: [], outputs: ["out"]),
            static (double t, ReadOnlySpan<float> ins, NodeContext ctx, Span<float> outs) =>
                outs[0] = 0.5f);
        Host.Nodes.AddNode("dgen", "Test Discrete Gen",
            new NodeShape(inputs: [], outputs: ["out"]),
            static (ReadOnlySpan<DiscreteReader> ins, NodeContext ctx,
                    ReadOnlySpan<DiscreteWriter> outs) => outs[0].Add(0.0, 50));

        // A TState modifier whose eval adds Offset to the input; its `ui` callback draws an editable slider
        // for Offset. The ui-smoke node_param_ui test renders this node's body to prove the
        // onNodeUi render path runs every frame without crashing or spuriously re-dirtying, and that the
        // persisted Offset flows through capture into eval.
        Host.Nodes.AddNode<OffsetState>("stateoffset", "State Offset",
            new NodeShape(inputs: ["in"], outputs: ["out"]),
            static (double t, ReadOnlySpan<float> ins, in OffsetState s, NodeContext ctx, Span<float> outs) =>
                outs[0] = ins[0] + s.Offset,
            (Ui ui, ref OffsetState s) => ui.Slider("Offset", ref s.Offset, -100f, 100f));

        // A TState modifier with a custom `ui` callback that draws a status label and two buttons:
        // a synchronous `ref` bump, and an async one routed through a capture-safe Node handle's deferred
        // Update. eval adds the accumulated bumps to the input.
        Host.Nodes.AddNode<BumpState>("custombump", "Custom Bump",
            new NodeShape(inputs: ["in"], outputs: ["out"]),
            static (double t, ReadOnlySpan<float> ins, in BumpState s, NodeContext ctx, Span<float> outs) =>
                outs[0] = ins[0] + s.Bumps,
            (Ui ui, ref BumpState s) =>
            {
                ui.Label($"bumps: {s.Bumps}");
                if (ui.Button("Bump"))
                    s.Bumps++; // synchronous ref write — change detected by the shallow value compare
                if (ui.Button("Async Bump"))
                    _ = AsyncBump(ui.Node()); // deferred: applied on the node's next UI pass via Node.Update
            });

        // ── Deliberately faulty entry points (crash-safety coverage) ────────────────────────────────
        // Each of these throws when invoked. A managed exception escaping back into native — through a
        // reverse-P/Invoke callback or a node trampoline — is an instant process crash, so every entry
        // point is wrapped by PluginGuard on the host side. The plugin_crash ui-tests fire each one and
        // assert the host survives (logs + a throttled toast, then keeps running). The exception messages
        // are intentional so a fault in the log is recognizable as the test's, not a real bug.
        Host.Commands.Register("boom", "Boom Command", () =>
            throw new InvalidOperationException("boom: intentional command fault"), inRebindList: true);

        // Functional modifier whose eval throws: the functional trampoline catch must pass the input through.
        Host.Nodes.AddNode<EmptyState>("boommod", "Boom Modifier",
            new NodeShape(inputs: ["in"], outputs: ["out"]),
            static (double t, ReadOnlySpan<float> ins, in EmptyState s, NodeContext ctx, Span<float> outs) =>
                throw new InvalidOperationException("boommod: intentional functional-node fault"));

        // Discrete generator whose eval throws: the discrete trampoline guard must emit nothing rather
        // than flush a half-filled writer. Covers the PluginGuard.Run (discrete) path, distinct from the
        // inline try/catch the functional trampoline uses.
        Host.Nodes.AddNode<EmptyState>("boomdgen", "Boom Discrete Gen",
            new NodeShape(inputs: [], outputs: ["out"]),
            static (ReadOnlySpan<DiscreteReader> ins, in EmptyState s, NodeContext ctx,
                    ReadOnlySpan<DiscreteWriter> outs) =>
                throw new InvalidOperationException("boomdgen: intentional discrete-node fault"));

        // ── Ui-guard probes (Ui.cs thread + freshness guard) ─────────────────────────────────────────
        // OnRenderUi stashes the live Ui builder in _stashedUi; these commands then deliberately misuse it.
        // Each runs on the main thread during event drain — i.e. OUTSIDE the render pass — so the seek
        // confirms the guard threw (the builder must reject misuse, not scribble on native ImGui state).
        //   • uioffpass:   call the stashed builder on the main thread but after its pass → the freshness
        //     branch throws (message names the "render pass"). Seek 94 iff rejected, 96 if it wrongly ran.
        Host.Commands.Register("uioffpass", "UI Off Pass", () =>
        {
            if (_stashedUi == null)
                return;
            try
            {
                _stashedUi.Button("probe");
                Host.Player.Seek(96.0); // reached native — the guard failed to fire (bug)
            }
            catch (InvalidOperationException ex)
            {
                if (ex.Message.Contains("render pass"))
                    Host.Player.Seek(94.0);
            }
        }, inRebindList: true);

        //   • uioffthread: call the stashed builder from a worker thread → the thread branch throws first
        //     (message names the "main thread"). Seek 95 iff rejected, 97 if it wrongly ran.
        Host.Commands.Register("uioffthread", "UI Off Thread", () =>
        {
            if (_stashedUi == null)
                return;
            bool threadRejected = false;
            Task.Run(() =>
            {
                try { _stashedUi.Button("probe"); }
                catch (InvalidOperationException ex) { threadRejected = ex.Message.Contains("main thread"); }
            }).Wait();
            Host.Player.Seek(threadRejected ? 95.0 : 97.0);
        }, inRebindList: true);

        // Dialog cancellation (Dialogs.cs): an awaited dialog whose token is cancelled completes as
        // cancelled instead of hanging — the same path UnloadToken takes when a plugin unloads mid-dialog.
        // A pre-cancelled caller token makes it deterministic: the await throws OperationCanceledException.
        Host.Commands.Register("uidialogcancel", "UI Dialog Cancel", () =>
        {
            var cts = new CancellationTokenSource();
            cts.Cancel();
            _ = DialogCancelAsync(cts.Token);
        }, inRebindList: true);
    }

    // No-parameter node state for the test nodes above.
    public struct EmptyState { }

    // A single scalar — the node's only state — rendered by the stateoffset `ui` callback's slider.
    public struct OffsetState
    {
        public float Offset;
    }

    // State for the "custombump" node, mutated only through the custom ui callback (sync or deferred).
    // eval reads Bumps. Persists as JSON like any TState.
    public struct BumpState
    {
        public int Bumps;
    }

    // Async deferred write path: a value type can't be captured by ref across an await, so the
    // continuation captures the capture-safe Node handle and applies the mutation on the main thread.
    private static async Task AsyncBump(Node node)
    {
        await Task.Yield();
        node.Update((ref BumpState s) => s.Bumps += 10);
    }

    // Backing state for the OnRenderUi widgets below.
    private bool _flag;
    private int _level;
    private int _mode;
    private int _radio;
    private int _intVal;
    private float _floatVal;
    private int _dragInt;
    private float _dragFloat;
    private string _text = "";
    private Vector4 _color = new(0.2f, 0.4f, 0.6f, 1.0f);
    private string _multiline = "line1\nline2";

    // Hoisted out of OnRenderUi so the per-frame Combo call doesn't allocate a fresh array each frame.
    private static readonly string[] TestModes = { "Item A", "Item B", "Item C" };

    // The live Ui builder, stashed each render so the Ui-guard probe commands can call it from the wrong
    // context (off-pass / off-thread) and confirm the guard rejects the misuse.
    private Ui? _stashedUi;

    // Overriding OnRenderUi is what makes the host expose a plugin window at all (PluginBridge only wires
    // OnBuildUI when this is overridden), so this drives the managed Ui.* bridge end to end. Each widget,
    // when changed, performs an observable host action — a seek to a distinct second — so a ui-test driving
    // the live plugin window can confirm the interaction round-tripped, including the ref-value write-back
    // for the checkbox and slider.
    protected override void OnRenderUi(Ui ui)
    {
        _stashedUi = ui; // captured so the Ui-guard probe commands can call it outside this pass

        if (ui.Button("Test UI Action"))
            Host.Player.Seek(3.0);

        if (ui.Checkbox("Test Flag", ref _flag))
            Host.Player.Seek(5.0);

        if (ui.Slider("Test Level", ref _level, 0, 10))
            Host.Player.Seek(_level); // seeked second == the written-back slider value

        // Combo, drawn outside any section so the ui-test can target it by its visible label. On change it
        // seeks to (4 + selected index), so the written-back index is observable as the seeked second.
        if (ui.Combo("Test Mode", ref _mode, TestModes))
            Host.Player.Seek(4.0 + _mode);

        // Nested sections: a button two section levels deep. Buttons keep their visible label even inside a
        // section, so the ui-test can click it; reaching it proves the host nests sections (only the
        // outermost owns the form table, the inner reuses it) and the table+ID stack stays balanced.
        ui.Section("Outer Section", () =>
        {
            ui.Section("Inner Section", () =>
            {
                if (ui.Button("Nested Action"))
                    Host.Player.Seek(2.0);
            });
        });

        // Crash-safety: a button that throws *while inside a section* — the hardest case. Ui.Section pops
        // in a finally, so the host's form table stays balanced as the exception unwinds; PluginGuard then
        // catches it at the OnRenderUi boundary. The plugin_crash ui-test clicks this and confirms the host
        // survives and the window keeps rendering. The throw fires only on the click frame, so the window
        // stays interactable until the user actually presses it.
        ui.Section("Boom Section", () =>
        {
            if (ui.Button("Boom UI"))
                throw new InvalidOperationException("Boom UI: intentional render fault inside a section");
        });

        // The remaining input widgets, drawn last and outside any section so the ui-test can address each by
        // its visible label (in a form section the host hides the widget label behind "##v"). Each writes a
        // distinct, observable seek so the managed marshaling + ref write-back is exercised per widget kind.
        // They sit after Boom Section so they don't shift the positions the crash test depends on (Boom UI is
        // skipped on its throw frame, but no test needs these on that frame).
        if (ui.RadioButton("Radio To 21", ref _radio, 1))
            Host.Player.Seek(21.0);
        if (ui.RadioButton("Radio To 22", ref _radio, 2))
            Host.Player.Seek(22.0);

        if (ui.InputInt("Test Int", ref _intVal))
            Host.Player.Seek(30.0 + _intVal); // seeked second == the written-back int

        if (ui.InputFloat("Test Float", ref _floatVal, 0.5f))
            Host.Player.Seek(40.0);

        if (ui.DragInt("Test Drag Int", ref _dragInt, 1f, 0, 100))
            Host.Player.Seek(50.0);

        if (ui.DragFloat("Test Drag Float", ref _dragFloat, 0.5f, 0f, 100f))
            Host.Player.Seek(60.0);

        if (ui.InputText("Test Text", ref _text, 64))
            Host.Player.Seek(70.0);

        // Ui.Row lays its contents on one line. Reaching the button inside proves uiPushRow/uiPopRow
        // balance and that widgets within a row still render and route their interactions through the host.
        ui.Row("Test Row", () =>
        {
            if (ui.Button("Row Action"))
                Host.Player.Seek(80.0);
        });

        // The widget additions, drawn last so they never shift the positions earlier tests depend on. The
        // two interactive ones write an observable seek; ProgressBar is render-only (a ui-test confirms the
        // window keeps rendering once it's present — it owns its items and can't unbalance).
        if (ui.ColorEdit("Test Color", ref _color))
            Host.Player.Seek(90.0);
        if (ui.InputTextMultiline("Test Multiline", ref _multiline, maxBytes: 256, heightLines: 2))
            Host.Player.Seek(91.0);
        ui.ProgressBar(0.5f, "halfway");

        // Non-blocking host dialogs: each starts a dialog and seeks an observable second when it resolves
        // (on a later frame, on the main thread). A ui-test drives these via the native-dialog test seam
        // (file) and by clicking the confirm modal (confirm).
        if (ui.Button("Test Open Dialog"))
            _ = OpenDialogAsync();
        if (ui.Button("Test Open Files Dialog"))
            _ = OpenFilesDialogAsync();
        if (ui.Button("Test Confirm Dialog"))
            _ = ConfirmDialogAsync();
    }

    private async Task OpenDialogAsync()
    {
        string? path = await Host.Dialogs.OpenFile("Test Open");
        if (path != null)
            Host.Player.Seek(92.0); // observable: a non-null result resolved back on the main thread
    }

    private async Task OpenFilesDialogAsync()
    {
        string[]? paths = await Host.Dialogs.OpenFiles("Test Open Files");
        if (paths != null)
            Host.Player.Seek(80.0 + paths.Length); // observable: seeks 80 + the count of chosen files
    }

    private async Task ConfirmDialogAsync()
    {
        if (await Host.Dialogs.Confirm("Test Confirm", "Proceed?", ConfirmKind.YesNo))
            Host.Player.Seek(93.0); // observable: the user clicked Yes
    }

    // The token is pre-cancelled by the "uidialogcancel" command, so the awaited OpenFile completes as
    // cancelled and the catch fires — proving the dialog task honours its cancellation token (UnloadToken
    // rides the same DialogBridge path when a plugin unloads mid-dialog).
    private async Task DialogCancelAsync(CancellationToken token)
    {
        try { await Host.Dialogs.OpenFile("Cancel Probe", cancel: token); }
        catch (OperationCanceledException) { Host.Player.Seek(98.0); }
    }
}
