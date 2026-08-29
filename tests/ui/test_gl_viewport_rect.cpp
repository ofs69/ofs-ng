#include <doctest/doctest.h>

#include "UI/GlViewportRect.h"
#include <cmath>

// Regression for the "3D simulator model drifts away from its overlay" bug: ScriptSimulator's draw
// callback issues raw GL, so it computes its own glViewport/glScissor from the overlay's screen rect.
// It did so in ImGui display units, which are framebuffer pixels only when FramebufferScale is 1 —
// true on Windows and X11, false on Wayland under fractional scaling (a 150% desktop reports a logical
// window size and a 1.5× pixel size). The model then rendered at 1/scale of its intended offset from
// the framebuffer's bottom-left corner, so it lagged further behind the ImGui-drawn overlay the further
// the overlay was dragged from that corner — and the equally unscaled scissor let it escape its window
// onto the timeline instead of clipping it.

namespace {

ImDrawData makeDrawData(const ImVec2 &displayPos, const ImVec2 &displaySize, float fbScale) {
    ImDrawData dd;
    dd.DisplayPos = displayPos;
    dd.DisplaySize = displaySize;
    dd.FramebufferScale = {fbScale, fbScale};
    return dd;
}

} // namespace

TEST_CASE("A screen rect maps to the same relative position in the framebuffer at any scale") {
    const ImVec2 displaySize{1600.f, 900.f};
    const ImVec2 rectMin{1200.f, 100.f}; // near the top-right, where the old bug was worst
    const ImVec2 rectSize{200.f, 150.f};

    const ofs::ui::GlRect at1x = ofs::ui::glRectFromScreen(rectMin, rectSize, makeDrawData({}, displaySize, 1.f));

    // At 1× it is the y-flip alone: GL's origin is bottom-left, ImGui's top-left.
    CHECK(at1x.x == 1200);
    CHECK(at1x.y == 650); // 900 - 100 - 150
    CHECK(at1x.w == 200);
    CHECK(at1x.h == 150);

    // Under fractional scaling the framebuffer is `scale` times larger and the rect must scale with
    // it, so it still covers the same fraction of the window. A fractional scale lands between pixels,
    // hence the pixel of slack for the truncation.
    for (const float scale : {1.25f, 1.5f, 2.f}) {
        CAPTURE(scale);
        const ofs::ui::GlRect r = ofs::ui::glRectFromScreen(rectMin, rectSize, makeDrawData({}, displaySize, scale));
        CHECK(std::abs(static_cast<float>(r.x) - static_cast<float>(at1x.x) * scale) <= 1.f);
        CHECK(std::abs(static_cast<float>(r.y) - static_cast<float>(at1x.y) * scale) <= 1.f);
        CHECK(std::abs(static_cast<float>(r.w) - static_cast<float>(at1x.w) * scale) <= 1.f);
        CHECK(std::abs(static_cast<float>(r.h) - static_cast<float>(at1x.h) * scale) <= 1.f);
    }
}

TEST_CASE("The displacement from skipping the framebuffer scale grows with distance from the origin") {
    const ImVec2 displaySize{1600.f, 900.f};
    const ImVec2 size{200.f, 150.f};
    const ImDrawData dd = makeDrawData({}, displaySize, 1.5f);

    // Bottom-left of the window: the unscaled rect was nearly right, which is why the reporter saw the
    // model sitting close to its overlay there.
    const ofs::ui::GlRect bottomLeft = ofs::ui::glRectFromScreen({0.f, displaySize.y - size.y}, size, dd);
    CHECK(bottomLeft.x == 0);
    CHECK(bottomLeft.y == 0);

    // Top-right: correct is 1.5× the display-unit offset, so the old code placed it a third of the way
    // back toward the bottom-left corner — hundreds of pixels adrift, over the middle of the video.
    const ImVec2 topRight{displaySize.x - size.x, 0.f};
    const ofs::ui::GlRect r = ofs::ui::glRectFromScreen(topRight, size, dd);
    CHECK(r.x == 2100); // 1400 * 1.5
    CHECK(r.y == 1125); // (900 - 150) * 1.5
}

TEST_CASE("Framebuffer rects are taken relative to the owning viewport") {
    // A rect on a secondary viewport is offset by DisplayPos, whose own origin is the framebuffer's.
    const ImDrawData dd = makeDrawData({500.f, 300.f}, {800.f, 600.f}, 2.f);
    const ofs::ui::GlRect r = ofs::ui::glRectFromScreen({600.f, 400.f}, {100.f, 50.f}, dd);
    CHECK(r.x == 200); // (600 - 500) * 2
    CHECK(r.y == 900); // (600 - (400 - 300) - 50) * 2
    CHECK(r.w == 200);
    CHECK(r.h == 100);
}

TEST_CASE("A degenerate clip rect yields no negative extent") {
    // User callbacks are dispatched before the backend's empty-clip-rect skip, so a fully clipped
    // window reaches the callback with an inverted rect; a negative extent is GL_INVALID_VALUE.
    const ImDrawData dd = makeDrawData({}, {1600.f, 900.f}, 1.5f);
    const ofs::ui::GlRect r = ofs::ui::glRectFromScreen({400.f, 200.f}, {-30.f, -10.f}, dd);
    CHECK(r.w == 0);
    CHECK(r.h == 0);
}
