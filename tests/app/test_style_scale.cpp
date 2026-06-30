#include <doctest/doctest.h>

#include "App/StyleScale.h"
#include <imgui.h>
#include <imgui_internal.h>

// Regression for the theme-tab "value explodes at >100% display scaling" bug: every theme::apply() runs
// a post-apply hook that captures the just-applied style as the 1× DPI base and re-scales it. apply()
// rewrites only Colors[] + the ImGuiStyleVar_* fields, leaving the fields ScaleAllSizes() *also* touches
// but which are not style-vars (notably ImGuiStyle::_MainScale) carrying the prior frame's scale.
// Capturing the live style verbatim therefore re-scaled those each apply and compounded them — at any
// content scale > 1×, (int)_MainScale climbs by one per apply, thickening the FrameBorderSize==0
// color-button border and caret. ofs::style::unscaledThemeBase() rebuilds the base from a pristine 1×
// style so they stay put. At exactly 1× the bug is invisible (multiplier 1.0), matching the report.

namespace {

// Faithful model of theme::apply()'s ImGui-style write (src/UI/Theme.cpp::apply steps 1-2): it copies
// the theme's Colors[] and the ImGuiStyleVar_* fields into the live style, and touches nothing else.
void applyThemeStyleVarsOnly(ImGuiStyle &live, const ImGuiStyle &themed1x) {
    for (int i = 0; i < ImGuiCol_COUNT; ++i)
        live.Colors[i] = themed1x.Colors[i];
    for (int i = 0; i < ImGuiStyleVar_COUNT; ++i) {
        const ImGuiStyleVarInfo *info = ImGui::GetStyleVarInfo(i);
        auto *dst = static_cast<float *>(info->GetVarPtr(&live));
        const auto *src = static_cast<const float *>(info->GetVarPtr(const_cast<ImGuiStyle *>(&themed1x)));
        dst[0] = src[0];
        if (info->Count == 2)
            dst[1] = src[1];
    }
}

} // namespace

TEST_CASE("Repeated theme apply at a >1 content scale does not compound ScaleAllSizes-only fields") {
    constexpr float kScale = 1.5f; // any DPI > 100%

    const ImGuiStyle pristine; // the app's 1× baseline, captured before any ScaleAllSizes / theme apply

    // A representative theme: a FrameBorderSize of 0 (the shipped M3 default) is what routes the
    // color-button border onto the _MainScale FIXME-DPI path in the first place.
    ImGuiStyle themed1x = pristine;
    themed1x.FrameBorderSize = 0.f;
    themed1x.FrameRounding = 4.f;
    themed1x.WindowPadding = ImVec2(8.f, 8.f);

    // Startup: theme applied once, base captured, scaled to the monitor's content scale.
    ImGuiStyle live = pristine;
    applyThemeStyleVarsOnly(live, themed1x);
    ImGuiStyle base = ofs::style::unscaledThemeBase(live, pristine);
    live = base;
    live.ScaleAllSizes(kScale);

    const float mainScaleAfterStartup = live._MainScale;
    const float frameRoundingAfterStartup = live.FrameRounding;
    CHECK(mainScaleAfterStartup == doctest::Approx(kScale));

    // Now drive a stream of theme edits — each one re-applies the theme and re-runs the post-apply hook,
    // exactly as dragging a swatch in the Theme tab does frame after frame. Every field of the scaled
    // style must stay byte-identical to its post-startup value: the DPI base never folds in the live
    // scale, so nothing drifts.
    for (int edit = 0; edit < 12; ++edit) {
        applyThemeStyleVarsOnly(live, themed1x);
        base = ofs::style::unscaledThemeBase(live, pristine);
        live = base;
        live.ScaleAllSizes(kScale);

        CHECK(live._MainScale == doctest::Approx(mainScaleAfterStartup));
        CHECK(live.FrameRounding == doctest::Approx(frameRoundingAfterStartup));
    }

    // The concrete symptom: the integer border width the color button derives from _MainScale is stable.
    CHECK(static_cast<int>(live._MainScale) == static_cast<int>(mainScaleAfterStartup));
}

TEST_CASE("unscaledThemeBase resets _MainScale to the pristine 1× value and keeps themed vars") {
    ImGuiStyle pristine; // _MainScale == 1
    REQUIRE(pristine._MainScale == doctest::Approx(1.f));

    // Stand in for a live style that has already been DPI-scaled twice (the compounding the bug caused).
    ImGuiStyle applied = pristine;
    applied.ScaleAllSizes(1.5f);
    applied.ScaleAllSizes(1.5f);
    REQUIRE(applied._MainScale == doctest::Approx(2.25f));
    applied.FrameRounding = 4.f; // a themed style-var the base must carry through

    const ImGuiStyle base = ofs::style::unscaledThemeBase(applied, pristine);
    CHECK(base._MainScale == doctest::Approx(1.f));    // non-var scale field reset to pristine
    CHECK(base.FrameRounding == doctest::Approx(4.f)); // themed style-var preserved from applied
}
