#include <doctest/doctest.h>

#include "Format/AppSettings.h" // kDefaultFontSizeBase — the size the ≈8 px figures are quoted against
#include "UI/TimelineLayout.h"

// The timeline band's vertical margin used to be a literal 8 px. Every build and test config runs at
// 1× content scale and the default font, so a margin that does not track DPI looks correct in the
// whole suite — the bug only shows on a user's high-DPI monitor, where the dots the margin exists to
// protect grow with the font while the clearance does not. These exercise the pure (explicit font
// size) form at simulated scales, which is the only place that blind spot is visible.

namespace {

constexpr float kBase = ofs::kDefaultFontSizeBase; // 18 px
// A band tall enough that the quarter-height cap never binds, even at 2×.
constexpr float kTallBand = 400.0f;

} // namespace

TEST_CASE("Script-line band margin scales with the font size") {
    const float at1x = ofs::ui::scriptLineVMargin(kTallBand, kBase);
    CHECK(at1x == doctest::Approx(8.1f)); // the literal it replaced, at the reference font

    // Linear in the font size: 1.5× DPI must widen the clearance by exactly 1.5×, not leave it at 8 px.
    CHECK(ofs::ui::scriptLineVMargin(kTallBand, kBase * 1.5f) == doctest::Approx(at1x * 1.5f));
    CHECK(ofs::ui::scriptLineVMargin(kTallBand, kBase * 2.0f) == doctest::Approx(at1x * 2.0f));
    CHECK(ofs::ui::scriptLineVMargin(kTallBand, kBase * 0.5f) == doctest::Approx(at1x * 0.5f));
}

TEST_CASE("Script-line band margin clears exactly one source dot") {
    // The margin exists to keep the pos=0/100 dots off the band edge, so it must not fall behind the
    // dot radius at any scale — that relationship is the whole reason it is font-relative.
    for (const float scale : {0.5f, 1.0f, 1.5f, 2.0f, 3.0f})
        CHECK(ofs::ui::scriptLineVMargin(kTallBand, kBase * scale) ==
              doctest::Approx(ofs::ui::dotRadius(kBase * scale)));
}

TEST_CASE("Script-line band margin cap keeps a thin lane's band usable") {
    // A Lanes row with many visible axes is short. At a high DPI one dot radius can exceed the row, and
    // an uncapped margin inverts posToScreenY (top of the range below the bottom) and pins screenYToPos
    // to 0 — the cap is what keeps the mapping monotonic.
    for (const float band : {6.0f, 12.0f, 24.0f, 60.0f, 400.0f})
        for (const float scale : {1.0f, 2.0f, 3.0f}) {
            const float margin = ofs::ui::scriptLineVMargin(band, kBase * scale);
            CHECK(margin <= band * 0.25f);              // never eats more than half the band
            CHECK(band - 2.0f * margin >= band * 0.5f); // so the script line always keeps half the row
        }
}
