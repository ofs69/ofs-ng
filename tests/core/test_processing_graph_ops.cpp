#include "Services/EffectRegistry.h"
#include "UI/ProcessingGraphOps.h"
#include <doctest/doctest.h>

using namespace ofs;

namespace {

// A registry with one effect of each shape the functional-propagation logic distinguishes:
// a pure functional generator (ignores input), a functional modifier (depends on input), and a
// discrete effect. Built by hand so the test needs no native-effect registration source.
EffectRegistryState makeRegistry() {
    EffectRegistryState reg;
    auto add = [&](const char *type, EffectKind kind, bool ignoresInput) {
        EffectDefinition def;
        def.kind = kind;
        def.type = type;
        def.ignoresInput = ignoresInput;
        reg.effects[type] = def;
    };
    add("gen", EffectKind::Functional, /*ignoresInput=*/true);
    add("mod", EffectKind::Functional, /*ignoresInput=*/false);
    add("disc", EffectKind::Discrete, /*ignoresInput=*/false);
    return reg;
}

ProcessingGraphLink link(int id, int from, int to, int toPin = 0) {
    return {.id = id, .fromNode = from, .toNode = to, .toPin = toPin};
}

} // namespace

TEST_CASE("isMathNode classifies the four arithmetic node types") {
    CHECK(isMathNode(GraphNodeType::Add));
    CHECK(isMathNode(GraphNodeType::Subtract));
    CHECK(isMathNode(GraphNodeType::Multiply));
    CHECK(isMathNode(GraphNodeType::Divide));

    CHECK_FALSE(isMathNode(GraphNodeType::Constant));
    CHECK_FALSE(isMathNode(GraphNodeType::Input));
    CHECK_FALSE(isMathNode(GraphNodeType::Output));
    CHECK_FALSE(isMathNode(GraphNodeType::Effect));
    CHECK_FALSE(isMathNode(GraphNodeType::PluginNode));
    CHECK_FALSE(isMathNode(GraphNodeType::Script));
}

TEST_CASE("wouldCreateCycle detects back-edges in a chain") {
    ProcessingNodeGraph g;
    g.links = {link(10, 1, 2), link(11, 2, 3)}; // 1 → 2 → 3

    // A back-edge 3 → 1 closes the loop (1 already reaches 3).
    CHECK(wouldCreateCycle(/*fromNode=*/3, /*toNode=*/1, g));
    // A forward edge 1 → 3 does not (3 reaches nothing).
    CHECK_FALSE(wouldCreateCycle(/*fromNode=*/1, /*toNode=*/3, g));
    // An unrelated node never reaches the chain.
    CHECK_FALSE(wouldCreateCycle(/*fromNode=*/3, /*toNode=*/99, g));
}

TEST_CASE("wouldCreateCycle handles a diamond without false positives") {
    ProcessingNodeGraph g;
    g.links = {link(10, 1, 2), link(11, 1, 3), link(12, 2, 4), link(13, 3, 4)}; // 1 ⇒ {2,3} ⇒ 4

    // 4 reaches 1 via either branch (visited set must not loop forever on the shared node 4).
    CHECK(wouldCreateCycle(/*fromNode=*/4, /*toNode=*/1, g));
    // 1 → 4 is a forward edge; the sink reaches nothing.
    CHECK_FALSE(wouldCreateCycle(/*fromNode=*/1, /*toNode=*/4, g));
}

TEST_CASE("autoConnectNewNode: dropped on an output pin inserts downstream") {
    ProcessingNodeGraph g;
    g.links = {link(10, 1, 2)}; // 1 → 2
    g.nextId = 100;

    // Drop the new node (id 5) on node 1's output pin: node 1's consumers reroute through 5,
    // and 1 feeds 5.
    autoConnectNewNode(/*newNodeId=*/5, GraphId::outPin(1), /*ignoresInput=*/false, g);

    REQUIRE(g.links.size() == 2);
    // Original link to node 2 now originates from the new node.
    CHECK(g.findLinkToPin(2, 0)->fromNode == 5);
    // New node is fed by node 1 on its primary input.
    CHECK(g.findLinkToPin(5, 0)->fromNode == 1);
}

