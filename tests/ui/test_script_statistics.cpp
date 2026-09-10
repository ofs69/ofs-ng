#include <doctest/doctest.h>

#include "UI/ScriptStatistics.h"

// The Statistics window reports one stroke around the playhead. The interesting case is the caret
// parked exactly on an action (a step-to-action landing): the stroke the user just watched is the one
// that *ended* there, not the one about to leave it, so an on-point action reads as the stroke's end.

namespace {

ofs::VectorSet<ofs::ScriptAxisAction> makeActions() {
    ofs::VectorSet<ofs::ScriptAxisAction> actions;
    for (const auto &a :
         {ofs::ScriptAxisAction{1.0, 0}, ofs::ScriptAxisAction{2.0, 100}, ofs::ScriptAxisAction{3.0, 20}})
        actions.insert(a);
    return actions;
}

} // namespace

TEST_CASE("Playhead on an action reports the stroke that ended there") {
    const auto actions = makeActions();

    const auto onMiddle = ofs::ui::strokeAtPlayhead(actions, 2.0);
    REQUIRE(onMiddle.from != nullptr);
    REQUIRE(onMiddle.to != nullptr);
    CHECK(onMiddle.from->at == doctest::Approx(1.0));
    CHECK(onMiddle.to->at == doctest::Approx(2.0));

    // The last action bounds a stroke too — parking on it used to leave the window blank.
    const auto onLast = ofs::ui::strokeAtPlayhead(actions, 3.0);
    REQUIRE(onLast.to != nullptr);
    CHECK(onLast.from->at == doctest::Approx(2.0));
    CHECK(onLast.to->at == doctest::Approx(3.0));

    // Within the on-point slop, from either side, still the ended stroke.
    for (const double t : {2.0 - ofs::ui::kOnActionEpsilon / 2.0, 2.0 + ofs::ui::kOnActionEpsilon / 2.0}) {
        const auto near = ofs::ui::strokeAtPlayhead(actions, t);
        REQUIRE(near.to != nullptr);
        CHECK(near.to->at == doctest::Approx(2.0));
    }
}

TEST_CASE("Playhead between actions reports the stroke it sits in") {
    const auto actions = makeActions();

    const auto inside = ofs::ui::strokeAtPlayhead(actions, 2.5);
    REQUIRE(inside.from != nullptr);
    CHECK(inside.from->at == doctest::Approx(2.0));
    CHECK(inside.to->at == doctest::Approx(3.0));

    // Only the first action is at or before the playhead, so no stroke is bounded yet.
    const auto beforeFirst = ofs::ui::strokeAtPlayhead(actions, 0.5);
    CHECK(beforeFirst.from == nullptr);
    CHECK(beforeFirst.to == nullptr);

    const auto onFirst = ofs::ui::strokeAtPlayhead(actions, 1.0);
    CHECK(onFirst.from == nullptr);
    CHECK(onFirst.to == nullptr);

    // Past the last action there is no stroke around the playhead either.
    const auto afterLast = ofs::ui::strokeAtPlayhead(actions, 4.0);
    CHECK(afterLast.to == nullptr);
}

TEST_CASE("Interval keeps measuring from the last action at or before the playhead") {
    const auto actions = makeActions();

    // On-point the interval is zero — the stroke shown moved, this did not.
    const auto onMiddle = ofs::ui::strokeAtPlayhead(actions, 2.0);
    REQUIRE(onMiddle.prev != nullptr);
    CHECK(onMiddle.prev->at == doctest::Approx(2.0));

    const auto inside = ofs::ui::strokeAtPlayhead(actions, 2.5);
    REQUIRE(inside.prev != nullptr);
    CHECK(inside.prev->at == doctest::Approx(2.0));

    const auto beforeFirst = ofs::ui::strokeAtPlayhead(actions, 0.5);
    CHECK(beforeFirst.prev == nullptr);

    const auto afterLast = ofs::ui::strokeAtPlayhead(actions, 4.0);
    REQUIRE(afterLast.prev != nullptr);
    CHECK(afterLast.prev->at == doctest::Approx(3.0));
}
