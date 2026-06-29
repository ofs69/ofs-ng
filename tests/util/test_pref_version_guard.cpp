#include "Util/PathUtil.h"
#include "Util/PrefVersionGuard.h"
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>

namespace {
std::filesystem::path markerPath() {
    return ofs::util::getPrefPath() / "pref-version.json";
}

void writeMarker(int schema, const std::string &writtenBy) {
    std::filesystem::create_directories(ofs::util::getPrefPath());
    nlohmann::json j = {{"schema", schema}, {"writtenBy", writtenBy}};
    std::ofstream f(markerPath(), std::ios::trunc);
    f << j.dump();
}

std::optional<nlohmann::json> readMarker() {
    std::ifstream f(markerPath());
    if (!f)
        return std::nullopt;
    try {
        nlohmann::json j;
        f >> j;
        return j;
    } catch (...) {
        return std::nullopt;
    }
}

void removeMarker() {
    std::error_code ec;
    std::filesystem::remove(markerPath(), ec);
}
} // namespace

TEST_CASE("checkPrefDirVersion stamps a fresh pref dir and allows launch") {
    removeMarker();

    const ofs::PrefVersionStatus st = ofs::checkPrefDirVersion("v9.9.9-test");
    CHECK(st.ok);

    auto j = readMarker();
    REQUIRE(j.has_value());
    CHECK((*j)["schema"].get<int>() == ofs::kPrefSchemaVersion);
    CHECK((*j)["writtenBy"].get<std::string>() == "v9.9.9-test");

    removeMarker();
}

TEST_CASE("checkPrefDirVersion accepts a marker at the current schema") {
    writeMarker(ofs::kPrefSchemaVersion, "v1.0.0");

    const ofs::PrefVersionStatus st = ofs::checkPrefDirVersion("v9.9.9-test");
    CHECK(st.ok);
    CHECK(st.markerSchema == ofs::kPrefSchemaVersion);

    removeMarker();
}

TEST_CASE("checkPrefDirVersion raises an older stamp and keeps launching") {
    // A pref dir stamped below the current build is upgraded in place to the current schema.
    writeMarker(ofs::kPrefSchemaVersion - 1, "v0.0.1");

    const ofs::PrefVersionStatus st = ofs::checkPrefDirVersion("v9.9.9-test");
    CHECK(st.ok);

    auto j = readMarker();
    REQUIRE(j.has_value());
    CHECK((*j)["schema"].get<int>() == ofs::kPrefSchemaVersion);
    CHECK((*j)["writtenBy"].get<std::string>() == "v9.9.9-test");

    removeMarker();
}

TEST_CASE("checkPrefDirVersion refuses a pref dir stamped by a newer build and leaves it intact") {
    const int newerSchema = ofs::kPrefSchemaVersion + 1;
    writeMarker(newerSchema, "v2.0.0-future");

    const ofs::PrefVersionStatus st = ofs::checkPrefDirVersion("v9.9.9-test");
    CHECK_FALSE(st.ok);
    CHECK(st.markerSchema == newerSchema);
    CHECK(st.writtenBy == "v2.0.0-future");

    // The newer marker must be untouched — the older build must not overwrite it.
    auto j = readMarker();
    REQUIRE(j.has_value());
    CHECK((*j)["schema"].get<int>() == newerSchema);
    CHECK((*j)["writtenBy"].get<std::string>() == "v2.0.0-future");

    removeMarker();
}

TEST_CASE("checkPrefDirVersion treats a corrupt marker as writable and re-stamps it") {
    std::filesystem::create_directories(ofs::util::getPrefPath());
    {
        std::ofstream f(markerPath(), std::ios::trunc);
        f << "{ this is not valid json";
    }

    const ofs::PrefVersionStatus st = ofs::checkPrefDirVersion("v9.9.9-test");
    CHECK(st.ok);
    CHECK(st.markerSchema == 0); // unreadable => treated as no stamp

    auto j = readMarker();
    REQUIRE(j.has_value());
    CHECK((*j)["schema"].get<int>() == ofs::kPrefSchemaVersion);

    removeMarker();
}
