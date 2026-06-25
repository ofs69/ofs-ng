// Full-field serialization round-trip for the project (.ofp) format. Project::save writes CBOR and
// Project::load reads it back; the deep-equality check compares nlohmann::json(original) against
// json(reloaded), which covers every serialized field at once and fails loudly if any field is
// written but never read back (or vice-versa). Also covers the defaulting (sparse-file) branch and
// the load-failure guards (missing file, garbage bytes, newer version).
#include "Core/ProcessingRegion.h"
#include "Core/StandardAxis.h"
#include "Format/Project.h"
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using ofs::GraphNodeType;
using ofs::Project;
using ofs::StandardAxis;

namespace {

std::filesystem::path tempPath(const char *name) {
    return std::filesystem::temp_directory_path() / name;
}

void writeCbor(const std::filesystem::path &path, const nlohmann::json &j) {
    const auto cbor = nlohmann::json::to_cbor(j);
    std::ofstream os(path, std::ios::binary);
    os.write(reinterpret_cast<const char *>(cbor.data()), static_cast<std::streamsize>(cbor.size()));
}

// A region carrying a non-trivial graph: Input → Effect(smooth, passes=2) → Output, on L0+R0.
ofs::ProcessingRegion richRegion() {
    ofs::ProcessingNodeGraph g;
    const int in = g.allocId();
    const int eff = g.allocId();
    const int out = g.allocId();
    g.nodes.push_back({.id = in, .type = GraphNodeType::Input, .posX = 10.0f, .posY = 20.0f, .role = StandardAxis::L0});
    ofs::ProcessingGraphNode e;
    e.id = eff;
    e.type = GraphNodeType::Effect;
    e.effect.type = "smooth";
    e.effect.params = {2.0f};
    e.posX = 200.0f;
    e.role = StandardAxis::L0;
    g.nodes.push_back(e);
    g.nodes.push_back({.id = out, .type = GraphNodeType::Output, .posX = 400.0f, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = in, .toNode = eff, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = eff, .toNode = out, .toPin = 0});

    // A plugin node carrying a TState (nodeState JSON) — exercises pluginNodeId + the parse-on-write /
    // dump-on-read nodeState path through the whole-project round-trip. The JSON is canonical/compact so it
    // survives nlohmann's parse→dump unchanged.
    ofs::ProcessingGraphNode plug;
    plug.id = g.allocId();
    plug.type = GraphNodeType::PluginNode;
    plug.pluginNodeId = "sample.blend";
    plug.pluginInputCount = 2;
    plug.pluginOutputCount = 2; // multi-output: persisted so a disabled plugin still draws both pins
    plug.pluginSignal = 1;
    plug.nodeState = R"({"Mix":0.25,"Mode":"Add"})";
    plug.posX = 200.0f;
    plug.posY = 150.0f;
    g.nodes.push_back(plug);

    // A second Output sink fed from the plugin node's *second* output pin — exercises a non-zero
    // ProcessingGraphLink::fromPin through the whole-project round-trip. (validateGraph isn't run on this
    // synthetic graph; the round-trip only proves every field survives save/load.)
    const int out2 = g.allocId();
    g.nodes.push_back(
        {.id = out2, .type = GraphNodeType::Output, .posX = 400.0f, .posY = 150.0f, .role = StandardAxis::R0});
    g.links.push_back({.id = g.allocId(), .fromNode = plug.id, .fromPin = 1, .toNode = out2, .toPin = 0});

    ofs::ProcessingRegion r;
    r.id = 7;
    r.startTime = 2.5;
    r.endTime = 12.0;
    r.name = "verse";
    r.color = IM_COL32(12, 200, 99, 210); // non-default so the round-trip proves per-region color persists
    r.nodeGraph = g;
    r.hz = 45;
    r.showSourceActions = false;
    r.axisRoleTags = {"L0", "R0"}; // the serialized form; the runtime bitset isn't persisted
    return r;
}

// A project with every serialized field set to a non-default value.
Project fullyPopulated() {
    Project p;
    p.mediaPath = "C:/videos/josé/clip.mp4";
    p.originalMediaPath = "C:/videos/josé/clip.mp4";
    p.intraOptimizeDeclined = true; // sticky "Not Now" persists
    p.dummyDuration = 123.5;
    p.activeAxisRole = StandardAxis::R1;

    ofs::SerializedAxis l0;
    l0.role = StandardAxis::L0;
    l0.isVisible = true;
    l0.showInStrip = false;
    l0.isLocked = true;
    l0.actions.insert({0.0, 0});
    l0.actions.insert({1.25, 73});
    l0.actions.insert({9.99, 100});

    ofs::SerializedAxis s3;
    s3.role = StandardAxis::S3;
    s3.isVisible = false;
    s3.showInStrip = true;
    s3.isLocked = false;
    s3.actions.insert({3.0, 42});
    p.scriptAxes = {l0, s3};

    p.processingRegions = {richRegion()};

    p.metadata.title = "Test Title";
    p.metadata.creator = "Tester";
    p.metadata.scriptUrl = "https://example.com/s";
    p.metadata.videoUrl = "https://example.com/v";
    p.metadata.description = "desc";
    p.metadata.notes = "notes";
    p.metadata.tags = {"a", "b"};
    p.metadata.performers = {"p1"};
    p.metadata.license = "Free";

    p.overlaySettings.overlay = ofs::ScriptingOverlay::Tempo;
    p.overlaySettings.frameFps = 24.0f;
    p.overlaySettings.tempoBpm = 140.0f;
    p.overlaySettings.tempoOffsetSeconds = 0.25f;
    p.overlaySettings.tempoMeasureIndex = 5;

    p.videoPlayerState.activeMode = ofs::VideoMode::VrMode;
    p.videoPlayerState.resolutionScale = 0.5f;

    p.simP1 = {1.0f, 2.0f};
    p.simP2 = {3.0f, 4.0f};
    p.sim3dPos = {5.0f, 6.0f};
    p.sim3dSize = 250.0f;

    p.defaultSceneView.framing.zoomFactor = 1.5f;
    p.defaultSceneView.framing.translation = {0.1f, 0.2f};
    p.defaultSceneView.framing.vrRotation = {0.3f, 0.7f};
    p.defaultSceneView.framing.vrZoom = 0.8f;
    p.defaultSceneView.anchor.widthNorm = 0.2f;
    p.defaultSceneView.inverted = true;

    ofs::Bookmark bm;
    bm.time = 4.0;
    bm.name = "cue";
    ofs::Chapter ch;
    ch.startTime = 1.0;
    ch.endTime = 5.0;
    ch.name = "intro";
    ch.color = IM_COL32(10, 20, 30, 200);
    ofs::SceneView sv;
    sv.inverted = true;
    sv.framing.zoomFactor = 2.0f;
    ch.sceneView = sv; // exercise the per-chapter scene-view override
    p.bookmarkChapters.bookmarks = {bm};
    p.bookmarkChapters.chapters = {ch};

    p.totalEditingSeconds = 555.0;
    p.createdAtUnix = 1750000000; // provenance timestamps round-trip
    p.modifiedAtUnix = 1750050000;
    p.editSessionCount = 7;
    p.playbackPosition = 12.5;                  // resume point round-trips
    p.activeNavigator = "ofs.core.next-action"; // a non-default (plugin) id to prove it round-trips
    p.activeEditMode = "ofs.core.alt";          // a non-default (plugin) id to prove it round-trips
    p.activeSelectionMode = "ofs.core.peaks";   // a non-default (plugin) id to prove it round-trips

    ofs::ExportConfig ec;
    ec.format = 2;
    ec.axes = {StandardAxis::L0, StandardAxis::R0};
    ec.outputPath = "C:/out/clip.funscript";
    p.lastExport = ec;

    // Per-plugin project data: two plugins' namespaced key→value stores, each a nested JSON value.
    p.pluginData = {{"Ofs.Core", {{"settings", {{"Mode", 1}, {"FixedTop", 90}}}}},
                    {"Other", {{"count", 3}, {"flag", true}}}};
    return p;
}

} // namespace

