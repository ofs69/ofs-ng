#include "Core/ScriptAxisAction.h"
#include "Core/StandardAxis.h"
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

    const auto versionPos = text.find("\"version\"");
    const auto metaPos = text.find("\"metadata\"");
    const auto actionsPos = text.find("\"actions\"");
    REQUIRE(versionPos != std::string::npos);
    REQUIRE(metaPos != std::string::npos);
    REQUIRE(actionsPos != std::string::npos);
    CHECK(versionPos < metaPos); // spec header fields lead, then metadata, then the bulk actions array
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

TEST_CASE("Funscript writes bookmarks/chapters as OFS timecodes and round-trips them to seconds") {
    ofs::Funscript fs;
    fs.actions = {{.at = 1000, .pos = 50}};
    fs.bookmarks.push_back({.time = 83.456, .name = "intro"}); // 00:01:23.456
    ofs::Chapter c;
    c.name = "scene1";
    c.startTime = 0.0;
    c.endTime = 90.0; // 00:01:30.000
    fs.chapters.push_back(std::move(c));

    nlohmann::json j = fs;
    CHECK(j["metadata"]["bookmarks"][0]["name"] == "intro");
    CHECK(j["metadata"]["bookmarks"][0]["time"] == "00:01:23.456"); // string timecode, not a number
    CHECK(j["metadata"]["chapters"][0]["startTime"] == "00:00:00.000");
    CHECK(j["metadata"]["chapters"][0]["endTime"] == "00:01:30.000");
    // Standard fields only: ofs-ng's chapter color / scene-view never reach the funscript.
    CHECK_FALSE(j["metadata"]["chapters"][0].contains("color"));
    CHECK_FALSE(j["metadata"]["chapters"][0].contains("sceneView"));

    auto back = j.get<ofs::Funscript>();
    REQUIRE(back.bookmarks.size() == 1);
    CHECK(back.bookmarks[0].name == "intro");
    CHECK(back.bookmarks[0].time == doctest::Approx(83.456));
    REQUIRE(back.chapters.size() == 1);
    CHECK(back.chapters[0].name == "scene1");
    CHECK(back.chapters[0].startTime == doctest::Approx(0.0));
    CHECK(back.chapters[0].endTime == doctest::Approx(90.0));
}

TEST_CASE("Funscript omits empty bookmarks/chapters from the metadata object") {
    ofs::Funscript fs;
    fs.actions = {{.at = 0, .pos = 0}};
    nlohmann::json j = fs;
    CHECK_FALSE(j["metadata"].contains("bookmarks"));
    CHECK_FALSE(j["metadata"].contains("chapters"));
}

// An OFS-authored funscript stores bookmark/chapter times as "HH:MM:SS.mmm" strings; parse them, and
// tolerate malformed entries (missing field, unparseable time, start > end) by skipping just those rows.
TEST_CASE("Funscript parses OFS-style string timecodes and skips malformed bookmark/chapter entries") {
    nlohmann::json j = {
        {"actions", nlohmann::json::array()},
        {"metadata",
         {{"bookmarks", nlohmann::json::array({
                            {{"name", "good"}, {"time", "00:00:05.500"}},
                            {{"name", "no-time"}},                   // missing time — skipped
                            {{"name", "bad-time"}, {"time", "abc"}}, // unparseable — skipped
                            {{"name", "numeric"}, {"time", 5}},      // non-string — skipped
                        })},
          {"chapters",
           nlohmann::json::array({
               {{"name", "ok"}, {"startTime", "00:00:00.000"}, {"endTime", "00:00:10.000"}},
               {{"name", "inverted"}, {"startTime", "00:00:20.000"}, {"endTime", "00:00:10.000"}}, // start>end
               {{"name", "missing-end"}, {"startTime", "00:00:30.000"}},                           // skipped
           })}}}};

    auto fs = j.get<ofs::Funscript>();
    REQUIRE(fs.bookmarks.size() == 1);
    CHECK(fs.bookmarks[0].name == "good");
    CHECK(fs.bookmarks[0].time == doctest::Approx(5.5));
    REQUIRE(fs.chapters.size() == 1);
    CHECK(fs.chapters[0].name == "ok");
    CHECK(fs.chapters[0].endTime == doctest::Approx(10.0));
    // A funscript's chapters/bookmarks are first-class, not swallowed into customFields.
    CHECK(fs.metadata.customFields.empty());
}

