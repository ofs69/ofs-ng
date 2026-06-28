namespace Ofs
{
    /// <summary>
    /// A funscript axis role, including the trailing <see cref="Count"/> sentinel. The values are part of
    /// the frozen plugin ABI and never change.
    /// </summary>
    public enum StandardAxis
    {
        /// <summary>Linear stroke — the main up/down motion.</summary>
        L0 = 0,
        /// <summary>Linear surge — forward/back motion.</summary>
        L1 = 1,
        /// <summary>Linear sway — left/right motion.</summary>
        L2 = 2,
        /// <summary>Rotational twist.</summary>
        R0 = 3,
        /// <summary>Rotational roll.</summary>
        R1 = 4,
        /// <summary>Rotational pitch.</summary>
        R2 = 5,
        /// <summary>Vibration channel.</summary>
        V0 = 6,
        /// <summary>Second vibration channel.</summary>
        V1 = 7,
        /// <summary>Air / suction channel.</summary>
        A0 = 8,
        /// <summary>Second air / suction channel.</summary>
        A1 = 9,
        /// <summary>Scratch axis 0 — a user-created general-purpose axis.</summary>
        S0 = 10,
        /// <summary>Scratch axis 1.</summary>
        S1 = 11,
        /// <summary>Scratch axis 2.</summary>
        S2 = 12,
        /// <summary>Scratch axis 3.</summary>
        S3 = 13,
        /// <summary>Scratch axis 4.</summary>
        S4 = 14,
        /// <summary>Scratch axis 5.</summary>
        S5 = 15,
        /// <summary>Scratch axis 6.</summary>
        S6 = 16,
        /// <summary>Scratch axis 7.</summary>
        S7 = 17,
        /// <summary>Scratch axis 8.</summary>
        S8 = 18,
        /// <summary>Scratch axis 9.</summary>
        S9 = 19,

        /// <summary>Number of axis roles (<c>kStandardAxisCount</c> in the core) — a sentinel one past the
        /// last real axis (S9), not itself an axis. Cast to <c>int</c> for the count.</summary>
        Count = 20,
    }

    /// <summary>Helpers for <see cref="StandardAxis"/>.</summary>
    public static class StandardAxisExtensions
    {
        /// <summary>True for the user-created scratch axes S0–S9.</summary>
        public static bool IsScratch(this StandardAxis a) => a >= StandardAxis.S0 && a <= StandardAxis.S9;

        /// <summary>A human-readable label for the axis.</summary>
        public static string DisplayName(this StandardAxis a) => a.ToString();
    }
}
