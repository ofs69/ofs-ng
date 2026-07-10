#include "Core/ScriptAxisAction.h"
#include "Core/VectorSet.h"
#include "Format/Funscript.h"
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <sstream>

TEST_CASE("toActions converts milliseconds to seconds") {
    ofs::Funscript fs;
    fs.actions = {{.at = 1500, .pos = 40}, {.at = 3000, .pos = 80}};
    auto acts = fs.toActions();
    REQUIRE(acts.size() == 2);
    CHECK(acts[0].at == doctest::Approx(1.5));
    CHECK(acts[0].pos == 40);
}

TEST_CASE("fromActions round-trips seconds to ms and back") {
    ofs::VectorSet<ofs::ScriptAxisAction> a;
    a.insert({1.5, 40});
    a.insert({3.0, 80});
    auto fs = ofs::Funscript::fromActions(a);
    REQUIRE(fs.actions.size() == 2);
    CHECK(fs.actions[0].at == 1500);
    CHECK(fs.toActions()[0].at == doctest::Approx(1.5));
}

TEST_CASE("isMultiAxis reflects axes/channels presence") {
    ofs::Funscript fs;
    CHECK_FALSE(fs.isMultiAxis());
    fs.axes.push_back({.id = "L0", .actions = {{.at = 0, .pos = 0}}});
    CHECK(fs.isMultiAxis());
}

TEST_CASE("toAllAxes maps root actions under L0") {
    ofs::Funscript fs;
    fs.actions = {{.at = 1000, .pos = 50}};
    auto all = fs.toAllAxes();
    REQUIRE(all.count("L0") == 1);
    CHECK(all["L0"][0].at == doctest::Approx(1.0));
}

TEST_CASE("fromAxes11 puts the L0 entry into root actions and keeps others multi-axis") {
    ofs::VectorSet<ofs::ScriptAxisAction> l0;
    l0.insert({1.0, 50});
    ofs::VectorSet<ofs::ScriptAxisAction> r0;
    r0.insert({2.0, 60});
    auto fs = ofs::Funscript::fromAxes11({{"L0", l0}, {"R0", r0}});
    CHECK_FALSE(fs.actions.empty()); // L0 -> root
    CHECK(fs.isMultiAxis());         // R0 -> axes[]
}

// Seconds→ms export rounds to the nearest millisecond (std::llround). 3.4567 s → 3456.7 ms must
// round up to 3457, not truncate to 3456 — the rounding behavior the roundtrip.ofp fixture pins
// through the full project→export path (tests/services/test_project_manager.cpp).
TEST_CASE("fromActions rounds sub-millisecond timestamps to the nearest ms") {
    ofs::VectorSet<ofs::ScriptAxisAction> a;
    a.insert({3.4567, 30});
    a.insert({2.75, 50});
    auto fs = ofs::Funscript::fromActions(a);
    REQUIRE(fs.actions.size() == 2);
    CHECK(fs.actions[0].at == 2750); // 2.75 s is exact at ms
    CHECK(fs.actions[1].at == 3457); // 3.4567 s rounds up, not down to 3456
}

TEST_CASE("Funscript metadata round-trips through save and load") {
    ofs::Funscript fs;
    fs.actions = {{.at = 1000, .pos = 50}};
    fs.metadata.title = "Round Trip";
    fs.metadata.creator = "tester";
    fs.metadata.videoUrl = "vid"; // shared document model uses camelCase; on disk it's snake_case
    fs.metadata.scriptUrl = "scr";
    fs.metadata.tags = {"a", "b"};
    fs.metadata.performers = {"p"};
    fs.metadata.license = "Free";
    fs.metadata.customFields = {{.key = "device", .value = "WeVibe"}};

    const auto path = std::filesystem::temp_directory_path() / "ofs_test_meta_roundtrip.funscript";
    REQUIRE(fs.save(path));
    auto loaded = ofs::Funscript::load(path);
    REQUIRE(loaded.has_value());

    CHECK(loaded->metadata.title == "Round Trip");
    CHECK(loaded->metadata.creator == "tester");
    CHECK(loaded->metadata.videoUrl == "vid");
    CHECK(loaded->metadata.scriptUrl == "scr");
    REQUIRE(loaded->metadata.tags.size() == 2);
    CHECK(loaded->metadata.tags[1] == "b");
    REQUIRE(loaded->metadata.performers.size() == 1);
    CHECK(loaded->metadata.license == "Free");
    REQUIRE(loaded->metadata.customFields.size() == 1);
    CHECK(loaded->metadata.customFields[0].key == "device");

    std::filesystem::remove(path);
}