TEST_CASE("Funscript accepts shorter MM:SS.mmm timecodes from other tools") {
    nlohmann::json j = {{"actions", nlohmann::json::array()},
                        {"metadata", {{"bookmarks", nlohmann::json::array({{{"name", "m"}, {"time", "02:03.250"}}})}}}};
    auto fs = j.get<ofs::Funscript>();
    REQUIRE(fs.bookmarks.size() == 1);
    CHECK(fs.bookmarks[0].time == doctest::Approx(123.25)); // 2*60 + 3.25
}

// Foreign funscripts carry the header fields where the original spec puts them — "version"/"inverted"/
// "range" at the top level — and often spell metadata.version as a *number*. A single mismatched type
// must degrade to the default, never sink the whole file.
TEST_CASE("Funscript imports a spec-layout file with top-level header fields and a numeric version") {
    nlohmann::json j = {
        {"version", "1.0"},
        {"inverted", true},
        {"range", 90},
        {"actions", nlohmann::json::array({{{"at", 1000}, {"pos", 50}}})},
        {"metadata",
         {{"type", "basic"},
          {"video_url", "v"},
          {"studio", "s"}, // custom
          {"title", "t"},
          {"description", "d"},
          {"duration", 1234.5}, // fractional seconds, not an integer
          {"performers", nlohmann::json::array({"p"})},
          {"tags", nlohmann::json::array({"a"})},
          {"released_on", "2026-01-01"}, // custom
          {"bookmarks", nlohmann::json::array({{{"name", "b"}, {"time", "00:00:05.500"}}})},
          {"chapters",
           nlohmann::json::array({{{"name", "c"}, {"startTime", "00:00:00.000"}, {"endTime", "00:00:10.000"}}})},
          {"version", 1.0},           // number where the spec says string
          {"average_speed", 42.5}}}}; // custom

    auto fs = j.get<ofs::Funscript>();
    REQUIRE(fs.actions.size() == 1);
    CHECK(fs.metadata.title == "t");
    CHECK(fs.metadata.videoUrl == "v");
    CHECK(fs.duration == 1234);
    CHECK(fs.inverted); // read from the top level, where the spec puts it
    CHECK(fs.range == 90);
    CHECK(fs.version == "1.0");
    REQUIRE(fs.bookmarks.size() == 1);
    REQUIRE(fs.chapters.size() == 1);
    CHECK(fs.metadata.customFields.size() == 3); // studio, released_on, average_speed
}

// Every metadata field is read defensively: a wrong type anywhere degrades that one field.
TEST_CASE("Funscript tolerates mistyped metadata fields instead of failing the load") {
    nlohmann::json j = {{"actions", nlohmann::json::array({{{"at", 0}, {"pos", 0}}})},
                        {"metadata",
                         {{"title", 42},                              // number where a string belongs
                          {"tags", "not-an-array"},                   // string where an array belongs
                          {"performers", nlohmann::json::array({1})}, // array of the wrong element type
                          {"duration", "1234"},                       // string where a number belongs
                          {"video_url", "v"}}}};

    auto fs = j.get<ofs::Funscript>();
    CHECK(fs.metadata.title.empty());
    CHECK(fs.metadata.tags.empty());
    CHECK(fs.metadata.performers.empty());
    CHECK(fs.duration == 0);
    CHECK(fs.metadata.videoUrl == "v"); // the well-typed neighbors still load
}

