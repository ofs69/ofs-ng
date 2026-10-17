#include <doctest/doctest.h>

#include "Platform/TitleBarHitTest.h"

using ofs::classifyTitleBarHit;
using ofs::TitleBarHitInfo;

namespace {
constexpr int kW = 800;
constexpr int kH = 600;

TitleBarHitInfo layout() {
    return TitleBarHitInfo{
        .height = 32.0f, .buttonsLeftX = 700.0f, .searchLeftX = 300.0f, .searchRightX = 500.0f, .enabled = true};
}

SDL_HitTestResult hit(SDL_WindowFlags flags, int x, int y, const TitleBarHitInfo &info = layout()) {
    return classifyTitleBarHit(info, flags, kW, kH, SDL_Point{.x = x, .y = y});
}
} // namespace

TEST_CASE("title bar hit test: normal window") {
    CHECK(hit(0, 100, 16) == SDL_HITTEST_DRAGGABLE);
    CHECK(hit(0, 400, 16) == SDL_HITTEST_NORMAL); // search box carve-out
    CHECK(hit(0, 750, 16) == SDL_HITTEST_NORMAL); // caption buttons
    CHECK(hit(0, 400, 300) == SDL_HITTEST_NORMAL);

    CHECK(hit(0, 0, 0) == SDL_HITTEST_RESIZE_TOPLEFT);
    CHECK(hit(0, kW - 1, 0) == SDL_HITTEST_RESIZE_TOPRIGHT);
    CHECK(hit(0, 0, kH - 1) == SDL_HITTEST_RESIZE_BOTTOMLEFT);
    CHECK(hit(0, kW - 1, kH - 1) == SDL_HITTEST_RESIZE_BOTTOMRIGHT);
    CHECK(hit(0, 0, 300) == SDL_HITTEST_RESIZE_LEFT);
    CHECK(hit(0, kW - 1, 300) == SDL_HITTEST_RESIZE_RIGHT);
    CHECK(hit(0, 100, 0) == SDL_HITTEST_RESIZE_TOP);
    CHECK(hit(0, 400, kH - 1) == SDL_HITTEST_RESIZE_BOTTOM);
}

TEST_CASE("title bar hit test: maximized window drags but does not resize") {
    CHECK(hit(SDL_WINDOW_MAXIMIZED, 100, 16) == SDL_HITTEST_DRAGGABLE);
    CHECK(hit(SDL_WINDOW_MAXIMIZED, 100, 0) == SDL_HITTEST_DRAGGABLE);
    CHECK(hit(SDL_WINDOW_MAXIMIZED, 0, 300) == SDL_HITTEST_NORMAL);
    CHECK(hit(SDL_WINDOW_MAXIMIZED, kW - 1, kH - 1) == SDL_HITTEST_NORMAL);
}

TEST_CASE("title bar hit test: fullscreen window neither moves nor resizes") {
    CHECK(hit(SDL_WINDOW_FULLSCREEN, 100, 16) == SDL_HITTEST_NORMAL);
    CHECK(hit(SDL_WINDOW_FULLSCREEN, 0, 0) == SDL_HITTEST_NORMAL);
    CHECK(hit(SDL_WINDOW_FULLSCREEN, kW - 1, 300) == SDL_HITTEST_NORMAL);
    CHECK(hit(SDL_WINDOW_FULLSCREEN, 400, kH - 1) == SDL_HITTEST_NORMAL);
}

TEST_CASE("title bar hit test: inactive custom title bar is all client area") {
    TitleBarHitInfo info = layout();
    info.enabled = false;
    CHECK(hit(0, 100, 16, info) == SDL_HITTEST_NORMAL);
    CHECK(hit(0, 0, 0, info) == SDL_HITTEST_NORMAL);
}
