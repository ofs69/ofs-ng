#include "Core/ScriptAxisAction.h"
#include "Core/VectorSet.h"
#include <doctest/doctest.h>

using ofs::ScriptAxisAction;
using ofs::VectorSet;

TEST_CASE("VectorSet insert maintains sorted order") {
    VectorSet<ScriptAxisAction> vs;
    vs.insert({3.0, 30});
    vs.insert({1.0, 10});
    vs.insert({2.0, 20});

    REQUIRE(vs.size() == 3);
    CHECK(vs[0].at == doctest::Approx(1.0));
    CHECK(vs[1].at == doctest::Approx(2.0));
    CHECK(vs[2].at == doctest::Approx(3.0));
}

TEST_CASE("VectorSet insert duplicate returns false and does not grow") {
    VectorSet<ScriptAxisAction> vs;
    auto [it1, inserted1] = vs.insert({1.0, 50});
    CHECK(inserted1 == true);

    auto [it2, inserted2] = vs.insert({1.0, 50});
    CHECK(inserted2 == false);
    CHECK(vs.size() == 1);
}

TEST_CASE("VectorSet appendSorted fast-appends strictly increasing elements") {
    VectorSet<ScriptAxisAction> vs;
    vs.reserve(4);
    for (double t : {1.0, 2.0, 3.0, 4.0}) {
        auto [it, inserted] = vs.appendSorted({t, 0});
        CHECK(inserted == true);
    }
    REQUIRE(vs.size() == 4);
    for (size_t i = 0; i < vs.size(); ++i)
        CHECK(vs[i].at == doctest::Approx(static_cast<double>(i) + 1.0));
}

TEST_CASE("VectorSet appendSorted rejects a duplicate of the back") {
    VectorSet<ScriptAxisAction> vs;
    vs.appendSorted({1.0, 10});
    auto [it, inserted] = vs.appendSorted({1.0, 10}); // equal to back ⇒ not strictly greater
    CHECK(inserted == false);
    CHECK(vs.size() == 1);
}

TEST_CASE("VectorSet appendSorted self-corrects out-of-order input") {
    // The codec relies on appendSorted preserving the sorted-unique invariant even if the byte stream is
    // somehow not ascending — it must fall back to a positioned insert, never leave the set unsorted.
    VectorSet<ScriptAxisAction> vs;
    vs.appendSorted({1.0, 10});
    vs.appendSorted({5.0, 50});
    vs.appendSorted({3.0, 30}); // lands before the back, not after
    vs.appendSorted({5.0, 99}); // duplicate time of an interior/back element ⇒ dropped

    REQUIRE(vs.size() == 3);
    CHECK(vs[0].at == doctest::Approx(1.0));
    CHECK(vs[1].at == doctest::Approx(3.0));
    CHECK(vs[2].at == doctest::Approx(5.0));
    CHECK(vs[2].pos == 50); // first {5.0,…} won; the later duplicate-time append was rejected
}

TEST_CASE("VectorSet erase by value removes the correct entry") {
    VectorSet<ScriptAxisAction> vs;
    vs.insert({1.0, 10});
    vs.insert({2.0, 20});
    vs.insert({3.0, 30});

    const size_t removed = vs.erase(ScriptAxisAction{2.0, 20});
    CHECK(removed == 1);
    REQUIRE(vs.size() == 2);
    CHECK(vs[0].at == doctest::Approx(1.0));
    CHECK(vs[1].at == doctest::Approx(3.0));
}

TEST_CASE("VectorSet erase non-existent value returns 0") {
    VectorSet<ScriptAxisAction> vs;
    vs.insert({1.0, 10});
    CHECK(vs.erase(ScriptAxisAction{99.0, 0}) == 0);
    CHECK(vs.size() == 1);
}

TEST_CASE("VectorSet insertRange deduplicates and preserves order") {
    VectorSet<ScriptAxisAction> vs;
    vs.insert({2.0, 20});

    std::vector<ScriptAxisAction> batch = {{3.0, 30}, {1.0, 10}, {2.0, 20}};
    vs.insertRange(batch.begin(), batch.end());

    REQUIRE(vs.size() == 3);
    CHECK(vs[0].at == doctest::Approx(1.0));
    CHECK(vs[1].at == doctest::Approx(2.0));
    CHECK(vs[2].at == doctest::Approx(3.0));
}

TEST_CASE("VectorSet contains and find work after mutations") {
    VectorSet<ScriptAxisAction> vs;
    vs.insert({1.0, 10});
    vs.insert({2.0, 20});

    CHECK(vs.contains({1.0, 10}));
    CHECK_FALSE(vs.contains({5.0, 50}));

    vs.erase({1.0, 10});
    CHECK_FALSE(vs.contains({1.0, 10}));
    CHECK(vs.contains({2.0, 20}));
}

TEST_CASE("VectorSet range-constructor produces sorted unique set") {
    std::vector<ScriptAxisAction> src = {{3.0, 30}, {1.0, 10}, {2.0, 20}, {2.0, 20}};
    VectorSet<ScriptAxisAction> vs(src.begin(), src.end());

    REQUIRE(vs.size() == 3);
    CHECK(vs[0].at == doctest::Approx(1.0));
    CHECK(vs[1].at == doctest::Approx(2.0));
    CHECK(vs[2].at == doctest::Approx(3.0));
}

TEST_CASE("VectorSet eraseRange removes the inclusive [lo,hi] span only") {
    VectorSet<ScriptAxisAction> vs;
    for (double t : {0.0, 1.0, 1.5, 2.0, 3.0})
        vs.insert({t, 10});

    // Bounds need not be present elements; pos is ignored by the comparator (compares `at`).
    vs.eraseRange({1.0, 0}, {2.0, 0});
    REQUIRE(vs.size() == 2);
    CHECK(vs.contains({0.0, 10}));
    CHECK(vs.contains({3.0, 10}));
    CHECK_FALSE(vs.contains({1.5, 10}));

    // A degenerate (lo == hi) range erases at most the single element at that time.
    vs.eraseRange({3.0, 0}, {3.0, 0});
    CHECK(vs.size() == 1);
    CHECK(vs.contains({0.0, 10}));

    // lo > hi is a no-op.
    vs.eraseRange({10.0, 0}, {0.0, 0});
    CHECK(vs.size() == 1);
}