// Other tools write at/pos as plain JSON numbers, sometimes fractional. Round them instead of letting a
// float sink the whole array, and skip a malformed entry rather than the rest of the script with it.
TEST_CASE("Funscript reads fractional action numbers and skips malformed entries") {
    nlohmann::json j = {{"actions", nlohmann::json::array({
                                        {{"at", 1000.6}, {"pos", 49.5}},
                                        {{"at", 2000}, {"pos", 80}},
                                        {{"at", "3000"}, {"pos", 10}}, // string — skipped
                                        {{"at", 4000}},                // no pos — skipped
                                        nlohmann::json::array(),       // not an object — skipped
                                    })}};

    auto fs = j.get<ofs::Funscript>();
    REQUIRE(fs.actions.size() == 2);
    CHECK(fs.actions[0].at == 1001); // rounded, not truncated
    CHECK(fs.actions[0].pos == 50);
    CHECK(fs.actions[1].at == 2000);
}

// version/inverted/range are file-level fields in the funscript spec, so that is where we write them —
// the "metadata" object carries only type/duration and the document metadata.
TEST_CASE("Funscript writes version/inverted/range at the top level, not inside metadata") {
    ofs::Funscript fs;
    fs.actions = {{.at = 0, .pos = 0}};
    fs.version = "1.1";
    fs.inverted = true;
    fs.range = 90;
    fs.duration = 120;

    nlohmann::json j = fs;
    CHECK(j["version"] == "1.1");
    CHECK(j["inverted"] == true);
    CHECK(j["range"] == 90);
    CHECK_FALSE(j["metadata"].contains("version"));
    CHECK_FALSE(j["metadata"].contains("inverted"));
    CHECK_FALSE(j["metadata"].contains("range"));
    CHECK(j["metadata"]["duration"] == 120); // type/duration stay metadata fields

    auto back = j.get<ofs::Funscript>();
    CHECK(back.version == "1.1");
    CHECK(back.inverted);
    CHECK(back.range == 90);
    CHECK(back.metadata.customFields.empty()); // never round-tripped as custom fields
}

// ofs-ng wrote the header fields inside "metadata" up to 0.1.x; those files must still read back the
// same, and re-exporting one must move the fields up rather than duplicate them.
TEST_CASE("Funscript reads header fields left inside metadata by an older build") {
    nlohmann::json j = {{"actions", nlohmann::json::array()},
                        {"metadata", {{"version", "1.1"}, {"inverted", true}, {"range", 80}, {"title", "t"}}}};

    auto fs = j.get<ofs::Funscript>();
    CHECK(fs.version == "1.1");
    CHECK(fs.inverted);
    CHECK(fs.range == 80);
    CHECK(fs.metadata.customFields.empty());

    nlohmann::json out = fs;
    CHECK(out["range"] == 80);
    CHECK_FALSE(out["metadata"].contains("range"));
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
    CHECK(fs.actions[0].at == 2000);          // L0 → root
    REQUIRE(fs.channels.count("twist") == 1); // R0 → its TCode track name
    CHECK(fs.channels["twist"][0].at == 1000);
}

TEST_CASE("fromAxes11/20 return an empty funscript for an empty axis list") {
    CHECK(ofs::Funscript::fromAxes11({}).actions.empty());
    auto fs = ofs::Funscript::fromAxes20({});
    CHECK(fs.actions.empty());
    CHECK(fs.channels.empty());
}

// Root "actions" is L0 by convention, and readers assign it to L0 unconditionally — MultiFunPlayer's
// FunscriptReader does exactly that, without consulting axes[]/channels{}. So an export that does not
// include L0 must leave root empty and give every axis its own id, or the promoted axis plays as stroke.
TEST_CASE("fromAxes11/20 leave root actions empty when no L0 axis is exported") {
    ofs::VectorSet<ofs::ScriptAxisAction> r1;
    r1.insert({1.0, 20});
    ofs::VectorSet<ofs::ScriptAxisAction> r2;
    r2.insert({2.0, 70});

    auto fs11 = ofs::Funscript::fromAxes11({{"R1", r1}, {"R2", r2}});
    CHECK(fs11.actions.empty());
    REQUIRE(fs11.axes.size() == 2);
    CHECK(fs11.axes[0].id == "R1");
    CHECK(fs11.axes[1].id == "R2");

    auto fs20 = ofs::Funscript::fromAxes20({{"R1", r1}, {"R2", r2}});
    CHECK(fs20.actions.empty());
    REQUIRE(fs20.channels.count("roll") == 1);
    REQUIRE(fs20.channels.count("pitch") == 1);
    CHECK(fs20.channels["roll"][0].at == 1000);

    // Our own reader must recover both axes under their real ids, with no phantom L0.
    auto all = fs11.toAllAxes();
    CHECK(all.count("L0") == 0);
    REQUIRE(all.count("R1") == 1);
    CHECK(all["R1"][0].at == doctest::Approx(1.0));
}

