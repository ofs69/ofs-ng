#include "Core/ProcessingRegion.h"
#include "Core/StandardAxis.h"
#include "Format/Project.h"
#include "Services/EffectRegistry.h"
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>

namespace {
ofs::ProcessingNodeGraph scriptGraph(const std::string &file) {
    ofs::ProcessingNodeGraph g;
    const int in = g.allocId();
    const int s = g.allocId();
    const int out = g.allocId();
    g.nodes.push_back({.id = in, .type = ofs::GraphNodeType::Input, .role = ofs::StandardAxis::L0});
    ofs::ProcessingGraphNode sn;
    sn.id = s;
    sn.type = ofs::GraphNodeType::Script;
    sn.scriptFile = file;
    sn.scriptInputCount = 1;
    sn.role = ofs::StandardAxis::L0;
    g.nodes.push_back(sn);
    g.nodes.push_back({.id = out, .type = ofs::GraphNodeType::Output, .role = ofs::StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = in, .toNode = s, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = s, .toNode = out, .toPin = 0});
    return g;
}
const char *kScriptSrc = "// !ofs:signal functional\n// !ofs:input a\nreturn a + 10f;\n";
} // namespace

TEST_CASE("GraphPreset save/load round-trips nodes, links, and axis tags") {
    ofs::EffectRegistryState reg; // default-constructed; the default graph needs no effects
    ofs::GraphPreset preset;
    preset.name = "test";
    preset.graph = ofs::buildDefaultGraph(ofs::StandardAxis::L0);
    preset.axisRoleTags = {"L0"};

    auto path = std::filesystem::temp_directory_path() / "ofs_test_graph.json";
    REQUIRE(preset.save(path, reg));

    auto result = ofs::loadGraphPreset(path, reg);
    REQUIRE(result.preset.has_value());
    CHECK(result.error.empty());
    CHECK(result.preset->axisRoleTags == std::vector<std::string>{"L0"});
    CHECK(result.preset->graph.nodes.size() == preset.graph.nodes.size());
    CHECK(result.preset->graph.links.size() == preset.graph.links.size());

    std::filesystem::remove(path);
}

TEST_CASE("loadGraphPreset reports an error for a missing file") {
    ofs::EffectRegistryState reg;
    auto result = ofs::loadGraphPreset("no_such_graph_xyz.json", reg);
    CHECK_FALSE(result.preset.has_value());
    CHECK_FALSE(result.error.empty());
}

TEST_CASE("loadGraphPreset refuses a file newer than kGraphPresetVersion") {
    ofs::EffectRegistryState reg;
    ofs::GraphPreset preset;
    preset.graph = ofs::buildDefaultGraph(ofs::StandardAxis::L0);
    preset.axisRoleTags = {"L0"};
    auto path = std::filesystem::temp_directory_path() / "ofs_test_graph_future.json";
    REQUIRE(preset.save(path, reg));

    // Bump the on-disk version past the supported maximum, then reload.
    nlohmann::json j;
    {
        std::ifstream in(path);
        in >> j;
    }
    j["ofsGraphVersion"] = ofs::kGraphPresetVersion + 1;
    {
        std::ofstream out(path);
        out << j.dump();
    }

    auto result = ofs::loadGraphPreset(path, reg);
    CHECK_FALSE(result.preset.has_value());
    CHECK_FALSE(result.error.empty());

    std::filesystem::remove(path);
}

// ── Embedded script source travels on the node ────────────────────────────────

TEST_CASE("GraphPreset save/load round-trips a graph-embedded script's source on its node") {
    ofs::EffectRegistryState reg;
    ofs::GraphPreset preset;
    preset.graph = scriptGraph("leaky.cs");
    for (auto &n : preset.graph.nodes)
        if (n.type == ofs::GraphNodeType::Script)
            n.scriptEmbeddedSource = kScriptSrc;
    preset.axisRoleTags = {"L0"};

    auto path = std::filesystem::temp_directory_path() / "ofs_embed_preset.json";
    REQUIRE(preset.save(path, reg));

    // The source lives on the node inside nodeGraph — there is no separate top-level "scripts" map.
    nlohmann::json j;
    {
        std::ifstream in(path);
        in >> j;
    }
    CHECK_FALSE(j.contains("scripts"));

    auto result = ofs::loadGraphPreset(path, reg);
    REQUIRE(result.preset.has_value());
    const ofs::ProcessingGraphNode *sn = nullptr;
    for (const auto &n : result.preset->graph.nodes)
        if (n.type == ofs::GraphNodeType::Script)
            sn = &n;
    REQUIRE(sn != nullptr);
    CHECK(sn->scriptEmbeddedSource == kScriptSrc);

    std::filesystem::remove(path);
}

