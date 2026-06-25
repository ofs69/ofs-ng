#include "TimeUtil.h"
#include "FrameAllocator.h"

#include <chrono>
#include <cmath>
#include <ctime>

namespace ofs {

const char *TimeUtil::formatTime(double timeSeconds, bool withMs) {
    if (std::isinf(timeSeconds) || std::isnan(timeSeconds))
        timeSeconds = 0.0;

    auto duration = std::chrono::duration<double>(timeSeconds);

    auto hours = std::chrono::duration_cast<std::chrono::hours>(duration);
    duration -= hours;

    auto minutes = std::chrono::duration_cast<std::chrono::minutes>(duration);
    duration -= minutes;

    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    duration -= seconds;

    if (withMs) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
        return fmtScratch("{:02}:{:02}:{:02}.{:03}", hours.count(), minutes.count(), seconds.count(), ms.count());
    }
    return fmtScratch("{:02}:{:02}:{:02}", hours.count(), minutes.count(), seconds.count());
}

const char *TimeUtil::formatTimeShort(double timeSeconds) {
    if (std::isinf(timeSeconds) || std::isnan(timeSeconds))
        timeSeconds = 0.0;

    auto duration = std::chrono::duration<double>(timeSeconds);
    auto hours = std::chrono::duration_cast<std::chrono::hours>(duration);
    duration -= hours;
    auto minutes = std::chrono::duration_cast<std::chrono::minutes>(duration);
    duration -= minutes;
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    duration -= seconds;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration);

    // Drop whole leading groups that are zero; the first shown group has no
    // leading zero (e.g. "5.200", "1:23.456", "1:02:03.456").
    if (hours.count() > 0)
        return fmtScratch("{}:{:02}:{:02}.{:03}", hours.count(), minutes.count(), seconds.count(), ms.count());
    if (minutes.count() > 0)
        return fmtScratch("{}:{:02}.{:03}", minutes.count(), seconds.count(), ms.count());
    return fmtScratch("{}.{:03}", seconds.count(), ms.count());
}

const char *TimeUtil::formatDate(int64_t unixSeconds) {
    const auto t = static_cast<std::time_t>(unixSeconds);
    std::tm tm{};
    // localtime_s / _r over std::localtime: the latter trips MSVC's C4996 under /WX and isn't thread-safe.
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return fmtScratch("{:04}-{:02}-{:02} {:02}:{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                      tm.tm_min);
}

} // namespace ofs