// XTPlayer/XTEngine keys its funscript 2.0 "channels" object on the TCode track name and matches it
// against its channel table by exact string, so a short-tag key ("R0") matches nothing and the axis is
// dropped silently. V1/A0 have no TCode track name and keep their tag.
TEST_CASE("fromAxes20 keys channels on the TCode track name") {
    ofs::VectorSet<ofs::ScriptAxisAction> acts;
    acts.insert({1.0, 50});

    auto fs = ofs::Funscript::fromAxes20({{"L0", acts},
                                          {"L1", acts},
                                          {"L2", acts},
                                          {"R0", acts},
                                          {"R1", acts},
                                          {"R2", acts},
                                          {"V0", acts},
                                          {"V1", acts},
                                          {"A0", acts},
                                          {"A1", acts}});

    CHECK(fs.actions.size() == 1); // L0 stays in root actions, where every reader expects stroke
    for (const auto *key : {"surge", "sway", "twist", "roll", "pitch", "vib", "suck"})
        CHECK(fs.channels.count(key) == 1);
    // No track name for these two, so the canonical tag is kept rather than inventing one.
    CHECK(fs.channels.count("V1") == 1);
    CHECK(fs.channels.count("A0") == 1);
    CHECK(fs.channels.count("stroke") == 0);
    CHECK(fs.channels.count("R0") == 0);
}

// The track-name keys must survive a round-trip through our own reader, which resolves them via
// standardAxisFromTag's alias table.
TEST_CASE("fromAxes20 track-name channels re-import under their canonical tag") {
    ofs::VectorSet<ofs::ScriptAxisAction> r0;
    r0.insert({1.0, 20});
    ofs::VectorSet<ofs::ScriptAxisAction> v0;
    v0.insert({2.0, 70});

    auto fs = ofs::Funscript::fromAxes20({{"R0", r0}, {"V0", v0}});
    auto all = fs.toAllAxes();
    REQUIRE(all.count("twist") == 1);
    REQUIRE(all.count("vib") == 1);
    CHECK(ofs::standardAxisFromTag("twist") == ofs::StandardAxis::R0);
    CHECK(ofs::standardAxisFromTag("vib") == ofs::StandardAxis::V0);
    CHECK(all["vib"][0].at == doctest::Approx(2.0));
}

// 1.1 keeps the short tag: MultiFunPlayer resolves axes[] ids through DeviceAxis.TryParse, which only
// knows the L0/R0 names, so a track-name id there would be dropped.
TEST_CASE("fromAxes11 keeps the short tag as the axes[] id") {
    ofs::VectorSet<ofs::ScriptAxisAction> acts;
    acts.insert({1.0, 50});
    auto fs = ofs::Funscript::fromAxes11({{"L0", acts}, {"R0", acts}, {"V0", acts}});
    REQUIRE(fs.axes.size() == 2);
    CHECK(fs.axes[0].id == "R0");
    CHECK(fs.axes[1].id == "V0");
}

TEST_CASE("fromAxes11/20 stamp the funscript version they produce") {
    ofs::VectorSet<ofs::ScriptAxisAction> acts;
    acts.insert({1.0, 50});
    CHECK(ofs::Funscript::fromAxes11({{"L0", acts}}).version == "1.1");
    CHECK(ofs::Funscript::fromAxes20({{"L0", acts}}).version == "2.0");
    CHECK(ofs::Funscript::fromActions(acts).version == "1.0");
}
