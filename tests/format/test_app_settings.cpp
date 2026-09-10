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
    in.webSocketServerEnabled = true;
    in.webSocketPort = 9090;

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
    CHECK(out.webSocketServerEnabled == true);
    CHECK(out.webSocketPort == 9090);
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
    CHECK(out.webSocketServerEnabled == false);
    CHECK(out.webSocketPort == 8080);
}

TEST_CASE("AppSettings clamps the classic OFS WebSocket port") {
    AppSettings out;
    from_json(nlohmann::json{{"webSocketPort", 99999}}, out);
    CHECK(out.webSocketPort == 65535);
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

// The Quick Export config lives here rather than in the .ofp so an export never marks the project
// dirty. That makes AppSettings responsible for the bookkeeping the .ofp used to give for free: one
// entry per project, most-recently-exported first, and a bound so the file can't grow forever.
TEST_CASE("AppSettings::rememberExport keys one config per project, most recent first") {
    AppSettings s;
    s.rememberExport("a.ofp", ofs::ExportConfig{.format = 0, .axes = {}, .outputPath = "out/a"});
    s.rememberExport("b.ofp", ofs::ExportConfig{.format = 1, .axes = {}, .outputPath = "out/b"});

    REQUIRE(s.lastExports.size() == 2);
    CHECK(s.lastExports.front().projectPath == "b.ofp"); // newest first

    REQUIRE(s.findExport("a.ofp") != nullptr);
    CHECK(s.findExport("a.ofp")->outputPath == "out/a");
    CHECK(s.findExport("never-exported.ofp") == nullptr);

    // Re-exporting a project replaces its entry rather than appending a second one, and promotes it.
    s.rememberExport("a.ofp", ofs::ExportConfig{.format = 2, .axes = {}, .outputPath = "out/a2"});
    REQUIRE(s.lastExports.size() == 2);
    CHECK(s.lastExports.front().projectPath == "a.ofp");
    REQUIRE(s.findExport("a.ofp") != nullptr);
    CHECK(s.findExport("a.ofp")->format == 2);
    CHECK(s.findExport("a.ofp")->outputPath == "out/a2");
}

TEST_CASE("AppSettings::rememberExport ignores an untitled project and caps the list") {
    AppSettings s;
    // An unsaved project has no path to key on; its config lives only in the session.
    s.rememberExport("", ofs::ExportConfig{});
    CHECK(s.lastExports.empty());
    CHECK(s.findExport("") == nullptr);

    for (size_t i = 0; i < ofs::kMaxExportMemories + 5; ++i)
        s.rememberExport(std::to_string(i) + ".ofp", ofs::ExportConfig{});

    CHECK(s.lastExports.size() == ofs::kMaxExportMemories);
    // The five least recently exported projects were evicted, the newest is kept.
    CHECK(s.findExport("0.ofp") == nullptr);
    CHECK(s.findExport(std::to_string(ofs::kMaxExportMemories + 4) + ".ofp") != nullptr);
}

TEST_CASE("AppSettings round-trips remembered exports through JSON") {
    AppSettings in;
    in.rememberExport("C:/proj/clip.ofp", ofs::ExportConfig{.format = 2,
                                                            .axes = {ofs::StandardAxis::L0, ofs::StandardAxis::R0},
                                                            .outputPath = "C:/out/clip.funscript"});

    nlohmann::json j;
    to_json(j, in);
    AppSettings out;
    from_json(j, out);

    const ofs::ExportConfig *cfg = out.findExport("C:/proj/clip.ofp");
    REQUIRE(cfg != nullptr);
    CHECK(cfg->format == 2);
    // Axes persist as TCode tags, so the enum's numeric order is not part of the on-disk contract.
    CHECK(cfg->axes == std::vector<ofs::StandardAxis>{ofs::StandardAxis::L0, ofs::StandardAxis::R0});
    CHECK(cfg->outputPath == "C:/out/clip.funscript");
}

TEST_CASE("AppSettings trims an over-long remembered-export list on read") {
    nlohmann::json entries = nlohmann::json::array();
    for (size_t i = 0; i < ofs::kMaxExportMemories + 10; ++i)
        entries.push_back({{"projectPath", std::to_string(i) + ".ofp"}, {"config", ofs::ExportConfig{}}});

    nlohmann::json j;
    to_json(j, AppSettings{});
    j["lastExports"] = entries;

    AppSettings out;
    from_json(j, out);
    CHECK(out.lastExports.size() == ofs::kMaxExportMemories);
}