TEST_CASE("autoConnectNewNode: dropped on an output pin is a no-op when the node ignores input") {
    ProcessingNodeGraph g;
    g.links = {link(10, 1, 2)};
    g.nextId = 100;

    autoConnectNewNode(/*newNodeId=*/5, GraphId::outPin(1), /*ignoresInput=*/true, g);

    // A generator has no input to splice into, so the graph is untouched.
    REQUIRE(g.links.size() == 1);
    CHECK(g.links[0].fromNode == 1);
    CHECK(g.links[0].toNode == 2);
}

TEST_CASE("autoConnectNewNode: dropped on a connected input pin rewires the old source through the new node") {
    ProcessingNodeGraph g;
    g.links = {link(10, 1, 2)}; // node 1 feeds node 2's input 0
    g.nextId = 100;

    autoConnectNewNode(/*newNodeId=*/5, GraphId::inPin(2, 0), /*ignoresInput=*/false, g);

    REQUIRE(g.links.size() == 2);
    CHECK(g.findLinkToPin(2, 0)->fromNode == 5); // new node now drives node 2
    CHECK(g.findLinkToPin(5, 0)->fromNode == 1); // old source backfeeds the new node
}

TEST_CASE("autoConnectNewNode: input-pin drop skips the backfeed when the new node ignores input") {
    ProcessingNodeGraph g;
    g.links = {link(10, 1, 2)};
    g.nextId = 100;

    autoConnectNewNode(/*newNodeId=*/5, GraphId::inPin(2, 0), /*ignoresInput=*/true, g);

    REQUIRE(g.links.size() == 1);
    CHECK(g.findLinkToPin(2, 0)->fromNode == 5);
    CHECK(g.findLinkToPin(5, 0) == nullptr); // no old-source backfeed for a generator
}

TEST_CASE("autoConnectNewNode: dropped on an unconnected input pin just adds the feed") {
    ProcessingNodeGraph g;
    g.nextId = 100; // no existing links

    autoConnectNewNode(/*newNodeId=*/5, GraphId::inPin(2, 0), /*ignoresInput=*/false, g);

    REQUIRE(g.links.size() == 1);
    CHECK(g.findLinkToPin(2, 0)->fromNode == 5);
}

TEST_CASE("isNodeOutputFunctional: leaf node types") {
    const EffectRegistryState reg = makeRegistry();
    ProcessingNodeGraph g;
    g.nodes = {
        {.id = 1, .type = GraphNodeType::Constant},
        {.id = 2, .type = GraphNodeType::Input},
    };

    CHECK(isNodeOutputFunctional(1, g, reg));        // a constant is always functional
    CHECK_FALSE(isNodeOutputFunctional(2, g, reg));  // an Input emits the discrete source signal
    CHECK_FALSE(isNodeOutputFunctional(99, g, reg)); // a missing node is not functional
}