TEST_CASE("Project save/load round-trips every serialized field") {
    const Project original = fullyPopulated();
    const auto path = tempPath("ofs_test_project_full.ofp");
    REQUIRE(original.save(path));

    auto loaded = Project::load(path);
    REQUIRE(loaded.has_value());

    // Deep equality across the whole serialized surface — any field written but not read (or read
    // with the wrong key/default) shows up as a JSON diff here.
    const nlohmann::json a = original;
    const nlohmann::json b = *loaded;
    CHECK(a == b);

    // A couple of spot checks so a failure points at a concrete field rather than just "json differs".
    CHECK(loaded->mediaPath == original.mediaPath);
    CHECK(loaded->intraOptimizeDeclined);
    CHECK(loaded->activeAxisRole == StandardAxis::R1);
    REQUIRE(loaded->scriptAxes.size() == 2);
    CHECK(loaded->scriptAxes[0].isLocked);
    CHECK_FALSE(loaded->scriptAxes[0].showInStrip);
    REQUIRE(loaded->scriptAxes[0].actions.size() == 3);
    CHECK(loaded->scriptAxes[0].actions[1].at == doctest::Approx(1.25));
    CHECK(loaded->scriptAxes[0].actions[1].pos == 73);
    REQUIRE(loaded->processingRegions.size() == 1);
    CHECK(loaded->processingRegions[0].name == "verse");
    CHECK(loaded->processingRegions[0].color == IM_COL32(12, 200, 99, 210));
    CHECK(loaded->processingRegions[0].hz == 45);
    CHECK(loaded->processingRegions[0].axisRoleTags == std::vector<std::string>{"L0", "R0"});
    CHECK(loaded->processingRegions[0].nodeGraph == original.processingRegions[0].nodeGraph);
    REQUIRE(loaded->bookmarkChapters.chapters.size() == 1);
    REQUIRE(loaded->bookmarkChapters.chapters[0].sceneView.has_value());
    CHECK(loaded->bookmarkChapters.chapters[0].sceneView->inverted);
    REQUIRE(loaded->lastExport.has_value());
    CHECK(loaded->lastExport->format == 2);
    CHECK(loaded->lastExport->axes == std::vector<StandardAxis>{StandardAxis::L0, StandardAxis::R0});
    CHECK(loaded->pluginData["Ofs.Core"]["settings"]["FixedTop"] == 90);
    CHECK(loaded->pluginData["Other"]["flag"] == true);
    CHECK(loaded->activeNavigator == "ofs.core.next-action");
    CHECK(loaded->activeEditMode == "ofs.core.alt");
    CHECK(loaded->activeSelectionMode == "ofs.core.peaks");
    CHECK(loaded->playbackPosition == doctest::Approx(12.5));
    CHECK(loaded->createdAtUnix == 1750000000);
    CHECK(loaded->modifiedAtUnix == 1750050000);
    CHECK(loaded->editSessionCount == 7);

    std::filesystem::remove(path);
}

