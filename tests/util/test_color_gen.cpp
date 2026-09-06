#include "Util/ColorGen.h"
#include <doctest/doctest.h>
#include <vector>

namespace {
struct Item {
    ImU32 color = 0;
};
ImU32 colorOf(const Item &i) {
    return i.color;
}
} // namespace

TEST_CASE("nextDistinctColor hands out the sequence while nothing is deleted") {
    constexpr size_t kSeed = 1234;
    std::vector<Item> items;
    for (size_t i = 0; i < 5; ++i) {
        items.push_back({ofs::util::nextDistinctColor(kSeed, items, colorOf)});
        CHECK(items.back().color == ofs::util::goldenRatioColor(kSeed + i));
    }
}

TEST_CASE("nextDistinctColor keeps a new item distinct after a middle one is deleted") {
    constexpr size_t kSeed = 7;
    std::vector<Item> items;
    for (int i = 0; i < 3; ++i)
        items.push_back({ofs::util::nextDistinctColor(kSeed, items, colorOf)});
    items.erase(items.begin() + 1); // the 2nd of 3, freeing its color while the 3rd keeps index 2

    const ImU32 fresh = ofs::util::nextDistinctColor(kSeed, items, colorOf);
    // Deriving the index from the count would re-pick index 2 here and duplicate the surviving 3rd item.
    CHECK(fresh != items[0].color);
    CHECK(fresh != items[1].color);
    CHECK(fresh == ofs::util::goldenRatioColor(kSeed + 1)); // reuses the freed slot
}

TEST_CASE("nextDistinctColor skips a user-chosen color that collides with the sequence") {
    constexpr size_t kSeed = 42;
    std::vector<Item> items;
    // A hand-picked color that happens to equal the generator's first entry must not be handed out twice.
    items.push_back({ofs::util::goldenRatioColor(kSeed + 0)});
    CHECK(ofs::util::nextDistinctColor(kSeed, items, colorOf) == ofs::util::goldenRatioColor(kSeed + 1));
}

TEST_CASE("nextDistinctColor ignores unrelated colors and starts at the sequence head") {
    constexpr size_t kSeed = 99;
    std::vector<Item> items{{IM_COL32(70, 130, 180, 220)}, {IM_COL32(1, 2, 3, 4)}};
    CHECK(ofs::util::nextDistinctColor(kSeed, items, colorOf) == ofs::util::goldenRatioColor(kSeed + 0));
}
