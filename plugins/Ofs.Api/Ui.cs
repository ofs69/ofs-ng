using System;
using System.Buffers;
using System.Collections.Generic;
using System.Numerics;
using System.Text;

namespace Ofs
{
    /// <summary>
    /// Immediate-mode widgets, drawn each frame inside <see cref="OfsPlugin.OnRenderUi"/>.
    /// Boolean-returning widgets report "changed this frame"; value widgets bind through ref.
    /// <para>Conventions: widget width is host-owned — there is no pixel-size parameter; a widget fills
    /// the available width, and widgets inside <see cref="Row"/> split it evenly, so plugin UI is
    /// automatically font-, DPI- and translation-safe (use <see cref="Section"/>/<see cref="Row"/> for
    /// layout). Labels and titles are rendered verbatim — localizing them is the plugin's job.
    /// <c>maxBytes</c> parameters count UTF-8 bytes, not characters (a CJK glyph is 3 bytes, an emoji 4),
    /// so size text buffers generously for non-ASCII input.</para>
    /// </summary>
    public sealed unsafe class Ui
    {
        private readonly HostApi* _api;

        // Strings longer than this fall back to a heap allocation.
        private const int StackUtf8Max = 512;

        // The thread this Ui was constructed on. Every construction site is on the main thread (plugin
        // window setup, RegisterMode under AssertMainThread, the host-invoked node-UI trampoline), so this
        // captures the main thread without needing an OfsHost reference (the static node path has none).
        private readonly int _mainThreadId;
        // True only while the host is inside the render callback this Ui was handed to. A Ui is a cached
        // per-surface builder (or, for node UI, freshly minted per call); outside its own pass it is inert.
        private bool _inPass;

        internal Ui(HostApi* api)
        {
            _api = api;
            _mainThreadId = Environment.CurrentManagedThreadId;
        }

        // The host brackets every callback that receives a Ui with these, marking the builder live only for
        // the pass. EndPass re-arms the guard so a stashed Ui called on a later frame throws.
        internal void BeginPass() => _inPass = true;
        internal void EndPass() => _inPass = false;

        // Every widget routes through here first. Two ways a Ui call is illegal — both of which would
        // otherwise corrupt native ImGui state silently instead of failing in managed code:
        //   • off the main thread — ImGui's context is a single, non-thread-safe global; a concurrent call
        //     from a worker races the main thread's render (torn draw data / crash, no plugin in the stack).
        //   • outside the render pass — a stashed Ui called from an await-continuation, command handler, or
        //     a later frame pushes/pops into whatever ImGui is drawing then, corrupting the host's own UI.
        private void Guard(string method)
        {
            if (Environment.CurrentManagedThreadId != _mainThreadId)
                throw new InvalidOperationException(
                    $"Ui.{method} must be called on the main thread, inside the render callback. ImGui is " +
                    "single-threaded; marshal cross-thread work with Host.RunOnMainThread.");
            if (!_inPass)
                throw new InvalidOperationException(
                    $"Ui.{method} called outside its render pass. A Ui builder is valid only for the duration " +
                    "of the OnRenderUi / mode onUi callback it is passed to — do not stash it and call it later.");
        }

        /// <summary>A capture-safe handle to the node currently rendering its body UI. Grab one inside a
        /// node's <c>ui</c> callback when a deferred/async write needs to update the node's state later (a
        /// <c>TState</c> value can't be captured by <c>ref</c> across an <c>await</c>): capture the returned
        /// <see cref="Node"/> and call <see cref="Node.Update{TState}"/> when the work completes. Outside a
        /// node UI pass the handle is inert (its <c>Update</c> is a no-op).</summary>
        public Node Node()
        {
            Guard(nameof(Node));
            return new Node(_api->NodeUiCurrentKey(_api->Ctx));
        }

        // Reused across frames for Combo's item buffer. Ui is a per-plugin singleton and OnRenderUi is
        // main-thread-only, so a per-instance scratch buffer is safe and removes a per-frame allocation.
        private byte[] _comboItemBuf = Array.Empty<byte>();

        // Per-enum-type caches keyed by TEnum. Enum.GetValues is reflection plus a fresh array on every
        // call, so cache the values once; Names is a reusable label buffer refilled (with possibly-
        // localized strings) each frame. UI is main-thread-only, so these shared statics need no locking.
        private static class EnumMeta<TEnum> where TEnum : struct, Enum
        {
            public static readonly TEnum[] Values = Enum.GetValues<TEnum>();
            public static readonly string[] Names = new string[Values.Length];
        }

