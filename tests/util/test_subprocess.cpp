#include "Util/PathUtil.h"
#include "Util/Subprocess.h"
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using namespace ofs::util;

TEST_CASE("toArgv yields a NULL-terminated argv borrowing the source strings") {
    const std::vector<std::string> args = {"ffmpeg", "-i", "in.mp4"};
    const auto argv = toArgv(args);
    REQUIRE(argv.size() == args.size() + 1);
    CHECK(std::string(argv[0]) == "ffmpeg");
    CHECK(std::string(argv[1]) == "-i");
    CHECK(std::string(argv[2]) == "in.mp4");
    CHECK(argv.back() == nullptr);
    CHECK(argv[0] == args[0].c_str()); // borrows, does not copy
}

TEST_CASE("toArgv of an empty list is just the NULL terminator") {
    const auto argv = toArgv({});
    REQUIRE(argv.size() == 1);
    CHECK(argv[0] == nullptr);
}

TEST_CASE("resolveTool maps a tool name to its launch command") {
#ifdef _WIN32
    // Windows: bundled next to the exe as an absolute "<base>/<tool>.exe".
    const std::string r = resolveTool("ffmpeg");
    const std::string suffix = "ffmpeg.exe";
    REQUIRE(r.size() >= suffix.size());
    CHECK(r.compare(r.size() - suffix.size(), suffix.size(), suffix) == 0);
    CHECK(std::filesystem::path(ofs::util::fromUtf8(r)).is_absolute());
#else
    // POSIX: the bare name, resolved from PATH at spawn time.
    CHECK(resolveTool("ffmpeg") == "ffmpeg");
#endif
}

#ifdef _WIN32
// The Windows availability probe is a pure filesystem::exists() check against the exe dir — no process
// is spawned (POSIX would spawn "<tool> -version", which the unit suite deliberately avoids). Driving a
// fake exe in and out of getBasePath() exercises both probe outcomes plus the session cache.
TEST_CASE("toolAvailable probes the bundled exe and caches the result") {
    const std::string tool = "ofs_unit_probe_tool"; // unique: never a real staged tool
    const std::filesystem::path exe = ofs::util::getBasePath() / ofs::util::fromUtf8(tool + ".exe");
    std::error_code ec;
    std::filesystem::remove(exe, ec);

    // Absent → false (and cached as false).
    CHECK(toolAvailable(tool, /*forceRefresh=*/true) == false);

    // Create it, but a cached lookup must still report the stale "false" without re-probing.
    {
        std::ofstream(exe).put('x');
    }
    REQUIRE(std::filesystem::exists(exe));
    CHECK(toolAvailable(tool) == false); // cache hit, not re-probed

    // forceRefresh re-probes and now finds it.
    CHECK(toolAvailable(tool, /*forceRefresh=*/true) == true);

    // Delete it; the cached "true" survives until the next forced refresh.
    std::filesystem::remove(exe, ec);
    CHECK(toolAvailable(tool) == true);                         // cache hit
    CHECK(toolAvailable(tool, /*forceRefresh=*/true) == false); // re-probed, gone
}
#endif
