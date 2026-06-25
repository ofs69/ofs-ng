#include "Core/ProcessingRegion.h"
#include <doctest/doctest.h>
#include <unordered_map>

using ofs::GraphId;

// The tagged GraphId layout must keep node bodies, every pin slot, static attrs, and links in one
// collision-free id space. Mint the full id set for nodes 1..256, all pin slots, and links, and assert
// no two distinct (owner, tag, slot) tuples land on the same encoded id.
TEST_CASE("GraphId encoding has no collisions across bodies, pins, static, and links") {
    constexpr int kMaxOwner = 256;
    std::unordered_map<int, std::string> seen;
    auto claim = [&](int id, const std::string &who) {
        auto [it, inserted] = seen.try_emplace(id, who);
        REQUIRE_MESSAGE(inserted, "Collision at id=" << id << " between " << who << " and " << it->second);
    };

    for (int n = 1; n <= kMaxOwner; ++n) {
        claim(GraphId::nodeBody(n), "body:" + std::to_string(n));
        claim(GraphId::staticAttr(n), "static:" + std::to_string(n));
        claim(GraphId::link(n), "link:" + std::to_string(n));
        for (int s = 0; s <= GraphId::kMaxSlot; ++s) {
            claim(GraphId::outPin(n, s), "out:" + std::to_string(n) + "." + std::to_string(s));
            claim(GraphId::inPin(n, s), "in:" + std::to_string(n) + "." + std::to_string(s));
        }
    }
}

TEST_CASE("GraphId round-trips owner/tag/slot through decode") {
    for (int n = 1; n <= 64; ++n) {
        // Output pins (slot 0 is the canonical single output).
        {
            const auto d = GraphId::decode(GraphId::outPin(n));
            CHECK(d.owner == n);
            CHECK(d.tag == GraphId::Tag::OutPin);
            CHECK(d.slot == 0);
        }
        // Input slots across the full per-direction range.
        for (int s = 0; s <= GraphId::kMaxSlot; ++s) {
            const auto d = GraphId::decode(GraphId::inPin(n, s));
            CHECK(d.owner == n);
            CHECK(d.tag == GraphId::Tag::InPin);
            CHECK(d.slot == s);
        }
        // Node body and link carry no slot.
        CHECK(GraphId::decode(GraphId::nodeBody(n)).tag == GraphId::Tag::NodeBody);
        CHECK(GraphId::decode(GraphId::nodeBody(n)).owner == n);
        CHECK(GraphId::decode(GraphId::link(n)).tag == GraphId::Tag::Link);
        CHECK(GraphId::decode(GraphId::link(n)).owner == n);
    }
}

TEST_CASE("buildDefaultGraph produces one Input and one Output node per axis") {
    ofs::AxisRoles roles;
    roles.set(static_cast<size_t>(ofs::StandardAxis::L0));
    roles.set(static_cast<size_t>(ofs::StandardAxis::R0));

    const auto g = ofs::buildDefaultGraph(roles);

    // 2 axes → 2 Input + 2 Output nodes + 2 links
    CHECK(g.nodes.size() == 4);
    CHECK(g.links.size() == 2);

    int inputCount = 0;
    int outputCount = 0;
    for (const auto &node : g.nodes) {
        if (node.type == ofs::GraphNodeType::Input)
            ++inputCount;
        if (node.type == ofs::GraphNodeType::Output)
            ++outputCount;
    }
    CHECK(inputCount == 2);
    CHECK(outputCount == 2);
}