        private static void WriteUtf8(string s, Span<byte> buf)
        {
            int n = Encoding.UTF8.GetBytes(s, buf);
            buf[n] = 0;
        }

        /// <summary>A single line of unwrapped text. For wrapping paragraphs use <see cref="LabelWrapped"/>.</summary>
        public void Label(string text)
        {
            Guard(nameof(Label));
            int max = Encoding.UTF8.GetMaxByteCount(text.Length) + 1;
            Span<byte> buf = max <= StackUtf8Max ? stackalloc byte[StackUtf8Max] : new byte[max];
            WriteUtf8(text, buf);
            fixed (byte* p = buf) _api->UiLabel(_api->Ctx, p);
        }

        /// <summary>Word-wrapped paragraph text. Unlike <see cref="Label"/> (a single unwrapped line),
        /// this wraps to the available width — use it for sentences and disclaimers.</summary>
        public void LabelWrapped(string text)
        {
            Guard(nameof(LabelWrapped));
            int max = Encoding.UTF8.GetMaxByteCount(text.Length) + 1;
            Span<byte> buf = max <= StackUtf8Max ? stackalloc byte[StackUtf8Max] : new byte[max];
            WriteUtf8(text, buf);
            fixed (byte* p = buf) _api->UiTextWrapped(_api->Ctx, p);
        }

        /// <summary>A clickable button. Returns true on the frame it is clicked.</summary>
        public bool Button(string label)
        {
            Guard(nameof(Button));
            int max = Encoding.UTF8.GetMaxByteCount(label.Length) + 1;
            Span<byte> buf = max <= StackUtf8Max ? stackalloc byte[StackUtf8Max] : new byte[max];
            WriteUtf8(label, buf);
            fixed (byte* p = buf) return _api->UiButton(_api->Ctx, p) != 0;
        }

        /// <summary>A boolean checkbox bound to <paramref name="value"/>. Returns true on the frame it changes.</summary>
        public bool Checkbox(string label, ref bool value)
        {
            Guard(nameof(Checkbox));
            int max = Encoding.UTF8.GetMaxByteCount(label.Length) + 1;
            Span<byte> buf = max <= StackUtf8Max ? stackalloc byte[StackUtf8Max] : new byte[max];
            WriteUtf8(label, buf);
            int v = value ? 1 : 0;
            int changed;
            fixed (byte* p = buf) changed = _api->UiCheckbox(_api->Ctx, p, &v);
            value = v != 0;
            return changed != 0;
        }

        /// <summary>A float slider. <paramref name="decimals"/> (0..9) is the displayed fractional-digit
        /// count.</summary>
        public bool Slider(string label, ref float value, float min, float max, int decimals = 3)
        {
            Guard(nameof(Slider));
            int maxBytes = Encoding.UTF8.GetMaxByteCount(label.Length) + 1;
            Span<byte> buf = maxBytes <= StackUtf8Max ? stackalloc byte[StackUtf8Max] : new byte[maxBytes];
            WriteUtf8(label, buf);
            float v = value;
            int changed;
            fixed (byte* p = buf) changed = _api->UiSliderFloat(_api->Ctx, p, &v, min, max, decimals);
            value = v;
            return changed != 0;
        }

        /// <summary>An integer slider over the inclusive range <paramref name="min"/>..<paramref name="max"/>.
        /// Returns true on the frame it changes.</summary>
        public bool Slider(string label, ref int value, int min, int max)
        {
            Guard(nameof(Slider));
            int maxBytes = Encoding.UTF8.GetMaxByteCount(label.Length) + 1;
            Span<byte> buf = maxBytes <= StackUtf8Max ? stackalloc byte[StackUtf8Max] : new byte[maxBytes];
            WriteUtf8(label, buf);
            int v = value;
            int changed;
            fixed (byte* p = buf) changed = _api->UiSliderInt(_api->Ctx, p, &v, min, max);
            value = v;
            return changed != 0;
        }

