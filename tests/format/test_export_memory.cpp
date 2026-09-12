#include "Format/ExportMemory.h"
#include "Util/FileUtil.h"
#include "Util/PathUtil.h"
#include <doctest/doctest.h>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using ofs::ExportMemory;

// Quick Export's config lives app-side rather than in the .ofp so an export never marks the project
// dirty. That makes this store responsible for the bookkeeping the .ofp used to give for free: one
// entry per project, most-recently-exported first, and a bound so the file can't grow forever.
TEST_CASE("ExportMemory keys one config per project, most recent first") {
    ExportMemory m;
    m.remember("a.ofp", ofs::ExportConfig{.format = 0, .axes = {}, .outputPath = "out/a"});
    m.remember("b.ofp", ofs::ExportConfig{.format = 1, .axes = {}, .outputPath = "out/b"});

    REQUIRE(m.entries.size() == 2);
    CHECK(m.entries.front().projectPath == "b.ofp"); // newest first

    REQUIRE(m.find("a.ofp") != nullptr);
    CHECK(m.find("a.ofp")->outputPath == "out/a");
    CHECK(m.find("never-exported.ofp") == nullptr);

    // Re-exporting a project replaces its entry rather than appending a second one, and promotes it.
    m.remember("a.ofp", ofs::ExportConfig{.format = 2, .axes = {}, .outputPath = "out/a2"});
    REQUIRE(m.entries.size() == 2);
    CHECK(m.entries.front().projectPath == "a.ofp");
    REQUIRE(m.find("a.ofp") != nullptr);
    CHECK(m.find("a.ofp")->format == 2);
    CHECK(m.find("a.ofp")->outputPath == "out/a2");
}

TEST_CASE("ExportMemory ignores an untitled project and caps the list") {
    ExportMemory m;
    // An unsaved project has no path to key on; its config lives only in the session.
    m.remember("", ofs::ExportConfig{});
    CHECK(m.entries.empty());
    CHECK(m.find("") == nullptr);

    for (size_t i = 0; i < ofs::kMaxExportMemories + 5; ++i)
        m.remember(std::to_string(i) + ".ofp", ofs::ExportConfig{});

    CHECK(m.entries.size() == ofs::kMaxExportMemories);
    // The five least recently exported projects were evicted, the newest is kept.
    CHECK(m.find("0.ofp") == nullptr);
    CHECK(m.find(std::to_string(ofs::kMaxExportMemories + 4) + ".ofp") != nullptr);
}

TEST_CASE("ExportMemory round-trips its entries through JSON") {
    ExportMemory in;
    in.remember("C:/proj/clip.ofp", ofs::ExportConfig{.format = 2,
                                                      .axes = {ofs::StandardAxis::L0, ofs::StandardAxis::R0},
                                                      .outputPath = "C:/out/clip.funscript"});

    const nlohmann::json j = nlohmann::json::object({{"exports", in.entries}});
    ExportMemory out;
    out.entries = j.at("exports").get<std::vector<ofs::ProjectExportMemory>>();

    const ofs::ExportConfig *cfg = out.find("C:/proj/clip.ofp");
    REQUIRE(cfg != nullptr);
    CHECK(cfg->format == 2);
    // Axes persist as TCode tags, so the enum's numeric order is not part of the on-disk contract.
    CHECK(cfg->axes == std::vector<ofs::StandardAxis>{ofs::StandardAxis::L0, ofs::StandardAxis::R0});
    CHECK(cfg->outputPath == "C:/out/clip.funscript");
}

// The store owns its own file rather than a corner of settings.json, so load/save must round-trip
// through it — and must hold the cap even for a file that was hand-edited past it.
TEST_CASE("ExportMemory round-trips through its own file and trims an over-long one on read") {
    // The unit-test harness redirects the pref path to a temp dir, so this is safe.
    const auto dir = ofs::util::getPrefPath();
    std::filesystem::create_directories(dir);
    const auto path = dir / "export_configs.json";
    const auto settingsPath = dir / "settings.json";
    std::filesystem::remove(path);
    std::filesystem::remove(settingsPath);

    ExportMemory in;
    in.remember("C:/proj/clip.ofp",
                ofs::ExportConfig{.format = 1, .axes = {ofs::StandardAxis::R0}, .outputPath = "C:/out/clip.funscript"});
    in.save();

    const ExportMemory reloaded = ExportMemory::load();
    REQUIRE(reloaded.find("C:/proj/clip.ofp") != nullptr);
    CHECK(reloaded.find("C:/proj/clip.ofp")->outputPath == "C:/out/clip.funscript");

    nlohmann::json entries = nlohmann::json::array();
    for (size_t i = 0; i < ofs::kMaxExportMemories + 10; ++i)
        entries.push_back({{"projectPath", std::to_string(i) + ".ofp"}, {"config", ofs::ExportConfig{}}});
    REQUIRE(ofs::util::writeFileAtomic(path, nlohmann::json::object({{"exports", entries}}).dump()));
    CHECK(ExportMemory::load().entries.size() == ofs::kMaxExportMemories);

    std::filesystem::remove(path);
}

// COMPAT(2026-09-12): v0.2.9 kept these entries in settings.json. The first launch after the upgrade
// finds no export_configs.json and must adopt them — and write them out straight away, because the next
// settings save drops the legacy key and would otherwise take the only copy with it.
TEST_CASE("ExportMemory adopts v0.2.9's entries from settings.json and rewrites them into its own file") {
    const auto dir = ofs::util::getPrefPath();
    std::filesystem::create_directories(dir);
    const auto path = dir / "export_configs.json";
    const auto settingsPath = dir / "settings.json";
    std::filesystem::remove(path);

    nlohmann::json legacy = nlohmann::json::object();
    legacy["lastExports"] = nlohmann::json::array(
        {{{"projectPath", "C:/proj/old.ofp"},
          {"config", ofs::ExportConfig{.format = 2, .axes = {ofs::StandardAxis::L0}, .outputPath = "C:/out/old.fs"}}}});
    REQUIRE(ofs::util::writeFileAtomic(settingsPath, legacy.dump()));

    const ExportMemory migrated = ExportMemory::load();
    REQUIRE(migrated.find("C:/proj/old.ofp") != nullptr);
    CHECK(migrated.find("C:/proj/old.ofp")->outputPath == "C:/out/old.fs");
    REQUIRE(std::filesystem::exists(path)); // rewritten, so losing the legacy key costs nothing

    // Its own file now wins: a stale legacy entry is no longer consulted.
    std::filesystem::remove(settingsPath);
    CHECK(ExportMemory::load().find("C:/proj/old.ofp") != nullptr);

    std::filesystem::remove(path);
}
