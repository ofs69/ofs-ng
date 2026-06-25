#include "Format/LayoutStore.h"
#include "Util/PathUtil.h"
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using ofs::DockLayoutPreset;
using ofs::LayoutStore;

namespace {
void removeStoreFile() {
    std::error_code ec;
    std::filesystem::remove(ofs::util::getPrefPath() / "layouts.json", ec);
}
} // namespace

TEST_CASE("LayoutStore round-trips presets, active name and lock through JSON") {
    LayoutStore in;
    in.layouts = {{.name = "Editing", .ini = "[ini-blob-A]"}, {.name = "Review", .ini = "[ini-blob-B]"}};
    in.activeLayoutName = "Review";
    in.locked = true;

    nlohmann::json j;
    to_json(j, in);
    LayoutStore out;
    from_json(j, out);

    REQUIRE(out.layouts.size() == 2);
    CHECK(out.layouts[0].name == "Editing");
    CHECK(out.layouts[1].ini == "[ini-blob-B]");
    CHECK(out.activeLayoutName == "Review");
    CHECK(out.locked == true);
}

TEST_CASE("LayoutStore::to_json stamps the schema version") {
    LayoutStore in;
    nlohmann::json j;
    to_json(j, in);
    REQUIRE(j.contains("version"));
    CHECK(j["version"].get<int>() == ofs::kLayoutStoreVersion);
}

TEST_CASE("LayoutStore from_json on an empty object yields documented defaults") {
    LayoutStore out;
    from_json(nlohmann::json::object(), out);
    CHECK(out.layouts.empty());
    CHECK(out.activeLayoutName == "Default");
    CHECK(out.locked == false);
}

TEST_CASE("LayoutStore::load refuses a file newer than kLayoutStoreVersion") {
    const auto dir = ofs::util::getPrefPath();
    std::filesystem::create_directories(dir);

    LayoutStore in;
    in.activeLayoutName = "FromTheFuture"; // a non-default value the guard must NOT surface
    nlohmann::json j;
    to_json(j, in);
    j["version"] = ofs::kLayoutStoreVersion + 1;
    {
        std::ofstream f(dir / "layouts.json", std::ios::trunc);
        f << j.dump();
    }

    LayoutStore loaded = LayoutStore::load();
    CHECK(loaded.activeLayoutName == "Default"); // defaults, not the newer file's value

    removeStoreFile();
}

TEST_CASE("LayoutStore::load swallows a malformed layouts file and returns defaults") {
    const auto dir = ofs::util::getPrefPath();
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "layouts.json", std::ios::trunc);
        f << R"({"layouts": "not an array"})";
    }

    LayoutStore loaded = LayoutStore::load();
    CHECK(loaded.activeLayoutName == "Default");
    CHECK(loaded.layouts.empty());

    removeStoreFile();
}
