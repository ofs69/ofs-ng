namespace Ofs
{
    /// <summary>Severity of a host log message written via <see cref="IOfsHost.Log(LogLevel, string)"/>.</summary>
    // Values are the ABI contract: they match the native hostLog level (0=trace 1=debug 2=info 3=warning
    // 4=error). Explicit so a reorder can't silently remap severities once third-party plugins compile.
    public enum LogLevel
    {
        /// <summary>Finest-grained diagnostic detail.</summary>
        Trace = 0,
        /// <summary>Developer-facing diagnostic information.</summary>
        Debug = 1,
        /// <summary>Normal informational message.</summary>
        Info = 2,
        /// <summary>A recoverable problem worth attention.</summary>
        Warning = 3,
        /// <summary>A failure.</summary>
        Error = 4,
    }

    /// <summary>Severity of a user-facing toast raised via <see cref="IOfsHost.Notify"/>.</summary>
    // Values are the ABI contract: they match the native ofs::NotifyLevel enum and the hostNotify
    // contract. Explicit so a reorder can't silently remap severities once third-party plugins compile.
    public enum NotifyLevel
    {
        /// <summary>Neutral information.</summary>
        Info = 0,
        /// <summary>An action completed successfully.</summary>
        Success = 1,
        /// <summary>A warning the user should notice.</summary>
        Warning = 2,
        /// <summary>An error the user should notice.</summary>
        Error = 3,
    }
}