// ── Structural validation (validateGraph, driven through sanitizeProjectGraph) ─
// sanitizeProjectGraph runs the same strict structural checks loadGraphPreset uses. With no
// effect/plugin nodes the param-normalization step is a no-op, so it isolates validateGraph: each
// invariant violation below must be rejected with a non-empty reason. App-saved graphs always pass.

namespace {
using ofs::GraphNodeType;
using ofs::StandardAxis;

// Input(1) → Output(2), one link, nextId = 4. Structurally valid.
ofs::ProcessingNodeGraph okGraph() {
    return ofs::buildDefaultGraph(StandardAxis::L0);
}

bool rejects(ofs::ProcessingNodeGraph g) {
    ofs::EffectRegistryState reg;
    std::string err;
    const bool ok = ofs::sanitizeProjectGraph(g, reg, err);
    if (!ok)
        CHECK_FALSE(err.empty()); // every rejection carries a human-readable reason
    return !ok;
}
} // namespace

TEST_CASE("sanitizeProjectGraph accepts a well-formed graph") {
    ofs::ProcessingNodeGraph g = okGraph();
    ofs::EffectRegistryState reg;
    std::string err;
    CHECK(ofs::sanitizeProjectGraph(g, reg, err));
    CHECK(err.empty());
}

TEST_CASE("sanitizeProjectGraph rejects every structural invariant violation") {
    SUBCASE("too many nodes") {
        auto g = okGraph();
        for (int i = 0; i < 4096; ++i)
            g.nodes.push_back({.id = g.allocId(), .type = GraphNodeType::Input, .role = StandardAxis::L0});
        CHECK(rejects(std::move(g)));
    }
    SUBCASE("non-positive node id") {
        auto g = okGraph();
        g.nodes[0].id = 0;
        CHECK(rejects(std::move(g)));
    }
    SUBCASE("duplicate node id") {
        auto g = okGraph();
        g.nodes[1].id = g.nodes[0].id;
        CHECK(rejects(std::move(g)));
    }
    SUBCASE("non-finite node position") {
        auto g = okGraph();
        g.nodes[0].posX = std::numeric_limits<float>::quiet_NaN();
        CHECK(rejects(std::move(g)));
    }
    SUBCASE("out-of-range axis role on an Input node") {
        auto g = okGraph();
        g.nodes[0].role = static_cast<StandardAxis>(999);
        CHECK(rejects(std::move(g)));
    }
    SUBCASE("non-positive link id") {
        auto g = okGraph();
        g.links[0].id = 0;
        CHECK(rejects(std::move(g)));
    }
    SUBCASE("link id reused from a node id") {
        auto g = okGraph();
        g.links[0].id = g.nodes[0].id;
        CHECK(rejects(std::move(g)));
    }
    SUBCASE("link references a missing node") {
        auto g = okGraph();
        g.links[0].toNode = 999;
        CHECK(rejects(std::move(g)));
    }
    SUBCASE("self-loop link") {
        auto g = okGraph();
        g.links[0] = {g.allocId(), 2, 2, 0}; // Output(2) → Output(2)
        CHECK(rejects(std::move(g)));
    }
    SUBCASE("link targets a node with no inputs") {
        auto g = okGraph();
        g.links[0] = {g.allocId(), 2, 1, 0}; // → Input(1), which accepts no input
        CHECK(rejects(std::move(g)));
    }
    SUBCASE("invalid input pin") {
        auto g = okGraph();
        g.links[0].toPin = 1; // Output has only pin 0
        CHECK(rejects(std::move(g)));
    }
    SUBCASE("invalid output pin") {
        auto g = okGraph();
        g.links[0].fromPin = 1; // the source Input declares a single output (pin 0)
        CHECK(rejects(std::move(g)));
    }
    SUBCASE("two links into one input pin") {
        auto g = okGraph();
        const int extra = g.allocId();
        g.nodes.push_back({.id = extra, .type = GraphNodeType::Input, .role = StandardAxis::L0});
        g.links.push_back(
            {.id = g.allocId(), .fromNode = extra, .toNode = 2, .toPin = 0}); // second link into Output(2) pin 0
        CHECK(rejects(std::move(g)));
    }
    SUBCASE("nextId not past the largest id") {
        auto g = okGraph();
        g.nextId = 1;
        CHECK(rejects(std::move(g)));
    }
    SUBCASE("cycle") {
        ofs::ProcessingNodeGraph g;
        ofs::ProcessingGraphNode a;
        a.id = g.allocId();
        a.type = GraphNodeType::Effect;
        ofs::ProcessingGraphNode b;
        b.id = g.allocId();
        b.type = GraphNodeType::Effect;
        g.nodes = {a, b};
        g.links.push_back({.id = g.allocId(), .fromNode = a.id, .toNode = b.id, .toPin = 0});
        g.links.push_back({.id = g.allocId(), .fromNode = b.id, .toNode = a.id, .toPin = 0}); // a → b → a
        CHECK(rejects(std::move(g)));
    }
}

