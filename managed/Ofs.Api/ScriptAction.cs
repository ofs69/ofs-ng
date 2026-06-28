using System;
using System.Runtime.InteropServices;

namespace Ofs
{
    /// <summary>
    /// A point in a funscript: a timestamp (<see cref="At"/>, seconds) and a position
    /// (<see cref="Pos"/>, 0–100). Value type with value equality, deconstruction and
    /// <c>with</c>-expressions. The sequential layout keeps it blittable so it can cross
    /// the native ABI without marshaling.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public readonly record struct ScriptAction(double At, int Pos) : IComparable<ScriptAction>
    {
        /// <summary>Orders actions by <see cref="At"/> (time) only, so a list stays sorted chronologically.</summary>
        public int CompareTo(ScriptAction other) => At.CompareTo(other.At);

        public static bool operator <(ScriptAction left, ScriptAction right) => left.CompareTo(right) < 0;
        public static bool operator <=(ScriptAction left, ScriptAction right) => left.CompareTo(right) <= 0;
        public static bool operator >(ScriptAction left, ScriptAction right) => left.CompareTo(right) > 0;
        public static bool operator >=(ScriptAction left, ScriptAction right) => left.CompareTo(right) >= 0;
    }
}
