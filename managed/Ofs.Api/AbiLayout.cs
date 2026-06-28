using System;
using System.Runtime.InteropServices;

namespace Ofs
{
    // ─────────────────────────────────────────────────────────────────────────
    // Managed-side mirror of the native marshaled-layout assertions in
    // src/Services/PluginApi.h. The two files compile independently, so the C++
    // static_asserts only pin the native side; this pins the C# side to the same
    // sizes and offsets. Verify() runs once at plugin-host startup (and so on
    // every `plugins` ctest run): a drift here throws immediately with a precise
    // message instead of corrupting memory the first time the struct crosses the
    // seam. The expected numbers ARE the contract — they must match the
    // static_asserts in PluginApi.h byte-for-byte.
    // ─────────────────────────────────────────────────────────────────────────
    internal static class AbiLayout
    {
        private static bool _verified;

        // Throws InvalidOperationException on the first detected size/offset drift.
        // Idempotent: the result is cached so repeated plugin loads don't re-walk it.
        internal static void Verify()
        {
            if (_verified) return;

            // Data structs — size + the field offsets the native side pins via offsetof. ScriptAction is a
            // positional record struct (At/Pos are properties, not Marshal-nameable fields), so size alone
            // guards it — the positional declaration order + sequential layout fixes the field order, and
            // the native PluginAction offsetof asserts pin the C++ side.
            Size<ScriptAction>(16);

            Size<OfsEvalCtx>(40);
            Offsets<OfsEvalCtx>(("RegionStart", 0), ("RegionEnd", 8), ("Params", 16), ("ParamCount", 24),
                ("StateHandle", 28), ("CancelFlag", 32));

            Size<OfsCommandDef>(32);
            Offsets<OfsCommandDef>(("Id", 0), ("Group", 8), ("Title", 16), ("InRebindList", 24), ("InPalette", 28));

            Size<OfsNodeDef>(96);
            Offsets<OfsNodeDef>(("Id", 0), ("DisplayName", 8), ("InputNames", 16), ("OutputNames", 24),
                ("Fn", 32), ("UserData", 40), ("OnNodeUi", 48), ("Signal", 56), ("InputCount", 60),
                ("OutputCount", 64), ("HasState", 68), ("Group", 72), ("Icon", 80), ("Description", 88));

            Size<OfsEditIntent>(48);
            Offsets<OfsEditIntent>(("Kind", 0), ("Axis", 4), ("Time", 8), ("FromTime", 16), ("Pos", 24),
                ("Direction", 28), ("Reps", 32), ("Exact", 36), ("SeekAfter", 40));

            Size<OfsNavIntent>(24);
            Offsets<OfsNavIntent>(("Direction", 0), ("Reps", 4), ("Time", 8), ("Granularity", 16), ("Axis", 20));

            Size<OfsEditModeDef>(56);
            Offsets<OfsEditModeDef>(("Id", 0), ("DisplayName", 8), ("OnEditIntent", 16), ("OnEnter", 24),
                ("OnExit", 32), ("OnUi", 40), ("UserData", 48));

            Size<OfsNavigatorDef>(56);
            Offsets<OfsNavigatorDef>(("Id", 0), ("DisplayName", 8), ("OnStep", 16), ("OnEnter", 24),
                ("OnExit", 32), ("OnUi", 40), ("UserData", 48));

            Size<OfsSelectRequest>(32);
            Offsets<OfsSelectRequest>(("Gesture", 0), ("Axis", 4), ("StartTime", 8), ("EndTime", 16),
                ("Pos", 24));

            Size<OfsSelectModeDef>(56);
            Offsets<OfsSelectModeDef>(("Id", 0), ("DisplayName", 8), ("OnSelect", 16), ("OnEnter", 24),
                ("OnExit", 32), ("OnUi", 40), ("UserData", 48));

            // Function-pointer tables — size only (the native side has no offsetof asserts on these). A
            // table is the layout most likely to drift: every new HostApi function appends a pointer, so a
            // mirror that adds it out of order reads every later pointer at the wrong offset. Pinning the
            // size catches a missing/extra/misplaced field.
            Size<HostApi>(736);
            Size<PluginApi>(120);

            _verified = true;
        }

        private static void Size<T>(int expected) where T : struct
        {
            int actual = Marshal.SizeOf<T>();
            if (actual != expected)
                throw new InvalidOperationException(
                    $"ABI layout drift: sizeof({typeof(T).Name}) is {actual}, expected {expected}. " +
                    "Ofs.Api/PluginAbi.cs no longer matches src/Services/PluginApi.h.");
        }

        private static void Offsets<T>(params (string Field, int Expected)[] fields) where T : struct
        {
            foreach (var (field, expected) in fields)
            {
                int actual = Marshal.OffsetOf<T>(field).ToInt32();
                if (actual != expected)
                    throw new InvalidOperationException(
                        $"ABI layout drift: offsetof({typeof(T).Name}.{field}) is {actual}, expected {expected}. " +
                        "Ofs.Api/PluginAbi.cs no longer matches src/Services/PluginApi.h.");
            }
        }
    }
}