// ── Param normalization (sanitizeProjectGraph) ────────────────────────────────

namespace {
// A registry whose one effect "smooth" declares three params with defaults 1,2,3.
ofs::EffectRegistryState regWithSmooth() {
    ofs::EffectRegistryState reg;
    ofs::EffectDefinition def;
    def.type = "smooth";
    def.paramDefs = {
        {.key = "a", .defaultValue = 1.0f}, {.key = "b", .defaultValue = 2.0f}, {.key = "c", .defaultValue = 3.0f}};
    reg.effects["smooth"] = def;
    reg.orderedKeys = {"smooth"};
    return reg;
}

// Input → Effect(smooth, params) → Output. Returns the graph; the Effect node is nodes[1].
ofs::ProcessingNodeGraph smoothGraph(std::vector<float> params) {
    ofs::ProcessingNodeGraph g;
    const int in = g.allocId();
    const int eff = g.allocId();
    const int out = g.allocId();
    g.nodes.push_back({.id = in, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    ofs::ProcessingGraphNode e;
    e.id = eff;
    e.type = GraphNodeType::Effect;
    e.effect.type = "smooth";
    e.effect.params = std::move(params);
    e.role = StandardAxis::L0;
    g.nodes.push_back(e);
    g.nodes.push_back({.id = out, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = in, .toNode = eff, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = eff, .toNode = out, .toPin = 0});
    return g;
}
} // namespace

TEST_CASE("sanitizeProjectGraph pads a short param list to the declared arity") {
    const ofs::EffectRegistryState reg = regWithSmooth();
    auto g = smoothGraph({}); // empty — a fuzzer crash: kernels deref params.data()
    std::string err;
    REQUIRE(ofs::sanitizeProjectGraph(g, reg, err));
    CHECK(g.nodes[1].effect.params == std::vector<float>{1.0f, 2.0f, 3.0f});
}

TEST_CASE("sanitizeProjectGraph truncates an over-long param list") {
    const ofs::EffectRegistryState reg = regWithSmooth();
    auto g = smoothGraph({9.0f, 8.0f, 7.0f, 6.0f, 5.0f});
    std::string err;
    REQUIRE(ofs::sanitizeProjectGraph(g, reg, err));
    CHECK(g.nodes[1].effect.params == std::vector<float>{9.0f, 8.0f, 7.0f});
}

TEST_CASE("sanitizeProjectGraph leaves a missing-dependency node's params untouched") {
    ofs::EffectRegistryState reg; // "smooth" not registered
    auto g = smoothGraph({42.0f});
    std::string err;
    REQUIRE(ofs::sanitizeProjectGraph(g, reg, err));
    CHECK(g.nodes[1].effect.params == std::vector<float>{42.0f});
}

// ── Registry-aware, name-keyed params (GraphPreset::save ↔ loadGraphPreset) ────

TEST_CASE("GraphPreset round-trips params by name and reconciles against the registry") {
    const ofs::EffectRegistryState reg = regWithSmooth();
    ofs::GraphPreset preset;
    preset.graph = smoothGraph({5.0f, 6.0f, 7.0f});
    preset.axisRoleTags = {"L0"};
    const auto path = std::filesystem::temp_directory_path() / "ofs_test_graph_named.json";
    REQUIRE(preset.save(path, reg));

    // Saved as a name-keyed object, not a positional array.
    nlohmann::json j;
    {
        std::ifstream in(path);
        in >> j;
    }
    const auto &effJson = j["nodeGraph"]["nodes"][1]["effect"]["params"];
    REQUIRE(effJson.is_object());
    CHECK(effJson["a"] == 5.0f);
    CHECK(effJson["c"] == 7.0f);

    auto result = ofs::loadGraphPreset(path, reg);
    REQUIRE(result.preset.has_value());
    CHECK(result.missingDeps.empty());
    CHECK(result.preset->graph.nodes[1].effect.params == std::vector<float>{5.0f, 6.0f, 7.0f});

    std::filesystem::remove(path);
}

TEST_CASE("loadGraphPreset fills a default for a param dropped from the saved file") {
    const ofs::EffectRegistryState reg = regWithSmooth();
    ofs::GraphPreset preset;
    preset.graph = smoothGraph({5.0f, 6.0f, 7.0f});
    preset.axisRoleTags = {"L0"};
    const auto path = std::filesystem::temp_directory_path() / "ofs_test_graph_dropparam.json";
    REQUIRE(preset.save(path, reg));

    nlohmann::json j;
    {
        std::ifstream in(path);
        in >> j;
    }
    j["nodeGraph"]["nodes"][1]["effect"]["params"].erase("b"); // drop the middle param
    {
        std::ofstream out(path);
        out << j.dump();
    }

    auto result = ofs::loadGraphPreset(path, reg);
    REQUIRE(result.preset.has_value());
    // 'b' falls back to its registry default (2); a and c keep their saved values.
    CHECK(result.preset->graph.nodes[1].effect.params == std::vector<float>{5.0f, 2.0f, 7.0f});

    std::filesystem::remove(path);
}

TEST_CASE("loadGraphPreset reports missing dependencies but still loads") {
    ofs::EffectRegistryState saveReg = regWithSmooth();
    ofs::GraphPreset preset;
    preset.graph = smoothGraph({5.0f, 6.0f, 7.0f});
    preset.axisRoleTags = {"L0"};
    const auto path = std::filesystem::temp_directory_path() / "ofs_test_graph_missingdep.json";
    REQUIRE(preset.save(path, saveReg));

    ofs::EffectRegistryState emptyReg; // "smooth" not installed on the loading side
    auto result = ofs::loadGraphPreset(path, emptyReg);
    REQUIRE(result.preset.has_value()); // load still succeeds; the node is just inactive
    REQUIRE(result.missingDeps.size() == 1);
    CHECK(result.missingDeps[0] == "effect: smooth");

    std::filesystem::remove(path);
}

// ── Plugin nodes (no scalar params; reported as missing deps by id) ───────────

namespace {
// PluginNode(source) → Output. A plugin node carries no positional params, so save() serializes its
// params via the array fallback and reports the node as a "plugin:" dependency on load.
ofs::ProcessingNodeGraph pluginGraph(const std::string &pluginId) {
    ofs::ProcessingNodeGraph g;
    const int p = g.allocId();
    const int out = g.allocId();
    ofs::ProcessingGraphNode pn;
    pn.id = p;
    pn.type = GraphNodeType::PluginNode;
    pn.pluginNodeId = pluginId;
    pn.pluginInputCount = 0; // generator/source — no inputs
    pn.role = StandardAxis::L0;
    g.nodes.push_back(pn);
    g.nodes.push_back({.id = out, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = p, .toNode = out, .toPin = 0});
    return g;
}
} // namespace

TEST_CASE("GraphPreset round-trips a plugin node and reports it as a missing dependency") {
    ofs::EffectRegistryState reg; // the plugin node's id is not installed
    ofs::GraphPreset preset;
    preset.graph = pluginGraph("acme.lfo");
    preset.axisRoleTags = {"L0"};
    const auto path = std::filesystem::temp_directory_path() / "ofs_test_graph_plugin.json";
    REQUIRE(preset.save(path, reg));

    // A plugin node names no scalar params, so save() emits the positional array fallback.
    nlohmann::json j;
    {
        std::ifstream in(path);
        in >> j;
    }
    CHECK(j["nodeGraph"]["nodes"][0]["effect"]["params"].is_array());

    auto result = ofs::loadGraphPreset(path, reg);
    REQUIRE(result.preset.has_value());
    REQUIRE(result.missingDeps.size() == 1);
    CHECK(result.missingDeps[0] == "plugin: acme.lfo");

    std::filesystem::remove(path);
}

// ── loadGraphPreset parse / structure error paths ─────────────────────────────

namespace {
std::filesystem::path writeText(const char *name, const std::string &text) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path);
    out << text;
    return path;
}
} // namespace

