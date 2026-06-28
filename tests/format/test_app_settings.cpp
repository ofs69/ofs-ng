#include "Format/AppSettings.h"
#include "Util/PathUtil.h"
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using ofs::AppSettings;
using ofs::MetadataPreset;

TEST_CASE("AppSettings round-trips all scalar fields through JSON") {
    AppSettings in;
    in.volume = 0.42f;
    in.fontSizeBase = 24.0f;
    in.showSimulator = false;
    in.showStatistics = false;
    in.activeTheme = "Midnight";
    in.hwdecEnabled = false;
    in.pauseOnSeek = false; // flip from default (true)
    in.autoBackupEnabled = false;
    in.language = "de";
    in.liveReloadTranslations = true;
    in.lastProjectPaths = {"a.ofp", "b.ofp"};

    nlohmann::json j;
    to_json(j, in);
    AppSettings out;
    from_json(j, out);

    CHECK(out.volume == doctest::Approx(0.42f));
    CHECK(out.fontSizeBase == doctest::Approx(24.0f));
    CHECK(out.showSimulator == false);
    CHECK(out.showStatistics == false);
    CHECK(out.activeTheme == "Midnight");
    CHECK(out.hwdecEnabled == false);
    CHECK(out.pauseOnSeek == false);
    CHECK(out.autoBackupEnabled == false);
    CHECK(out.language == "de");
    CHECK(out.liveReloadTranslations == true);
    REQUIRE(out.lastProjectPaths.size() == 2);
    CHECK(out.lastProjectPaths[0] == "a.ofp");
}

TEST_CASE("AppSettings from_json on an empty object yields documented defaults") {
    // Missing keys must fall back, not throw.
    AppSettings out;
    from_json(nlohmann::json::object(), out);

    CHECK(out.volume == doctest::Approx(1.0f));
    CHECK(out.showSimulator == true);
    CHECK(out.activeTheme == "Dark");
    CHECK(out.pauseOnSeek == true);
    CHECK(out.autoBackupEnabled == true);
    CHECK(out.language.empty());
    CHECK(out.lastProjectPaths.empty());
    CHECK(out.metadataPresets.empty());
}

TEST_CASE("AppSettings simulator sub-struct round-trips (key is \"simulatorVisuals\")") {
    AppSettings in;
    in.simulator.extraLinesCount = 5;
    in.simulator.use3dSimulator = true;
    in.simulator.labels3dMask = std::bitset<ofs::SimulatorState::kSim3dDofCount>(0b010101); // mixed on/off bits
    in.simulator.labels3dInDegrees = false;                                                 // flip from default (true)

    nlohmann::json j;
    to_json(j, in);
    REQUIRE(j.contains("simulatorVisuals")); // not "simulator" — guards the rename

    AppSettings out;
    from_json(j, out);
    CHECK(out.simulator.extraLinesCount == 5);
    CHECK(out.simulator.use3dSimulator == true);
    CHECK(out.simulator.labels3dMask.to_ulong() == 0b010101UL);
    CHECK(out.simulator.labels3dInDegrees == false);
}

TEST_CASE("AppSettings metadata presets round-trip name and metadata") {
    AppSettings in;
    MetadataPreset p;
    p.name = "Studio Default";
    p.metadata.title = "Scene 1";
    p.metadata.creator = "ofs";
    in.metadataPresets.push_back(p);

    nlohmann::json j;
    to_json(j, in);
    AppSettings out;
    from_json(j, out);

    REQUIRE(out.metadataPresets.size() == 1);
    CHECK(out.metadataPresets[0].name == "Studio Default");
    CHECK(out.metadataPresets[0].metadata.title == "Scene 1");
    CHECK(out.metadataPresets[0].metadata.creator == "ofs");
}

TEST_CASE("AppSettings from_json throws on a wrong-typed key (resilience lives in load)") {
    // Contrary to a common assumption, nlohmann's value<T>() does NOT fall back to
    // the default when the key exists with the wrong type — it throws type_error.302.
    // So from_json is not tolerant on its own; callers must guard.
    nlohmann::json j = {{"volume", "not a number"}};
    AppSettings out;
    CHECK_THROWS_AS(from_json(j, out), nlohmann::json::exception);
}

TEST_CASE("AppSettings::to_json stamps the schema version") {
    AppSettings in;
    nlohmann::json j;
    to_json(j, in);
    REQUIRE(j.contains("version"));
    CHECK(j["version"].get<int>() == ofs::kAppSettingsVersion);
}

TEST_CASE("AppSettings::load refuses a file newer than kAppSettingsVersion") {
    const auto dir = ofs::util::getPrefPath();
    std::filesystem::create_directories(dir);
    const auto settingsPath = dir / "settings.json";

    AppSettings in;
    in.volume = 0.25f; // a non-default value the guard must NOT surface
    nlohmann::json j;
    to_json(j, in);
    j["version"] = ofs::kAppSettingsVersion + 1;
    {
        std::ofstream f(settingsPath, std::ios::trunc);
        f << j.dump();
    }

    AppSettings loaded = AppSettings::load();
    CHECK(loaded.volume == doctest::Approx(1.0f)); // defaults, not the newer file's 0.25

    std::filesystem::remove(settingsPath);
}

TEST_CASE("AppSettings::load swallows a malformed settings file and returns defaults") {
    // load() is the real resilience boundary: it wraps from_json in try/catch, so a
    // corrupt settings.json on disk yields documented defaults instead of crashing.
    // The unit-test harness redirects the pref path to a temp dir, so this is safe.
    const auto dir = ofs::util::getPrefPath();
    std::filesystem::create_directories(dir);
    const auto settingsPath = dir / "settings.json";

    {
        std::ofstream f(settingsPath, std::ios::trunc);
        f << R"({"volume": "not a number", "activeTheme": 12345})";
    }

    AppSettings loaded = AppSettings::load();
    CHECK(loaded.volume == doctest::Approx(1.0f));
    CHECK(loaded.activeTheme == "Dark");

    std::filesystem::remove(settingsPath);
}