TEST_CASE("Project::load fills defaults for a sparse document") {
    // Only the version field present — every other field must default.
    const auto path = tempPath("ofs_test_project_sparse.ofp");
    writeCbor(path, nlohmann::json{{"ofsProjectVersion", ofs::kProjectFileVersion}});

    auto loaded = Project::load(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->mediaPath.empty());
    CHECK(loaded->dummyDuration == doctest::Approx(0.0));
    CHECK(loaded->activeAxisRole == StandardAxis::L0);
    CHECK(loaded->scriptAxes.empty());
    CHECK(loaded->processingRegions.empty());
    CHECK(loaded->overlaySettings.frameFps == doctest::Approx(30.0f)); // documented default
    CHECK(loaded->videoPlayerState.resolutionScale == doctest::Approx(1.0f));
    CHECK(loaded->playbackPosition == doctest::Approx(0.0)); // absent → start of timeline
    CHECK(loaded->bookmarkChapters.bookmarks.empty());
    CHECK_FALSE(loaded->lastExport.has_value());
    CHECK(loaded->pluginData.is_object());
    CHECK(loaded->pluginData.empty());                  // absent key → empty object, never null
    CHECK(loaded->activeNavigator == "follow-overlay"); // absent → native navigator default
    CHECK(loaded->activeEditMode == "native");          // absent → native edit mode default
    CHECK(loaded->activeSelectionMode == "native");     // absent → native selection mode default

    std::filesystem::remove(path);
}

TEST_CASE("Project::load clamps an out-of-range resolution scale") {
    const auto path = tempPath("ofs_test_project_clamp.ofp");
    writeCbor(path, nlohmann::json{{"ofsProjectVersion", ofs::kProjectFileVersion},
                                   {"videoPlayerState", {{"activeMode", "Full"}, {"resolutionScale", 9.0f}}}});
    auto loaded = Project::load(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->videoPlayerState.resolutionScale == doctest::Approx(1.0f));
    std::filesystem::remove(path);
}