TEST_CASE("loadGraphPreset reports an error for invalid JSON") {
    ofs::EffectRegistryState reg;
    const auto path = writeText("ofs_test_graph_badjson.json", "{ this is not json");
    auto result = ofs::loadGraphPreset(path, reg);
    CHECK_FALSE(result.preset.has_value());
    CHECK_FALSE(result.error.empty());
    std::filesystem::remove(path);
}

TEST_CASE("loadGraphPreset reports an error when nodeGraph is absent") {
    ofs::EffectRegistryState reg;
    const auto path =
        writeText("ofs_test_graph_nograph.json", nlohmann::json{{"ofsGraphVersion", ofs::kGraphPresetVersion}}.dump());
    auto result = ofs::loadGraphPreset(path, reg);
    CHECK_FALSE(result.preset.has_value());
    CHECK_FALSE(result.error.empty());
    std::filesystem::remove(path);
}

TEST_CASE("loadGraphPreset reports an error when nodeGraph is the wrong type") {
    ofs::EffectRegistryState reg;
    const auto path =
        writeText("ofs_test_graph_badtype.json",
                  nlohmann::json{{"ofsGraphVersion", ofs::kGraphPresetVersion}, {"nodeGraph", 42}}.dump());
    auto result = ofs::loadGraphPreset(path, reg);
    CHECK_FALSE(result.preset.has_value());
    CHECK_FALSE(result.error.empty());
    std::filesystem::remove(path);
}

