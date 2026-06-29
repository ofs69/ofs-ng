#include "Util/FrameAllocator.h"
#include "Util/TimeUtil.h"
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <doctest/doctest.h>
#include <string>

TEST_CASE("formatTime with ms produces HH:MM:SS.mmm") {
    std::string s = ofs::TimeUtil::formatTime(3661.5, true); // 1h 01m 01.500s
    CHECK(s == "01:01:01.500");
}

TEST_CASE("formatTime without ms omits the fractional part") {
    std::string s = ofs::TimeUtil::formatTime(65.0, false);
    CHECK(s == "00:01:05");
}

TEST_CASE("formatTime guards NaN/inf to zero") {
    CHECK(std::string(ofs::TimeUtil::formatTime(std::nan(""), false)) == "00:00:00");
    CHECK(std::string(ofs::TimeUtil::formatTime(INFINITY, false)) == "00:00:00");
}

TEST_CASE("formatTimeShort drops whole leading zero groups") {
    // sub-minute: no minute/hour groups, the first shown group has no leading zero
    CHECK(std::string(ofs::TimeUtil::formatTimeShort(5.5)) == "5.500");
    // sub-hour: minutes lead (unpadded), seconds padded
    CHECK(std::string(ofs::TimeUtil::formatTimeShort(65.5)) == "1:05.500");
    CHECK(std::string(ofs::TimeUtil::formatTimeShort(605.5)) == "10:05.500");
    // hours present: hours lead (unpadded), minutes+seconds padded
    CHECK(std::string(ofs::TimeUtil::formatTimeShort(3661.5)) == "1:01:01.500");
}

TEST_CASE("formatTimeShort guards NaN/inf to zero") {
    CHECK(std::string(ofs::TimeUtil::formatTimeShort(std::nan(""))) == "0.000");
    CHECK(std::string(ofs::TimeUtil::formatTimeShort(INFINITY)) == "0.000");
}

TEST_CASE("formatDate renders local wall-clock as YYYY-MM-DD HH:MM") {
    // Pin the zone to UTC so the local-time conversion is deterministic regardless of
    // the test machine's timezone. No other unit test reads local time, so mutating the
    // process-global zone here is safe.
#ifdef _WIN32
    _putenv_s("TZ", "UTC0");
    _tzset();
#else
    setenv("TZ", "UTC0", 1);
    tzset();
#endif
    CHECK(std::string(ofs::TimeUtil::formatDate(0)) == "1970-01-01 00:00");
    CHECK(std::string(ofs::TimeUtil::formatDate(1609459200)) == "2021-01-01 00:00"); // 2021-01-01T00:00:00Z
    CHECK(std::string(ofs::TimeUtil::formatDate(1609462920)) == "2021-01-01 01:02"); // +1h02m, exercises H:M
    CHECK(std::string(ofs::TimeUtil::formatDate(1609545600)) == "2021-01-02 00:00"); // +1 day rollover
}
