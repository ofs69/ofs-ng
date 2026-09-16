#include <doctest/doctest.h>

#include "Core/SpeedLimit.h"

namespace {

// Strokes: 0→1s 0→100 (100 u/s), 1→1.1s 100→0 (1000 u/s), 1.1→2s 0→50 (~56 u/s), 2→2.1s 50→100 (500 u/s).
ofs::VectorSet<ofs::ScriptAxisAction> makeActions() {
    ofs::VectorSet<ofs::ScriptAxisAction> actions;
    for (const auto &a : {ofs::ScriptAxisAction{0.0, 0}, ofs::ScriptAxisAction{1.0, 100}, ofs::ScriptAxisAction{1.1, 0},
                          ofs::ScriptAxisAction{2.0, 50}, ofs::ScriptAxisAction{2.1, 100}})
        actions.insert(a);
    return actions;
}

} // namespace

TEST_CASE("A stroke exceeds the speed limit only when strictly faster") {
    CHECK(ofs::exceedsSpeedLimit({0.0, 0}, {0.5, 100}, 100.0f));
    CHECK_FALSE(ofs::exceedsSpeedLimit({0.0, 0}, {1.0, 100}, 100.0f));
    CHECK(ofs::exceedsSpeedLimit({0.0, 100}, {0.5, 0}, 100.0f)); // direction doesn't matter
    CHECK_FALSE(ofs::exceedsSpeedLimit({0.0, 20}, {0.01, 20}, 100.0f));
}

TEST_CASE("A zero-duration stroke is never over the limit") {
    CHECK_FALSE(ofs::exceedsSpeedLimit({1.0, 0}, {1.0, 100}, 1.0f));
}

TEST_CASE("Counting over-limit strokes") {
    const auto actions = makeActions();
    CHECK(ofs::countOverSpeedLimit(actions, 400.0f) == 2);
    CHECK(ofs::countOverSpeedLimit(actions, 600.0f) == 1);
    CHECK(ofs::countOverSpeedLimit(actions, 5000.0f) == 0);
    CHECK(ofs::countOverSpeedLimit(ofs::VectorSet<ofs::ScriptAxisAction>{}, 1.0f) == 0);
}

TEST_CASE("Stepping forward to the next over-limit stroke") {
    const auto actions = makeActions();
    auto next = ofs::findOverSpeedLimitStroke(actions, 0.0, true, 400.0f);
    REQUIRE(next.has_value());
    CHECK(*next == doctest::Approx(1.0));

    // Parked on a flagged stroke's start: step past it, not onto it again.
    next = ofs::findOverSpeedLimitStroke(actions, 1.0, true, 400.0f);
    REQUIRE(next.has_value());
    CHECK(*next == doctest::Approx(2.0));

    CHECK_FALSE(ofs::findOverSpeedLimitStroke(actions, 2.0, true, 400.0f).has_value());
}

TEST_CASE("Stepping backward to the previous over-limit stroke") {
    const auto actions = makeActions();
    auto prev = ofs::findOverSpeedLimitStroke(actions, 5.0, false, 400.0f);
    REQUIRE(prev.has_value());
    CHECK(*prev == doctest::Approx(2.0));

    // Mid-stroke: the stroke the playhead is inside started before it, so it is the previous one.
    prev = ofs::findOverSpeedLimitStroke(actions, 2.05, false, 400.0f);
    REQUIRE(prev.has_value());
    CHECK(*prev == doctest::Approx(2.0));

    prev = ofs::findOverSpeedLimitStroke(actions, 2.0, false, 400.0f);
    REQUIRE(prev.has_value());
    CHECK(*prev == doctest::Approx(1.0));

    CHECK_FALSE(ofs::findOverSpeedLimitStroke(actions, 1.0, false, 400.0f).has_value());
}
