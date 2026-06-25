#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace ofs {
class TimeUtil {
  public:
    static const char *formatTime(double timeSeconds, bool withMs = true);

    // Compact form for tight UI (tooltips, labels): omits the hours group unless
    // non-zero, so a sub-hour timestamp reads "M:SS.mmm" instead of "00:MM:SS.mmm".
    static const char *formatTimeShort(double timeSeconds);

    // Local-time wall-clock date for a Unix timestamp (seconds), as "YYYY-MM-DD HH:MM". Main-thread
    // only (returns a frame-arena string, like the other formatters).
    static const char *formatDate(int64_t unixSeconds);
};
} // namespace ofs
