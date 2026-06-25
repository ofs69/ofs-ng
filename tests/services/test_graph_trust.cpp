#include <doctest/doctest.h>

#include "Core/Events.h"
#include "Core/ProcessingRegion.h"
#include "Util/FileUtil.h"
#include "Util/PathUtil.h"
#include "Util/Resources.h"
#include "helpers/PmFixture.h"

#include <filesystem>
#include <nlohmann/json.hpp>

using namespace ofs;
using ofs::test::PmFixture;

namespace {
constexpr const char *kSrc = "// !ofs:signal functional\n// !ofs:input a\nreturn a;\n";

// Input(axis) -> graph-embedded Script(carries `source`, named `file`) -> Output(axis).
ProcessingNodeGraph trustGraph(const std::string &file, StandardAxis axis, const std::string &source = kSrc) {
    ProcessingNodeGraph g;
    const int in = g.allocId();
    const int s = g.allocId();
    const int out = g.allocId();
    g.nodes.push_back({.id = in, .type = GraphNodeType::Input, .role = axis});
    ProcessingGraphNode sn;
    sn.id = s;
    sn.type = GraphNodeType::Script;
    sn.scriptFile = file;
    sn.scriptEmbeddedSource = source;
    sn.scriptInputCount = 1;
    sn.role = axis;
    g.nodes.push_back(sn);
    g.nodes.push_back({.id = out, .type = GraphNodeType::Output, .role = axis});
    g.links.push_back({.id = g.allocId(), .fromNode = in, .toNode = s, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = s, .toNode = out, .toPin = 0});
    return g;
}
} // namespace

TEST_CASE("ConfirmGraphTrust keeps scripts embedded (not on disk) and applies when axes match") {
    PmFixture f;
    const auto scriptsDir = ofs::util::getPrefPath() / "scripts";
    std::error_code ec;
    std::filesystem::remove(scriptsDir / "t.cs", ec);

    ProcessingRegion r;
    r.id = 7;
    r.startTime = 0.0;
    r.endTime = 10.0;
    r.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    r.nodeGraph = buildDefaultGraph(StandardAxis::L0);
    f.project().regions.push_back(r);
    f.project().sortRegions();

    // A pending load carrying embedded code, as onLoadGraph would stash it (trust not yet given).
    PendingGraphLoad pend;
    pend.regionId = 7;
    pend.graph = trustGraph("t.cs", StandardAxis::L0);
    pend.savedAxes = {StandardAxis::L0};
    pend.name = "loaded";
    pend.needsTrust = true;
    f.project().pendingGraphLoad = pend;

    f.send(ConfirmGraphTrustEvent{7});

    // Accepting does NOT write to the scripts folder: the script stays embedded in the graph. The
    // graph is applied (axes match) and the pending load is gone.
    CHECK_FALSE(std::filesystem::exists(scriptsDir / "t.cs"));
    CHECK_FALSE(f.project().pendingGraphLoad.has_value());

    auto *region = f.project().findRegion(7);
    REQUIRE(region != nullptr);
    CHECK(region->name == "loaded");
    const ProcessingGraphNode *scriptNode = nullptr;
    for (const auto &n : region->nodeGraph.nodes)
        if (n.type == GraphNodeType::Script)
            scriptNode = &n;
    REQUIRE(scriptNode != nullptr);
    CHECK(scriptNode->scriptEmbedded());
    CHECK(scriptNode->scriptEmbeddedSource == kSrc);
}