// The saved file must emit "metadata" ahead of "actions" so the small header block is readable at the top
// of the file in a text editor rather than below the whole (potentially huge) actions array.
TEST_CASE("save emits metadata before actions on disk") {
    ofs::Funscript fs;
    fs.actions = {{.at = 1000, .pos = 50}, {.at = 2000, .pos = 10}};
    fs.metadata.title = "Top";

    const auto path = std::filesystem::temp_directory_path() / "ofs_test_order.funscript";
    REQUIRE(fs.save(path));

    std::string text;
    {
        std::ifstream in(path);
        std::stringstream buf;
        buf << in.rdbuf();
        text = buf.str();
    }

    const auto metaPos = text.find("\"metadata\"");
    const auto actionsPos = text.find("\"actions\"");
    REQUIRE(metaPos != std::string::npos);
    REQUIRE(actionsPos != std::string::npos);
    CHECK(metaPos < actionsPos);

    std::filesystem::remove(path);
}

// The document model spells URLs camelCase; the funscript file spells them snake_case. The mapping now
// lives in the funscript (de)serializer — not a separate struct — so verify both directions on the wire.
TEST_CASE("Funscript serializes URLs snake_case and reads them back into the document model") {
    ofs::Funscript fs;
    fs.metadata.scriptUrl = "scr";
    fs.metadata.videoUrl = "vid";

    nlohmann::json j = fs;
    CHECK(j["metadata"]["script_url"] == "scr");
    CHECK(j["metadata"]["video_url"] == "vid");
    CHECK_FALSE(j["metadata"].contains("scriptUrl")); // camelCase must not leak onto disk

    auto back = j.get<ofs::Funscript>();
    CHECK(back.metadata.scriptUrl == "scr");
    CHECK(back.metadata.videoUrl == "vid");
}

TEST_CASE("Funscript preserves non-standard custom fields verbatim through JSON") {
    nlohmann::json j = {{"metadata",
                         {{"title", "t"},
                          {"video_url", "v"},   // standard snake_case key — must NOT become custom
                          {"device", "WeVibe"}, // custom string
                          {"intensity", 7},     // custom number
                          {"ranges", nlohmann::json{{"min", 0}, {"max", 100}}}}}}; // custom nested object

    auto fs = j.get<ofs::Funscript>();
    CHECK(fs.metadata.videoUrl == "v");
    REQUIRE(fs.metadata.customFields.size() == 3); // device, intensity, ranges — not video_url

    nlohmann::json out = fs;
    CHECK(out["metadata"]["device"] == "WeVibe");
    CHECK(out["metadata"]["ranges"]["max"] == 100);
    CHECK_FALSE(out["metadata"].contains("customFields")); // inline, no wrapper key
}

TEST_CASE("load returns nullopt for a missing file") {
    CHECK_FALSE(ofs::Funscript::load("does_not_exist_zzz.funscript").has_value());
}

// funscript 2.0 "channels" object: parsed by from_json and surfaced under its key by toAllAxes.
TEST_CASE("Funscript parses a 2.0 channels object and maps each channel through toAllAxes") {
    nlohmann::json j = {{"actions", nlohmann::json::array()},
                        {"channels",
                         {{"L0", {{"actions", {{{"at", 1000}, {"pos", 50}}}}}},
                          {"twist", {{"actions", {{{"at", 2000}, {"pos", 30}}}}}},
                          {"ignored", {{"note", "no actions array"}}}}}}; // entries without "actions" are skipped

    auto fs = j.get<ofs::Funscript>();
    REQUIRE(fs.channels.count("L0") == 1);
    REQUIRE(fs.channels.count("twist") == 1);
    CHECK(fs.channels.count("ignored") == 0);

    auto all = fs.toAllAxes();
    REQUIRE(all.count("twist") == 1);
    CHECK(all["twist"][0].at == doctest::Approx(2.0));
}

TEST_CASE("fromAxes20 emits a channels object and finds L0 when it is not the first entry") {
    ofs::VectorSet<ofs::ScriptAxisAction> r0;
    r0.insert({1.0, 20});
    ofs::VectorSet<ofs::ScriptAxisAction> l0;
    l0.insert({2.0, 70});
    // R0 first, L0 second: buildMultiAxis must still pick L0 as the primary (root actions).
    auto fs = ofs::Funscript::fromAxes20({{"R0", r0}, {"L0", l0}});
    REQUIRE(fs.actions.size() == 1);
    CHECK(fs.actions[0].at == 2000); // L0 → root
    REQUIRE(fs.channels.count("R0") == 1);
    CHECK(fs.channels["R0"][0].at == 1000);
}

TEST_CASE("fromAxes11/20 return an empty funscript for an empty axis list") {
    CHECK(ofs::Funscript::fromAxes11({}).actions.empty());
    auto fs = ofs::Funscript::fromAxes20({});
    CHECK(fs.actions.empty());
    CHECK(fs.channels.empty());
}
