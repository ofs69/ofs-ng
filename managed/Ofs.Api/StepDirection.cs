namespace Ofs
{
    /// <summary>A step / nudge direction shared by <see cref="EditIntent.Direction"/> and
    /// <see cref="NavStep.Direction"/>. <see cref="None"/> is an <see cref="EditIntent"/>-only state: a
    /// position nudge rather than a time nudge (<see cref="EditIntent.Pos"/> carries the delta). A
    /// <see cref="NavStep"/> only ever carries <see cref="Backward"/> or <see cref="Forward"/>.</summary>
    // Values are the ABI contract: ±1 is the step sign and 0 is the position-nudge sentinel, matching
    // native StepDirection / the OfsEditIntent.Direction & OfsNavIntent.Direction fields. Explicit so a
    // reorder can't silently remap the sign once third-party plugins compile.
    public enum StepDirection
    {
        /// <summary>Step backward / nudge earlier in time (−1).</summary>
        Backward = -1,
        /// <summary>EditIntent only: a position nudge, not a time nudge (0).</summary>
        None = 0,
        /// <summary>Step forward / nudge later in time (+1).</summary>
        Forward = 1,
    }
}