TEST_CASE("ConfirmGraphTrust keeps a now-trusted pending load when axes differ (for remap)") {
    PmFixture f;
    const auto scriptsDir = ofs::util::getPrefPath() / "scripts";
    std::error_code ec;
    std::filesystem::remove(scriptsDir / "t2.cs", ec);

    ProcessingRegion r;
    r.id = 8;
    r.startTime = 0.0;
    r.endTime = 10.0;
    r.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    r.nodeGraph = buildDefaultGraph(StandardAxis::L0);
    f.project().regions.push_back(r);
    f.project().sortRegions();

    // Graph targets a different axis (L1) than the region (L0).
    PendingGraphLoad pend;
    pend.regionId = 8;
    pend.graph = trustGraph("t2.cs", StandardAxis::L1);
    pend.savedAxes = {StandardAxis::L1};
    pend.name = "loaded2";
    pend.needsTrust = true;
    f.project().pendingGraphLoad = pend;

    f.send(ConfirmGraphTrustEvent{8});

    // Nothing written to disk; the script stays embedded on the pending graph's node and the load
    // stays pending (now trusted) for the remap dialog.
    CHECK_FALSE(std::filesystem::exists(scriptsDir / "t2.cs"));
    REQUIRE(f.project().pendingGraphLoad.has_value());
    CHECK_FALSE(f.project().pendingGraphLoad->needsTrust);
    const ProcessingGraphNode *scriptNode = nullptr;
    for (const auto &n : f.project().pendingGraphLoad->graph.nodes)
        if (n.type == GraphNodeType::Script)
            scriptNode = &n;
    REQUIRE(scriptNode != nullptr);
    CHECK(scriptNode->scriptEmbeddedSource == kSrc);
}

TEST_CASE("ConfirmGraphTrust records the graph's code so it is not re-prompted; identical code dedups") {
    const auto trustFile = ofs::util::getPrefPath() / "trusted_graphs.json";
    std::error_code ec;
    std::filesystem::remove(trustFile, ec); // start from a clean trust store

    auto confirmEmbedded = [](StandardAxis axis, int regionId, const std::string &file) {
        PmFixture f;
        ProcessingRegion r;
        r.id = regionId;
        r.startTime = 0.0;
        r.endTime = 10.0;
        r.axisRoles.set(static_cast<size_t>(axis));
        r.nodeGraph = buildDefaultGraph(axis);
        f.project().regions.push_back(r);
        f.project().sortRegions();

        PendingGraphLoad pend;
        pend.regionId = regionId;
        pend.graph = trustGraph(file, axis);
        pend.savedAxes = {axis};
        pend.name = "loaded";
        pend.needsTrust = true;
        f.project().pendingGraphLoad = pend;
        f.send(ConfirmGraphTrustEvent{regionId});
    };

    // First accept persists exactly one hash of the embedded code.
    confirmEmbedded(StandardAxis::L0, 1, "a.cs");
    auto text = ofs::util::readFile(trustFile);
    REQUIRE(text.has_value());
    auto stored = nlohmann::json::parse(*text);
    REQUIRE(stored.is_array());
    CHECK(stored.size() == 1);

    // A different graph carrying the SAME embedded source hashes the same — no duplicate is added,
    // which is what lets an already-trusted graph reload without prompting again.
    confirmEmbedded(StandardAxis::L0, 2, "b.cs");
    text = ofs::util::readFile(trustFile);
    REQUIRE(text.has_value());
    CHECK(nlohmann::json::parse(*text).size() == 1);

    std::filesystem::remove(trustFile, ec);
}

// A shipped library script dropped from the add menu is embedded verbatim from data.pak. Because it is
// first-party code, ProcessingManager auto-trusts it: its source hashes into the shipped-hash set, so
// graphTrustHash excludes it and a graph carrying only shipped scripts records (and prompts) nothing.
TEST_CASE("ConfirmGraphTrust auto-trusts shipped library scripts (records nothing)") {
    const auto trustFile = ofs::util::getPrefPath() / "trusted_graphs.json";
    std::error_code ec;
    std::filesystem::remove(trustFile, ec); // start from a clean trust store

    // The exact bytes the add menu embeds and that ProjectManager hashes into its auto-trusted set.
    auto shipped = ofs::res::readText("data/scripts/lib/Scale.cs");
    REQUIRE(shipped.has_value());

    PmFixture f;
    ProcessingRegion r;
    r.id = 31;
    r.startTime = 0.0;
    r.endTime = 10.0;
    r.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    r.nodeGraph = buildDefaultGraph(StandardAxis::L0);
    f.project().regions.push_back(r);
    f.project().sortRegions();

    PendingGraphLoad pend;
    pend.regionId = 31;
    pend.graph = trustGraph("Scale.cs", StandardAxis::L0, *shipped);
    pend.savedAxes = {StandardAxis::L0};
    pend.name = "loaded";
    pend.needsTrust = true;
    f.project().pendingGraphLoad = pend;

    f.send(ConfirmGraphTrustEvent{31});

    // Pure-shipped graph: nothing persisted (the trust store file is never even created).
    CHECK_FALSE(std::filesystem::exists(trustFile));
}