        /// <summary>A dropdown over <paramref name="items"/>, bound to the selected <paramref name="index"/>.
        /// Returns true on the frame the selection changes. For an enum-bound combo, prefer the
        /// <see cref="Combo{TEnum}(string, ref TEnum, Func{TEnum, string})"/> overload.</summary>
        public bool Combo(string label, ref int index, IReadOnlyList<string> items)
        {
            Guard(nameof(Combo));
            int maxLabel = Encoding.UTF8.GetMaxByteCount(label.Length) + 1;
            Span<byte> lb = maxLabel <= StackUtf8Max ? stackalloc byte[StackUtf8Max] : new byte[maxLabel];
            WriteUtf8(label, lb);

            // items_separated_by_zeros: "item1\0item2\0item3\0" — trailing \0 terminates the scan.
            int capacity = 1;
            for (int i = 0; i < items.Count; i++)
                capacity += Encoding.UTF8.GetByteCount(items[i]) + 1;
            if (_comboItemBuf.Length < capacity) _comboItemBuf = new byte[capacity];
            byte[] ib = _comboItemBuf;
            int offset = 0;
            for (int i = 0; i < items.Count; i++)
            {
                offset += Encoding.UTF8.GetBytes(items[i], ib.AsSpan(offset));
                ib[offset++] = 0;
            }
            // The reused buffer may hold stale bytes past `offset`; a fresh `new byte[]` was zero-filled so
            // the scan stopped here on its own. Write the terminating empty string explicitly now.
            ib[offset] = 0;

            int idx = index;
            int changed;
            fixed (byte* lp = lb)
            fixed (byte* ip = ib)
                changed = _api->UiCombo(_api->Ctx, lp, &idx, ip);
            index = idx;
            return changed != 0;
        }

        /// <summary>Binds directly to an enum with a caller-supplied label per member — the localized,
        /// order-safe way to drive an enum from a combo. <paramref name="labelOf"/> maps each member to its
        /// display string (typically a <c>Str.*</c> catalog entry), so the combo follows the active UI
        /// language, and because the binding is by enum <em>value</em> (not list position) reordering or
        /// inserting members can never mismap a label. This replaces the manual pattern of a parallel
        /// ordering array plus index ↔ value bookkeeping around the
        /// <see cref="Combo(string, ref int, IReadOnlyList{string})"/> overload.</summary>
        public bool Combo<TEnum>(string label, ref TEnum value, Func<TEnum, string> labelOf)
            where TEnum : struct, Enum
        {
            var values = EnumMeta<TEnum>.Values;
            var names = EnumMeta<TEnum>.Names;
            for (int i = 0; i < values.Length; i++)
                names[i] = labelOf(values[i]);

            int idx = Array.IndexOf(values, value);
            if (idx < 0) idx = 0;

            bool changed = Combo(label, ref idx, names);
            if (changed && (uint)idx < (uint)values.Length)
                value = values[idx];
            return changed;
        }

        /// <summary>A radio button bound to a shared int: selected when <paramref name="value"/> equals
        /// <paramref name="option"/>; clicking sets <paramref name="value"/> to <paramref name="option"/>.
        /// Returns true on the frame it is clicked.</summary>
        public bool RadioButton(string label, ref int value, int option)
        {
            Guard(nameof(RadioButton));
            int max = Encoding.UTF8.GetMaxByteCount(label.Length) + 1;
            Span<byte> buf = max <= StackUtf8Max ? stackalloc byte[StackUtf8Max] : new byte[max];
            WriteUtf8(label, buf);
            int v = value;
            int clicked;
            fixed (byte* p = buf) clicked = _api->UiRadioButton(_api->Ctx, p, &v, option);
            value = v;
            return clicked != 0;
        }

        /// <summary>An integer input box. <paramref name="step"/> is the amount each -/+ button applies.
        /// Set <paramref name="stepButtons"/> to false to drop the -/+ buttons (e.g. when laying two inputs
        /// side-by-side in a narrow panel, where the fixed-size buttons would overflow).</summary>
        public bool InputInt(string label, ref int value, int step = 1, bool stepButtons = true)
        {
            Guard(nameof(InputInt));
            int max = Encoding.UTF8.GetMaxByteCount(label.Length) + 1;
            Span<byte> buf = max <= StackUtf8Max ? stackalloc byte[StackUtf8Max] : new byte[max];
            WriteUtf8(label, buf);
            int v = value;
            int changed;
            // The host shows the -/+ buttons iff the step it receives is > 0, so collapse "no buttons" to 0.
            int hostStep = stepButtons ? step : 0;
            fixed (byte* p = buf) changed = _api->UiInputInt(_api->Ctx, p, &v, hostStep);
            value = v;
            return changed != 0;
        }