TEST_CASE("loadGraphPreset rejects a structurally invalid graph") {
    ofs::EffectRegistryState reg;
    ofs::GraphPreset preset;
    preset.graph = ofs::buildDefaultGraph(ofs::StandardAxis::L0);
    preset.axisRoleTags = {"L0"};
    const auto path = std::filesystem::temp_directory_path() / "ofs_test_graph_invalid.json";
    REQUIRE(preset.save(path, reg));

    nlohmann::json j;
    {
        std::ifstream in(path);
        in >> j;
    }
    j["nodeGraph"]["nodes"][0]["id"] = 0; // non-positive id → validateGraph rejects
    {
        std::ofstream out(path);
        out << j.dump();
    }

    auto result = ofs::loadGraphPreset(path, reg);
    CHECK_FALSE(result.preset.has_value());
    CHECK_FALSE(result.error.empty());
    std::filesystem::remove(path);
}

TEST_CASE("GraphPreset::save returns false when the file can't be written") {
    ofs::EffectRegistryState reg;
    ofs::GraphPreset preset;
    preset.graph = ofs::buildDefaultGraph(ofs::StandardAxis::L0);
    const auto path = std::filesystem::temp_directory_path() / "ofs_no_such_dir_zzz" / "g.json";
    CHECK_FALSE(preset.save(path, reg));
}

// ── Positional-array param fallback (definition unavailable at save time) ─────

TEST_CASE("GraphPreset saves params positionally when the effect's definition is unavailable") {
    // Saving with a registry that lacks "smooth": paramNames() can't name the slots, so the params
    // serialize as a positional array rather than a name-keyed object — values aren't lost.
    ofs::EffectRegistryState emptyReg;
    ofs::GraphPreset preset;
    preset.graph = smoothGraph({5.0f, 6.0f, 7.0f});
    preset.axisRoleTags = {"L0"};
    const auto path = std::filesystem::temp_directory_path() / "ofs_test_graph_arrayparams.json";
    REQUIRE(preset.save(path, emptyReg));

    nlohmann::json j;
    {
        std::ifstream in(path);
        in >> j;
    }
    CHECK(j["nodeGraph"]["nodes"][1]["effect"]["params"].is_array()); // array fallback, not an object

    // Loading without the definition reconciles the positional array verbatim (node stays inactive).
    auto result = ofs::loadGraphPreset(path, emptyReg);
    REQUIRE(result.preset.has_value());
    CHECK(result.preset->graph.nodes[1].effect.params == std::vector<float>{5.0f, 6.0f, 7.0f});

    std::filesystem::remove(path);
}

TEST_CASE("loadGraphPreset maps a positional-array param file onto the definition by index") {
    // A file whose params are a positional array (e.g. saved before the effect was installed) must load
    // by position once the definition becomes available — the legacy positional fallback in reconcile.
    ofs::GraphPreset preset;
    preset.graph = smoothGraph({5.0f, 6.0f, 7.0f});
    preset.axisRoleTags = {"L0"};
    const auto path = std::filesystem::temp_directory_path() / "ofs_test_graph_posarray.json";
    REQUIRE(preset.save(path, ofs::EffectRegistryState{})); // empty reg → positional array on disk

    const ofs::EffectRegistryState reg = regWithSmooth();
    auto result = ofs::loadGraphPreset(path, reg);
    REQUIRE(result.preset.has_value());
    CHECK(result.missingDeps.empty());
    CHECK(result.preset->graph.nodes[1].effect.params == std::vector<float>{5.0f, 6.0f, 7.0f});

    std::filesystem::remove(path);
}
