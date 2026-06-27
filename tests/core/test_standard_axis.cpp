#include "Core/StandardAxis.h"
#include "UI/AxisColors.h"
#include "UI/Theme.h"
#include "imgui.h"
#include <array>
#include <doctest/doctest.h>
#include <string>

using ofs::StandardAxis;
using ofs::standardAxisFromTag;
using ofs::standardAxisTag;

namespace {
// All real axes in enum order, paired with their canonical short tag. Count is excluded.
constexpr std::array<std::pair<StandardAxis, std::string_view>, ofs::kStandardAxisCount> kAxes = {{
    {StandardAxis::L0, "L0"}, {StandardAxis::L1, "L1"}, {StandardAxis::L2, "L2"}, {StandardAxis::R0, "R0"},
    {StandardAxis::R1, "R1"}, {StandardAxis::R2, "R2"}, {StandardAxis::V0, "V0"}, {StandardAxis::V1, "V1"},
    {StandardAxis::A0, "A0"}, {StandardAxis::A1, "A1"}, {StandardAxis::S0, "S0"}, {StandardAxis::S1, "S1"},
    {StandardAxis::S2, "S2"}, {StandardAxis::S3, "S3"}, {StandardAxis::S4, "S4"}, {StandardAxis::S5, "S5"},
    {StandardAxis::S6, "S6"}, {StandardAxis::S7, "S7"}, {StandardAxis::S8, "S8"}, {StandardAxis::S9, "S9"},
}};

bool vec4Eq(const ImVec4 &a, const ImVec4 &b) {
    return a.x == doctest::Approx(b.x) && a.y == doctest::Approx(b.y) && a.z == doctest::Approx(b.z) &&
           a.w == doctest::Approx(b.w);
}
} // namespace

TEST_CASE("standardAxisTag returns the canonical short tag for every axis") {
    for (const auto &[axis, tag] : kAxes)
        CHECK(standardAxisTag(axis) == tag);
    CHECK(standardAxisTag(StandardAxis::Count).empty());
}

TEST_CASE("standardAxisFromTag round-trips every canonical tag") {
    for (const auto &[axis, tag] : kAxes) {
        auto parsed = standardAxisFromTag(tag);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == axis);
    }
}

TEST_CASE("standardAxisFromTag accepts case-insensitive short tags") {
    // Lowercase short tags miss the case-sensitive fast path and exercise the
    // lowercasing alias branch instead.
    CHECK(standardAxisFromTag("l0") == StandardAxis::L0);
    CHECK(standardAxisFromTag("r2") == StandardAxis::R2);
    CHECK(standardAxisFromTag("v1") == StandardAxis::V1);
    CHECK(standardAxisFromTag("a0") == StandardAxis::A0);
    for (int i = 0; i <= 9; ++i) {
        const std::string tag = "s" + std::to_string(i);
        auto parsed = standardAxisFromTag(tag);
        REQUIRE(parsed.has_value());
        CHECK(static_cast<int>(*parsed) == static_cast<int>(StandardAxis::S0) + i);
    }
}

TEST_CASE("standardAxisFromTag resolves the human-readable funscript aliases") {
    CHECK(standardAxisFromTag("stroke") == StandardAxis::L0);
    CHECK(standardAxisFromTag("surge") == StandardAxis::L1);
    CHECK(standardAxisFromTag("sway") == StandardAxis::L2);
    CHECK(standardAxisFromTag("twist") == StandardAxis::R0);
    CHECK(standardAxisFromTag("roll") == StandardAxis::R1);
    CHECK(standardAxisFromTag("pitch") == StandardAxis::R2);
    CHECK(standardAxisFromTag("vibe") == StandardAxis::V0);
    CHECK(standardAxisFromTag("vibrate") == StandardAxis::V0);
    CHECK(standardAxisFromTag("vibe2") == StandardAxis::V1);
    CHECK(standardAxisFromTag("vibrate2") == StandardAxis::V1);
    CHECK(standardAxisFromTag("air") == StandardAxis::A0);
    CHECK(standardAxisFromTag("air2") == StandardAxis::A1);
}

TEST_CASE("standardAxisFromTag aliases are case-insensitive") {
    CHECK(standardAxisFromTag("Stroke") == StandardAxis::L0);
    CHECK(standardAxisFromTag("TWIST") == StandardAxis::R0);
    CHECK(standardAxisFromTag("Vibrate2") == StandardAxis::V1);
}

TEST_CASE("standardAxisFromTag rejects unknown tags") {
    CHECK_FALSE(standardAxisFromTag("").has_value());
    CHECK_FALSE(standardAxisFromTag("nonsense").has_value());
    CHECK_FALSE(standardAxisFromTag("s10").has_value());
    CHECK_FALSE(standardAxisFromTag("L3").has_value());
}