        /// <summary>A float input box. The -/+ buttons appear only when <paramref name="stepButtons"/> is
        /// true and <paramref name="step"/> &gt; 0 (the amount each button applies); pass false to force
        /// them off regardless of <paramref name="step"/>. <paramref name="decimals"/> (0..9) is the
        /// displayed fractional-digit count.</summary>
        public bool InputFloat(string label, ref float value, float step = 0f, bool stepButtons = true,
            int decimals = 3)
        {
            Guard(nameof(InputFloat));
            int max = Encoding.UTF8.GetMaxByteCount(label.Length) + 1;
            Span<byte> buf = max <= StackUtf8Max ? stackalloc byte[StackUtf8Max] : new byte[max];
            WriteUtf8(label, buf);
            float v = value;
            int changed;
            float hostStep = stepButtons ? step : 0f;
            fixed (byte* p = buf) changed = _api->UiInputFloat(_api->Ctx, p, &v, hostStep, decimals);
            value = v;
            return changed != 0;
        }

        /// <summary>A draggable integer. <paramref name="speed"/> scales the drag; <paramref name="min"/>
        /// equal to <paramref name="max"/> (the default) leaves the value unbounded.</summary>
        public bool DragInt(string label, ref int value, float speed = 1f, int min = 0, int max = 0)
        {
            Guard(nameof(DragInt));
            int maxBytes = Encoding.UTF8.GetMaxByteCount(label.Length) + 1;
            Span<byte> buf = maxBytes <= StackUtf8Max ? stackalloc byte[StackUtf8Max] : new byte[maxBytes];
            WriteUtf8(label, buf);
            int v = value;
            int changed;
            fixed (byte* p = buf) changed = _api->UiDragInt(_api->Ctx, p, &v, speed, min, max);
            value = v;
            return changed != 0;
        }

        /// <summary>A draggable float. <paramref name="decimals"/> (0..9) is the displayed
        /// fractional-digit count.</summary>
        public bool DragFloat(string label, ref float value, float speed = 1f, float min = 0f, float max = 0f,
            int decimals = 3)
        {
            Guard(nameof(DragFloat));
            int maxBytes = Encoding.UTF8.GetMaxByteCount(label.Length) + 1;
            Span<byte> buf = maxBytes <= StackUtf8Max ? stackalloc byte[StackUtf8Max] : new byte[maxBytes];
            WriteUtf8(label, buf);
            float v = value;
            int changed;
            fixed (byte* p = buf) changed = _api->UiDragFloat(_api->Ctx, p, &v, speed, min, max, decimals);
            value = v;
            return changed != 0;
        }

        /// <summary>Single-line text box. <paramref name="maxBytes"/> caps the UTF-8 edit buffer
        /// (including the trailing NUL); a longer initial value is truncated to fit. Set
        /// <paramref name="password"/> to mask the text, or <paramref name="readOnly"/> to display it
        /// without allowing edits.</summary>
        public bool InputText(string label, ref string value, int maxBytes = 256, bool password = false,
            bool readOnly = false)
        {
            Guard(nameof(InputText));
            if (maxBytes < 1) maxBytes = 1;
            int maxLabel = Encoding.UTF8.GetMaxByteCount(label.Length) + 1;
            Span<byte> lb = maxLabel <= StackUtf8Max ? stackalloc byte[StackUtf8Max] : new byte[maxLabel];
            WriteUtf8(label, lb);

            // Mutable, NUL-terminated UTF-8 edit buffer; ImGui writes the edited text back into it.
            // Encoder.Convert fills only what fits (leaving room for the NUL) without splitting a
            // multi-byte char, so an over-long initial value is truncated on a valid boundary. Pooled
            // (drawn every frame the widget is visible); the host caps writes at maxBytes regardless of
            // the rented buffer's actual (possibly larger) length.
            byte[] tb = ArrayPool<byte>.Shared.Rent(maxBytes);
            try
            {
                Encoding.UTF8.GetEncoder().Convert(value.AsSpan(), tb.AsSpan(0, maxBytes - 1),
                    flush: true, out _, out int n, out _);
                tb[n] = 0;

                int changed;
                fixed (byte* lp = lb)
                fixed (byte* tp = tb)
                    changed = _api->UiInputText(_api->Ctx, lp, tp, maxBytes, password ? 1 : 0, readOnly ? 1 : 0);

                if (changed != 0)
                {
                    // The host NUL-terminates within maxBytes, so content is at most maxBytes-1 bytes.
                    // Clamp the no-NUL fallback to maxBytes-1 (not maxBytes) so a contract violation can
                    // never pull the reserved terminator slot into the decoded string.
                    int len = Array.IndexOf(tb, (byte)0, 0, maxBytes);
                    if (len < 0) len = maxBytes - 1;
                    value = Encoding.UTF8.GetString(tb, 0, len);
                }
                return changed != 0;
            }
            finally { ArrayPool<byte>.Shared.Return(tb); }
        }