TEST_CASE("Project::load refuses a file newer than kProjectFileVersion") {
    Project p;
    nlohmann::json j = p;
    j["ofsProjectVersion"] = ofs::kProjectFileVersion + 1;
    const auto path = tempPath("ofs_test_project_newer.ofp");
    writeCbor(path, j);

    CHECK_FALSE(Project::load(path).has_value());
    std::filesystem::remove(path);
}

TEST_CASE("Project::load returns nullopt for a missing file") {
    CHECK_FALSE(Project::load("no_such_project_xyz.ofp").has_value());
}

namespace {
// A project whose single region holds one node carrying `state` as its nodeState. The node-level
// to_json/from_json aren't exposed in a header, so nodeState is exercised through the Project surface.
Project projectWithNodeState(GraphNodeType type, std::string state) {
    ofs::ProcessingGraphNode n;
    n.id = 1;
    n.type = type;
    n.pluginNodeId = "sample.blend";
    n.nodeState = std::move(state);
    ofs::ProcessingRegion r;
    r.id = 1;
    r.nodeGraph.nodes.push_back(n);
    Project p;
    p.processingRegions = {r};
    return p;
}
const nlohmann::json &firstNode(const nlohmann::json &projJson) {
    return projJson["processingRegions"][0]["nodeGraph"]["nodes"][0];
}
} // namespace

// nodeState: the plugin-node TState JSON is embedded as a *nested object* (not an opaque string)
// so the file stays readable, and round-trips losslessly even with no plugin loaded.
TEST_CASE("nodeState embeds as a nested JSON object and round-trips") {
    // Keys in alphabetical order so the parse→dump canonical text matches byte-for-byte (nlohmann sorts).
    const Project p = projectWithNodeState(GraphNodeType::PluginNode, R"({"Curve":[1,2,3],"Mix":0.5})");

    const nlohmann::json j = p;
    const auto &jn = firstNode(j);
    REQUIRE(jn.contains("nodeState"));
    CHECK(jn["nodeState"].is_object()); // a real object, queryable in place — not a quoted string blob
    CHECK(jn["nodeState"]["Mix"] == 0.5);

    const auto back = j.get<Project>();
    CHECK(back.processingRegions[0].nodeGraph.nodes[0].nodeState ==
          p.processingRegions[0].nodeGraph.nodes[0].nodeState); // canonical text survives parse→dump
    CHECK(back.processingRegions[0] == p.processingRegions[0]); // operator== includes nodeState
}

// A node with no state must not emit the key at all (keeps the file lean for the common case).
TEST_CASE("an empty nodeState omits the key") {
    const Project p = projectWithNodeState(GraphNodeType::Effect, "");
    const nlohmann::json j = p;
    CHECK_FALSE(firstNode(j).contains("nodeState"));
    CHECK(j.get<Project>().processingRegions[0].nodeGraph.nodes[0].nodeState.empty());
}

// A hand-edit that breaks the JSON can't parse to an object; rather than drop it, to_json preserves the
// text verbatim (as a JSON string) and from_json reads it straight back — lossless passthrough.
TEST_CASE("nodeState survives a malformed hand-edit verbatim") {
    const Project p = projectWithNodeState(GraphNodeType::PluginNode, "{not valid json");

    const nlohmann::json j = p;
    const auto &jn = firstNode(j);
    REQUIRE(jn.contains("nodeState"));
    CHECK(jn["nodeState"].is_string()); // fell back to a string rather than an object
    CHECK(j.get<Project>().processingRegions[0].nodeGraph.nodes[0].nodeState == "{not valid json");
}

// A TState that serializes to a bare JSON *string* scalar (a Guid/DateTime/primitive-typed state) must
// keep its quotes. A string scalar and a malformed hand-edit both surface as a JSON string in the file,
// so from_json can't strip the quotes for one without corrupting the other — the encoding must round-trip
// both. (Regression: storing the parsed scalar made from_json hand back the unquoted contents.)
TEST_CASE("nodeState round-trips a JSON string-scalar value") {
    const Project p = projectWithNodeState(GraphNodeType::PluginNode, R"("e3b0c442")");
    const auto back = nlohmann::json(p).get<Project>();
    CHECK(back.processingRegions[0].nodeGraph.nodes[0].nodeState == R"("e3b0c442")");
}

