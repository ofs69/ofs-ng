#define DOCTEST_CONFIG_IMPLEMENT
#include "Util/PathUtil.h"
#include <doctest/doctest.h>
#include <filesystem>
#include <spdlog/spdlog.h>

int main(int argc, char **argv) {
    // The pref path is redirected to a temp directory at compile time via the
    // OFS_TEST_PREF_SUBDIR define (tests/CMakeLists.txt), so tests never touch the
    // user's real app data and there is no runtime ordering to get wrong.
    //
    // Wipe it up front so each run starts from a clean slate and no persisted state
    // (settings.json, layouts.json, …) leaks between runs. has_filename() guards against
    // ever pointing at a drive/temp root.
    if (const auto prefDir = ofs::util::getPrefPath(); prefDir.has_filename()) {
        std::error_code ec;
        std::filesystem::remove_all(prefDir, ec);
        std::filesystem::create_directories(prefDir, ec);
    }

    spdlog::set_level(spdlog::level::warn);
    doctest::Context ctx(argc, argv);
    return ctx.run();
}