TEST_CASE("isNodeOutputFunctional: effect nodes propagate through their input") {
    const EffectRegistryState reg = makeRegistry();

    SUBCASE("functional generator ignores input") {
        ProcessingNodeGraph g;
        g.nodes = {{.id = 3, .type = GraphNodeType::Effect, .effect = {.type = "gen"}}};
        CHECK(isNodeOutputFunctional(3, g, reg)); // functional regardless of any input
    }
    SUBCASE("discrete effect is never functional") {
        ProcessingNodeGraph g;
        g.nodes = {{.id = 3, .type = GraphNodeType::Effect, .effect = {.type = "disc"}}};
        CHECK_FALSE(isNodeOutputFunctional(3, g, reg));
    }
    SUBCASE("unknown effect type is not functional") {
        ProcessingNodeGraph g;
        g.nodes = {{.id = 3, .type = GraphNodeType::Effect, .effect = {.type = "missing"}}};
        CHECK_FALSE(isNodeOutputFunctional(3, g, reg));
    }
    SUBCASE("functional modifier depends on its input") {
        ProcessingNodeGraph g;
        g.nodes = {
            {.id = 1, .type = GraphNodeType::Constant},
            {.id = 2, .type = GraphNodeType::Input},
            {.id = 3, .type = GraphNodeType::Effect, .effect = {.type = "mod"}},
        };
        CHECK_FALSE(isNodeOutputFunctional(3, g, reg)); // no input → not functional

        g.links = {link(10, 1, 3)}; // fed by a functional constant
        CHECK(isNodeOutputFunctional(3, g, reg));

        g.links = {link(10, 2, 3)}; // fed by a discrete Input
        CHECK_FALSE(isNodeOutputFunctional(3, g, reg));
    }
}

TEST_CASE("isNodeOutputFunctional: math node needs both inputs functional") {
    const EffectRegistryState reg = makeRegistry();
    ProcessingNodeGraph g;
    g.nodes = {
        {.id = 1, .type = GraphNodeType::Constant},
        {.id = 2, .type = GraphNodeType::Input},
        {.id = 6, .type = GraphNodeType::Add},
    };

    SUBCASE("both inputs functional") {
        g.links = {link(10, 1, 6, 0), link(11, 1, 6, 1)};
        CHECK(isNodeOutputFunctional(6, g, reg));
    }
    SUBCASE("primary functional, secondary unconnected (defaults to constant 50)") {
        g.links = {link(10, 1, 6, 0)};
        CHECK(isNodeOutputFunctional(6, g, reg)); // missing B is treated as functional
    }
    SUBCASE("primary discrete fails") {
        g.links = {link(10, 2, 6, 0), link(11, 1, 6, 1)};
        CHECK_FALSE(isNodeOutputFunctional(6, g, reg));
    }
    SUBCASE("no primary input fails") {
        CHECK_FALSE(isNodeOutputFunctional(6, g, reg));
    }
}

TEST_CASE("isNodeOutputFunctional: plugin nodes") {
    const EffectRegistryState reg = makeRegistry();

    SUBCASE("functional generator is functional") {
        ProcessingNodeGraph g;
        g.nodes = {{.id = 4,
                    .type = GraphNodeType::PluginNode,
                    .pluginInputCount = 0,
                    .pluginSignal = static_cast<uint8_t>(OfsSignalFunctional)}};
        CHECK(isNodeOutputFunctional(4, g, reg));
    }
    SUBCASE("discrete-signal plugin node is not functional") {
        ProcessingNodeGraph g;
        g.nodes = {{.id = 4,
                    .type = GraphNodeType::PluginNode,
                    .pluginInputCount = 0,
                    .pluginSignal = static_cast<uint8_t>(OfsSignalDiscrete)}};
        CHECK_FALSE(isNodeOutputFunctional(4, g, reg));
    }
    SUBCASE("functional modifier depends on input A") {
        ProcessingNodeGraph g;
        g.nodes = {
            {.id = 1, .type = GraphNodeType::Constant},
            {.id = 4,
             .type = GraphNodeType::PluginNode,
             .pluginInputCount = 1,
             .pluginSignal = static_cast<uint8_t>(OfsSignalFunctional)},
        };
        CHECK_FALSE(isNodeOutputFunctional(4, g, reg)); // unconnected
        g.links = {link(10, 1, 4, 0)};
        CHECK(isNodeOutputFunctional(4, g, reg));
    }
    SUBCASE("functional combiner also gates on its second input B") {
        ProcessingNodeGraph g;
        g.nodes = {
            {.id = 1, .type = GraphNodeType::Constant},
            {.id = 2, .type = GraphNodeType::Input},
            {.id = 4,
             .type = GraphNodeType::PluginNode,
             .pluginInputCount = 2,
             .pluginSignal = static_cast<uint8_t>(OfsSignalFunctional)},
        };
        g.links = {link(10, 1, 4, 0)}; // A functional, B unconnected → still functional
        CHECK(isNodeOutputFunctional(4, g, reg));

        g.links = {link(10, 1, 4, 0), link(11, 2, 4, 1)}; // B is the discrete Input → fails
        CHECK_FALSE(isNodeOutputFunctional(4, g, reg));

        g.links = {link(10, 1, 4, 0), link(11, 1, 4, 1)}; // both functional → functional
        CHECK(isNodeOutputFunctional(4, g, reg));
    }
}