        /// <summary>A horizontal separator line.</summary>
        public void Separator()
        {
            Guard(nameof(Separator));
            _api->UiSeparator(_api->Ctx);
        }

        /// <summary>Draws the widgets in <paramref name="body"/> greyed and non-interactive when
        /// <paramref name="disabled"/> is true (a no-op wrapper when false). Balanced and depth-guarded by
        /// the host — an exception thrown in <paramref name="body"/> cannot leave the block open.</summary>
        public void Disabled(bool disabled, Action body)
        {
            Guard(nameof(Disabled));
            _api->UiPushDisabled(_api->Ctx, disabled ? 1 : 0);
            try { body(); }
            finally { _api->UiPopDisabled(_api->Ctx); }
        }

        /// <summary>Like <see cref="Disabled(bool, Action)"/>, but while disabled, hovering any greyed
        /// widget in <paramref name="body"/> shows <paramref name="tooltipWhenDisabled"/> explaining why —
        /// a hover-on-demand replacement for an always-visible disclaimer line. The tooltip never appears
        /// while enabled, so pass <c>""</c> (or anything) for the enabled case.</summary>
        public void Disabled(bool disabled, string tooltipWhenDisabled, Action body)
        {
            Guard(nameof(Disabled));
            int max = Encoding.UTF8.GetMaxByteCount(tooltipWhenDisabled.Length) + 1;
            Span<byte> buf = max <= StackUtf8Max ? stackalloc byte[StackUtf8Max] : new byte[max];
            WriteUtf8(tooltipWhenDisabled, buf);
            fixed (byte* p = buf) _api->UiPushDisabledTooltip(_api->Ctx, disabled ? 1 : 0, p);
            try { body(); }
            finally { _api->UiPopDisabledTooltip(_api->Ctx); }
        }

        /// <summary>Attaches a hover tooltip to the most recently drawn widget — call immediately after it.
        /// A no-op if no widget precedes it, and it does not appear on a disabled widget (use the
        /// <see cref="Disabled(bool, string, Action)"/> overload for that).</summary>
        public void Tooltip(string text)
        {
            Guard(nameof(Tooltip));
            int max = Encoding.UTF8.GetMaxByteCount(text.Length) + 1;
            Span<byte> buf = max <= StackUtf8Max ? stackalloc byte[StackUtf8Max] : new byte[max];
            WriteUtf8(text, buf);
            fixed (byte* p = buf) _api->UiTooltip(_api->Ctx, p);
        }

        /// <summary>Groups the widgets drawn in <paramref name="body"/> under a titled heading, laid out as a
        /// 2-column label/value form. Balanced and depth-guarded by the host — an exception in
        /// <paramref name="body"/> cannot leave the section open.</summary>
        public void Section(string title, Action body)
        {
            Guard(nameof(Section));
            int max = Encoding.UTF8.GetMaxByteCount(title.Length) + 1;
            Span<byte> buf = max <= StackUtf8Max ? stackalloc byte[StackUtf8Max] : new byte[max];
            WriteUtf8(title, buf);
            fixed (byte* p = buf) _api->UiPushSection(_api->Ctx, p);
            try { body(); }
            finally { _api->UiPopSection(_api->Ctx); }
        }

        /// <summary>Lays the widgets drawn inside <paramref name="body"/> out on a single horizontal
        /// line, with an optional left-hand <paramref name="label"/>. Width-bearing widgets (sliders,
        /// inputs, drags, combos) share the row width evenly. Balanced and depth-guarded by the host —
        /// an exception thrown in <paramref name="body"/> cannot leave the row open.</summary>
        public void Row(string label, Action body)
        {
            Guard(nameof(Row));
            int max = Encoding.UTF8.GetMaxByteCount(label.Length) + 1;
            Span<byte> buf = max <= StackUtf8Max ? stackalloc byte[StackUtf8Max] : new byte[max];
            WriteUtf8(label, buf);
            fixed (byte* p = buf) _api->UiPushRow(_api->Ctx, p);
            try { body(); }
            finally { _api->UiPopRow(_api->Ctx); }
        }