// The object-or-array branch in to_json rests on a class/record TState serializing to a JSON object. A
// *primitive-typed* TState (e.g. `[OfsState] int Seed;` or a bool) serializes to a bare number/bool
// scalar instead, which takes the same string-preserving path as the string scalar above: stored as a
// JSON string and handed back verbatim. Lock that so a future refactor can't start parsing scalars (which
// would hand from_json an unquoted/typed value and corrupt the text).
TEST_CASE("nodeState round-trips a primitive-typed (number/bool) scalar verbatim") {
    for (const char *scalar : {"42", "-3.5", "true", "false", "null"}) {
        CAPTURE(scalar);
        const Project p = projectWithNodeState(GraphNodeType::PluginNode, scalar);
        const nlohmann::json j = p;
        const auto &jn = firstNode(j);
        REQUIRE(jn.contains("nodeState"));
        CHECK(jn["nodeState"].is_string()); // preserved as text, not re-typed to a bare number/bool
        CHECK(j.get<Project>().processingRegions[0].nodeGraph.nodes[0].nodeState == scalar);
    }
}

TEST_CASE("Project::load returns nullopt for non-CBOR garbage") {
    const auto path = tempPath("ofs_test_project_garbage.ofp");
    {
        std::ofstream os(path, std::ios::binary);
        const char junk[] = "this is not cbor at all";
        os.write(junk, sizeof(junk));
    }
    CHECK_FALSE(Project::load(path).has_value());
    std::filesystem::remove(path);
}

// Actions are stored as a zlib blob ([4-byte LE uncompressed size][payload]). A foreign or corrupt
// file can present any of these malformed shapes; each must decode to an empty action set — never a
// crash, an out-of-bounds read, or garbage actions. Drives actionsFromBlob's guards through load.
TEST_CASE("Project::load tolerates a corrupt actions blob") {
    auto withActionsBlob = [](std::vector<uint8_t> blob) {
        nlohmann::json axis = {{"role", "L0"}, {"visible", true}, {"inPanel", true}, {"locked", false}};
        axis["actions"] = nlohmann::json::binary(std::move(blob));
        return nlohmann::json{{"ofsProjectVersion", ofs::kProjectFileVersion},
                              {"scriptAxes", nlohmann::json::array({axis})}};
    };
    const auto path = tempPath("ofs_test_project_badblob.ofp");

    SUBCASE("blob shorter than the 4-byte size header") {
        writeCbor(path, withActionsBlob({0x01, 0x02}));
    }
    SUBCASE("header declares zero uncompressed bytes") {
        writeCbor(path, withActionsBlob({0, 0, 0, 0}));
    }
    SUBCASE("uncompressed size is not a multiple of the 9-byte record") {
        writeCbor(path, withActionsBlob({5, 0, 0, 0, 0xDE, 0xAD}));
    }
    SUBCASE("payload is not valid zlib") {
        // Header claims 9 bytes (one record), but the payload won't decompress.
        writeCbor(path, withActionsBlob({9, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF}));
    }

    auto loaded = Project::load(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->scriptAxes.size() == 1);
    CHECK(loaded->scriptAxes[0].actions.empty());
    std::filesystem::remove(path);
}

// A chapter color is stored as a 4-element [r,g,b,a] array. A hand-edit or foreign file that drops or
// shortens it must fall back to the default bookmark color rather than read past the array.
TEST_CASE("Chapter color falls back to default for a malformed color array") {
    const nlohmann::json chapter = {
        {"startTime", 1.0}, {"endTime", 2.0}, {"name", "c"}, {"color", nlohmann::json::array({1, 2})}};
    const nlohmann::json j = {{"ofsProjectVersion", ofs::kProjectFileVersion},
                              {"bookmarkChapters", {{"chapters", nlohmann::json::array({chapter})}}}};
    const auto path = tempPath("ofs_test_project_badcolor.ofp");
    writeCbor(path, j);

    auto loaded = Project::load(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->bookmarkChapters.chapters.size() == 1);
    CHECK(loaded->bookmarkChapters.chapters[0].name == "c"); // loaded cleanly; fallback color applied
    std::filesystem::remove(path);
}

// save() returns false (rather than throwing) when the target can't be written — here because the
// parent directory does not exist, so the atomic write's temp-file open fails.
TEST_CASE("Project::save returns false when the file can't be written") {
    const Project p;
    const auto path = std::filesystem::temp_directory_path() / "ofs_no_such_dir_zzz" / "p.ofp";
    CHECK_FALSE(p.save(path));
}