TEST_CASE("isNodeOutputFunctional: script nodes") {
    const EffectRegistryState reg = makeRegistry();

    SUBCASE("functional 0-input script (generator) is functional") {
        ProcessingNodeGraph g;
        g.nodes = {{.id = 7,
                    .type = GraphNodeType::Script,
                    .scriptSignal = static_cast<uint8_t>(OfsSignalFunctional),
                    .scriptInputCount = 0}};
        CHECK(isNodeOutputFunctional(7, g, reg));
    }
    SUBCASE("discrete-signal script is not functional") {
        ProcessingNodeGraph g;
        g.nodes = {{.id = 7,
                    .type = GraphNodeType::Script,
                    .scriptSignal = static_cast<uint8_t>(OfsSignalDiscrete),
                    .scriptInputCount = 0}};
        CHECK_FALSE(isNodeOutputFunctional(7, g, reg));
    }
    SUBCASE("functional 1-input script depends on its input") {
        ProcessingNodeGraph g;
        g.nodes = {
            {.id = 1, .type = GraphNodeType::Constant},
            {.id = 7,
             .type = GraphNodeType::Script,
             .scriptSignal = static_cast<uint8_t>(OfsSignalFunctional),
             .scriptInputCount = 1},
        };
        CHECK_FALSE(isNodeOutputFunctional(7, g, reg)); // unconnected
        g.links = {link(10, 1, 7, 0)};
        CHECK(isNodeOutputFunctional(7, g, reg));
    }
    SUBCASE("functional 2-input script also gates on its second input B") {
        ProcessingNodeGraph g;
        g.nodes = {
            {.id = 1, .type = GraphNodeType::Constant},
            {.id = 2, .type = GraphNodeType::Input},
            {.id = 7,
             .type = GraphNodeType::Script,
             .scriptSignal = static_cast<uint8_t>(OfsSignalFunctional),
             .scriptInputCount = 2},
        };
        g.links = {link(10, 1, 7, 0)}; // A functional, B unconnected → still functional
        CHECK(isNodeOutputFunctional(7, g, reg));

        g.links = {link(10, 1, 7, 0), link(11, 2, 7, 1)}; // B is the discrete Input → fails
        CHECK_FALSE(isNodeOutputFunctional(7, g, reg));
    }
}

TEST_CASE("isNodeOutputFunctional: an Output node carries no functional output of its own") {
    const EffectRegistryState reg = makeRegistry();
    ProcessingNodeGraph g;
    g.nodes = {{.id = 3, .type = GraphNodeType::Output}};
    // Output is a sink: it has no output pin to classify, so it falls through to the default.
    CHECK_FALSE(isNodeOutputFunctional(3, g, reg));
}

TEST_CASE("isLinkFunctional reflects the source node's output") {
    const EffectRegistryState reg = makeRegistry();
    ProcessingNodeGraph g;
    g.nodes = {
        {.id = 1, .type = GraphNodeType::Constant},
        {.id = 2, .type = GraphNodeType::Input},
        {.id = 3, .type = GraphNodeType::Output},
    };

    CHECK(isLinkFunctional(link(10, 1, 3), g, reg));       // from a constant
    CHECK_FALSE(isLinkFunctional(link(11, 2, 3), g, reg)); // from an Input
}
