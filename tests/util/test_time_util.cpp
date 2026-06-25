#include "Util/FrameAllocator.h"
#include "Util/TimeUtil.h"
#include <cmath>
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
