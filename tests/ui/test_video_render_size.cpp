#include <doctest/doctest.h>

#include "UI/VideoRenderSize.h"

// Regression for "video is soft under fractional display scaling": the mpv render target is allocated
// in framebuffer pixels, but its size was computed from the ImGui quad's display units. Those are equal
// only at FramebufferScale 1 — true on Windows and X11, false on Wayland at 150% and on Retina — so the
// target held 1/scale of the pixels it was stretched across. The user could not compensate either:
// resolutionScale is capped at 1.0, which made "Full" resolution a 67% render at 150% scaling.

TEST_CASE("The render target matches the pixels the image actually covers") {
    constexpr int kSourceW = 3840; // a source with detail to spare, so the cap never binds here
    constexpr int kSourceH = 2160;
    const ImVec2 image{960.f, 540.f}; // display units on screen

    SUBCASE("at 1x a display unit is a pixel") {
        const auto r = ofs::ui::videoRenderPixels(image, 1.f, {1.f, 1.f}, kSourceW, kSourceH);
        CHECK(r.w == 960);
        CHECK(r.h == 540);
    }

    SUBCASE("a scaled framebuffer gets the pixels it will magnify") {
        const auto r = ofs::ui::videoRenderPixels(image, 1.f, {1.5f, 1.5f}, kSourceW, kSourceH);
        CHECK(r.w == 1440); // 960 display units * 1.5 physical pixels each
        CHECK(r.h == 810);

        const auto retina = ofs::ui::videoRenderPixels(image, 1.f, {2.f, 2.f}, kSourceW, kSourceH);
        CHECK(retina.w == 1920);
        CHECK(retina.h == 1080);
    }

    SUBCASE("the user's quality reduction still applies on top") {
        const auto r = ofs::ui::videoRenderPixels(image, 0.5f, {1.5f, 1.5f}, kSourceW, kSourceH);
        CHECK(r.w == 720); // half of the full 1440
        CHECK(r.h == 405);
    }
}

TEST_CASE("The render target never exceeds the source resolution") {
    // Zoomed past 1:1 on a modest source: the extra texels would carry no picture.
    const auto r = ofs::ui::videoRenderPixels({1600.f, 900.f}, 1.f, {2.f, 2.f}, 1280, 720);
    CHECK(r.w == 1280);
    CHECK(r.h == 720);
}

TEST_CASE("A degenerate size still yields a usable target") {
    // A collapsed panel (or audio-only media, whose source dimensions are 0) must not request a
    // zero-sized texture.
    const auto collapsed = ofs::ui::videoRenderPixels({0.f, 0.f}, 1.f, {1.5f, 1.5f}, 1920, 1080);
    CHECK(collapsed.w == 1);
    CHECK(collapsed.h == 1);

    const auto audioOnly = ofs::ui::videoRenderPixels({960.f, 540.f}, 1.f, {1.f, 1.f}, 0, 0);
    CHECK(audioOnly.w == 1);
    CHECK(audioOnly.h == 1);
}
