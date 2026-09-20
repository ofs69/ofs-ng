#include "UI/SimulatorBaseline.h"
#include <doctest/doctest.h>
#include <glm/gtc/constants.hpp>

TEST_CASE("simulator baseline matches reference geometry") {
    ofs::sg::Transform stroker;

    stroker.position = {0.f, -1.f, 0.f};
    auto baseline = ofs::simulatorBaselineGeometry(stroker, 1.f);
    CHECK(baseline.tip.y == doctest::Approx(-1.875f));
    CHECK(baseline.target.y == doctest::Approx(-1.875f));
    CHECK(baseline.percent == doctest::Approx(0.f));

    stroker.position = {0.f, 1.f, 0.f};
    baseline = ofs::simulatorBaselineGeometry(stroker, 1.f);
    CHECK(baseline.tip.y == doctest::Approx(0.125f));
    CHECK(baseline.percent == doctest::Approx(100.f));
}

TEST_CASE("simulator baseline measures rotated tip in world space") {
    ofs::sg::Transform stroker;
    stroker.rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3{0.f, 0.f, 1.f});

    const auto baseline = ofs::simulatorBaselineGeometry(stroker, 1.f);
    CHECK(baseline.tip.x == doctest::Approx(0.875f));
    CHECK(baseline.tip.y == doctest::Approx(0.f));
    CHECK(baseline.percent > 100.f);
}