// A graph mixing a shipped script with a user-authored embedded script still records exactly one hash —
// the user-authored code — proving the shipped one is excluded from the digest, not the whole graph.
TEST_CASE("ConfirmGraphTrust records only the user-authored script in a mixed graph") {
    const auto trustFile = ofs::util::getPrefPath() / "trusted_graphs.json";
    std::error_code ec;
    std::filesystem::remove(trustFile, ec);

    auto shipped = ofs::res::readText("data/scripts/lib/Scale.cs");
    REQUIRE(shipped.has_value());

    PmFixture f;
    ProcessingRegion r;
    r.id = 32;
    r.startTime = 0.0;
    r.endTime = 10.0;
    r.axisRoles.set(static_cast<size_t>(StandardAxis::L0));
    r.nodeGraph = buildDefaultGraph(StandardAxis::L0);
    f.project().regions.push_back(r);
    f.project().sortRegions();

    // Input -> shippedScript -> userScript(kSrc) -> Output. Wiring is irrelevant to the trust digest;
    // what matters is that two distinct embedded sources are present.
    ProcessingNodeGraph g;
    const int in = g.allocId();
    const int shippedNode = g.allocId();
    const int userNode = g.allocId();
    const int out = g.allocId();
    g.nodes.push_back({.id = in, .type = GraphNodeType::Input, .role = StandardAxis::L0});
    ProcessingGraphNode sShipped;
    sShipped.id = shippedNode;
    sShipped.type = GraphNodeType::Script;
    sShipped.scriptFile = "Scale.cs";
    sShipped.scriptEmbeddedSource = *shipped;
    sShipped.scriptInputCount = 1;
    sShipped.role = StandardAxis::L0;
    g.nodes.push_back(sShipped);
    ProcessingGraphNode sUser;
    sUser.id = userNode;
    sUser.type = GraphNodeType::Script;
    sUser.scriptFile = "mine.cs";
    sUser.scriptEmbeddedSource = kSrc;
    sUser.scriptInputCount = 1;
    sUser.role = StandardAxis::L0;
    g.nodes.push_back(sUser);
    g.nodes.push_back({.id = out, .type = GraphNodeType::Output, .role = StandardAxis::L0});
    g.links.push_back({.id = g.allocId(), .fromNode = in, .toNode = shippedNode, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = shippedNode, .toNode = userNode, .toPin = 0});
    g.links.push_back({.id = g.allocId(), .fromNode = userNode, .toNode = out, .toPin = 0});

    PendingGraphLoad pend;
    pend.regionId = 32;
    pend.graph = std::move(g);
    pend.savedAxes = {StandardAxis::L0};
    pend.name = "loaded";
    pend.needsTrust = true;
    f.project().pendingGraphLoad = pend;

    f.send(ConfirmGraphTrustEvent{32});

    // Exactly one hash recorded — the user-authored script; the shipped one was auto-trusted out.
    auto text = ofs::util::readFile(trustFile);
    REQUIRE(text.has_value());
    CHECK(nlohmann::json::parse(*text).size() == 1);

    std::filesystem::remove(trustFile, ec);
}