        /// <summary>Color editor; each channel is 0..1. Returns true the frame it is edited. When
        /// <paramref name="alpha"/> is false the alpha channel is hidden and <c>color.W</c> is left
        /// unchanged (RGB-only editing).</summary>
        public bool ColorEdit(string label, ref Vector4 color, bool alpha = true)
        {
            Guard(nameof(ColorEdit));
            int max = Encoding.UTF8.GetMaxByteCount(label.Length) + 1;
            Span<byte> lb = max <= StackUtf8Max ? stackalloc byte[StackUtf8Max] : new byte[max];
            WriteUtf8(label, lb);
            float* rgba = stackalloc float[4];
            rgba[0] = color.X; rgba[1] = color.Y; rgba[2] = color.Z; rgba[3] = color.W;
            int changed;
            fixed (byte* p = lb) changed = _api->UiColorEdit(_api->Ctx, p, rgba, alpha ? 1 : 0);
            if (changed != 0) color = new Vector4(rgba[0], rgba[1], rgba[2], rgba[3]);
            return changed != 0;
        }

        /// <summary>Multi-line text box. <paramref name="maxBytes"/> caps the UTF-8 edit buffer (incl. the
        /// trailing NUL); <paramref name="heightLines"/> is the visible height in text lines (0 = default).
        /// Set <paramref name="readOnly"/> to display the text without allowing edits.</summary>
        public bool InputTextMultiline(string label, ref string value, int maxBytes = 4096, int heightLines = 0,
            bool readOnly = false)
        {
            Guard(nameof(InputTextMultiline));
            if (maxBytes < 1) maxBytes = 1;
            int maxLabel = Encoding.UTF8.GetMaxByteCount(label.Length) + 1;
            Span<byte> lb = maxLabel <= StackUtf8Max ? stackalloc byte[StackUtf8Max] : new byte[maxLabel];
            WriteUtf8(label, lb);

            // Mutable NUL-terminated UTF-8 edit buffer; truncated on a valid char boundary if value
            // overflows. Pooled (drawn every frame the widget is visible); the host caps writes at maxBytes
            // regardless of the rented buffer's actual (possibly larger) length.
            byte[] tb = ArrayPool<byte>.Shared.Rent(maxBytes);
            try
            {
                Encoding.UTF8.GetEncoder().Convert(value.AsSpan(), tb.AsSpan(0, maxBytes - 1),
                    flush: true, out _, out int n, out _);
                tb[n] = 0;

                int changed;
                fixed (byte* lp = lb)
                fixed (byte* tp = tb)
                    changed = _api->UiInputTextMultiline(_api->Ctx, lp, tp, maxBytes, heightLines, readOnly ? 1 : 0);

                if (changed != 0)
                {
                    // The host NUL-terminates within maxBytes, so content is at most maxBytes-1 bytes.
                    // Clamp the no-NUL fallback to maxBytes-1 (not maxBytes) so a contract violation can
                    // never pull the reserved terminator slot into the decoded string.
                    int len = Array.IndexOf(tb, (byte)0, 0, maxBytes);
                    if (len < 0) len = maxBytes - 1;
                    value = Encoding.UTF8.GetString(tb, 0, len);
                }
                return changed != 0;
            }
            finally { ArrayPool<byte>.Shared.Return(tb); }
        }

        /// <summary>A progress bar; <paramref name="fraction"/> is clamped to 0..1. Optional overlay text.</summary>
        public void ProgressBar(float fraction, string? overlay = null)
        {
            Guard(nameof(ProgressBar));
            if (overlay == null) { _api->UiProgressBar(_api->Ctx, fraction, null); return; }
            int max = Encoding.UTF8.GetMaxByteCount(overlay.Length) + 1;
            Span<byte> buf = max <= StackUtf8Max ? stackalloc byte[StackUtf8Max] : new byte[max];
            WriteUtf8(overlay, buf);
            fixed (byte* p = buf) _api->UiProgressBar(_api->Ctx, fraction, p);
        }
    }
}