TEST_CASE("standardAxisName gives a descriptive label per axis, empty for Count") {
    // Spot-check the human-readable forms for the named device axes.
    CHECK(ofs::standardAxisName(StandardAxis::L0) == "L0 (Stroke)");
    CHECK(ofs::standardAxisName(StandardAxis::R2) == "R2 (Pitch)");
    CHECK(ofs::standardAxisName(StandardAxis::V0) == "V0 (Vibe)");
    CHECK(ofs::standardAxisName(StandardAxis::A1) == "A1 (Air2)");
    // Every real axis is non-empty and is prefixed by its short tag; scratch axes are
    // exactly their short tag.
    for (const auto &[axis, tag] : kAxes) {
        CAPTURE(tag);
        const std::string_view name = ofs::standardAxisName(axis);
        REQUIRE_FALSE(name.empty());
        CHECK(name.substr(0, tag.size()) == tag);
    }
    CHECK(ofs::standardAxisName(StandardAxis::Count).empty());
}

TEST_CASE("standardAxisShortName stays in sync with standardAxisTag") {
    // The header switch and the .cpp tag table must agree for every axis, else
    // serialization (tag) and display (short name) would drift apart.
    for (const auto &[axis, tag] : kAxes) {
        CAPTURE(tag);
        CHECK(ofs::standardAxisShortName(axis) == tag);
        CHECK(ofs::standardAxisShortName(axis) == standardAxisTag(axis));
    }
    CHECK(ofs::standardAxisShortName(StandardAxis::Count).empty());
}

TEST_CASE("isScratchAxis is true only for S0..S9") {
    for (const auto &[axis, tag] : kAxes) {
        CAPTURE(tag);
        const bool expectScratch = static_cast<int>(axis) >= static_cast<int>(StandardAxis::S0);
        CHECK(ofs::isScratchAxis(axis) == expectScratch);
    }
    // Boundaries: the last device axis is not scratch, the first scratch axis is.
    CHECK_FALSE(ofs::isScratchAxis(StandardAxis::A1));
    CHECK(ofs::isScratchAxis(StandardAxis::S0));
    CHECK(ofs::isScratchAxis(StandardAxis::S9));
}

TEST_CASE("scratchIndex maps S0..S9 to 0..9") {
    for (int i = 0; i <= 9; ++i) {
        const auto axis = static_cast<StandardAxis>(static_cast<int>(StandardAxis::S0) + i);
        CHECK(ofs::scratchIndex(axis) == i);
    }
}

TEST_CASE("standard axis colors index the matching theme slots for every axis") {
    // standardAxisColor* do arithmetic into the AppCol_Axis* / AppCol_AxisDim* blocks
    // (Theme.h warns about this). Verify each axis lands on its own slot in the right
    // block -- catching an off-by-one or a base/dim swap. Both sides read the same
    // gCustomColors storage, so this needs no applied theme or ImGui frame.
    for (int i = 0; i < static_cast<int>(ofs::kStandardAxisCount); ++i) {
        const auto axis = static_cast<StandardAxis>(i);
        CAPTURE(i);
        const auto baseSlot = static_cast<AppCol>(AppCol_AxisL0 + i);
        const auto dimSlot = static_cast<AppCol>(AppCol_AxisDimL0 + i);
        CHECK(ofs::standardAxisColor(axis) == ofs::theme::GetColorU32(baseSlot));
        CHECK(ofs::standardAxisColorDim(axis) == ofs::theme::GetColorU32(dimSlot));
        CHECK(vec4Eq(ofs::standardAxisColorVec4(axis), ofs::theme::GetStyleColorVec4(baseSlot)));
        CHECK(vec4Eq(ofs::standardAxisColorDimVec4(axis), ofs::theme::GetStyleColorVec4(dimSlot)));
    }
}

TEST_CASE("standard axis colors fall back to neutral gray for out-of-range axes") {
    // StandardAxis::Count (== kStandardAxisCount) is out of range and must hit the
    // documented neutral fallbacks, which never touch the theme tables.
    CHECK(ofs::standardAxisColor(StandardAxis::Count) == IM_COL32(128, 128, 128, 255));
    CHECK(ofs::standardAxisColorDim(StandardAxis::Count) == IM_COL32(70, 70, 70, 255));
    CHECK(vec4Eq(ofs::standardAxisColorVec4(StandardAxis::Count), ImVec4(0.5f, 0.5f, 0.5f, 1.0f)));
    CHECK(vec4Eq(ofs::standardAxisColorDimVec4(StandardAxis::Count), ImVec4(0.27f, 0.27f, 0.27f, 1.0f)));
}
