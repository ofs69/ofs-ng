#include "Scenegraph/VrCamera.h"
#include <cmath>
#include <doctest/doctest.h>

using namespace ofs::vrcam;

// project() is the analytic inverse of the VrShader fragment shader; unproject() mirrors the shader
// exactly. So project(unproject(uv)) == uv proves project lands a sphere direction exactly where the
// shader samples it — the safety net for the VR overlay anchoring.
TEST_CASE("project inverts unproject across the field of view") {
    const ImVec2 rot{0.6f, 0.45f};
    const float zoom = 0.5f;
    const float aspect = 16.0f / 9.0f;
    for (float u = -0.4f; u <= 0.4f + 1e-4f; u += 0.2f) {
        for (float v = -0.4f; v <= 0.4f + 1e-4f; v += 0.2f) {
            const glm::vec3 dir = unproject({u, v}, rot, zoom, aspect);
            const Projected p = project(dir, rot, zoom, aspect);
            REQUIRE(p.visible);
            CHECK(p.ndc.x == doctest::Approx(u).epsilon(0.01));
            CHECK(p.ndc.y == doctest::Approx(v).epsilon(0.01));
        }
    }
}

TEST_CASE("sphereDir and dirToYawPitch round-trip") {
    for (float yaw = -3.0f; yaw <= 3.0f; yaw += 1.0f) {
        for (float pitch = -1.2f; pitch <= 1.2f; pitch += 0.4f) {
            const glm::vec3 d = sphereDir(yaw, pitch);
            float y = 0.f, p = 0.f;
            dirToYawPitch(d, y, p);
            // Compare via sin/cos so the ±π wrap of atan2 doesn't trip the check.
            CHECK(std::cos(y) == doctest::Approx(std::cos(yaw)).epsilon(0.01));
            CHECK(std::sin(y) == doctest::Approx(std::sin(yaw)).epsilon(0.01));
            CHECK(p == doctest::Approx(pitch).epsilon(0.01));
        }
    }
}

TEST_CASE("a direction behind the camera is culled") {
    const ImVec2 rot{0.5f, 0.5f};
    const float zoom = 0.5f;
    const float aspect = 1.0f;
    const glm::vec3 forward = unproject({0.0f, 0.0f}, rot, zoom, aspect);
    CHECK(project(forward, rot, zoom, aspect).visible);
    CHECK_FALSE(project(-forward, rot, zoom, aspect).visible);
}

// dragRotation's contract: the returned rotation must pin the grabbed world direction exactly under
// the cursor. So project(grabbed, dragRotation(grabbed, cursorNdc, …), …).ndc == cursorNdc — the
// grab-to-pan analogue of the project/unproject round-trip, and the safety net for the connected feel.
TEST_CASE("dragRotation pins the grabbed direction under the cursor") {
    const ImVec2 startRot{0.55f, 0.5f};
    const float zoom = 0.5f;
    const float aspect = 16.0f / 9.0f;
    // Grab a point near the centre, then drag the cursor to several offsets within the FOV.
    const glm::vec3 grabbed = unproject({0.05f, -0.05f}, startRot, zoom, aspect);
    for (float u = -0.3f; u <= 0.3f + 1e-4f; u += 0.3f) {
        for (float v = -0.3f; v <= 0.3f + 1e-4f; v += 0.3f) {
            const ImVec2 cursor{u, v};
            const ImVec2 newRot = dragRotation(grabbed, cursor, startRot, zoom, aspect);
            const Projected p = project(grabbed, newRot, zoom, aspect);
            REQUIRE(p.visible);
            CHECK(p.ndc.x == doctest::Approx(cursor.x).epsilon(0.02));
            CHECK(p.ndc.y == doctest::Approx(cursor.y).epsilon(0.02));
        }
    }
}

TEST_CASE("dragRotation with the cursor still on the grab point leaves the rotation unchanged") {
    const ImVec2 startRot{0.4f, 0.55f};
    const float zoom = 0.5f;
    const float aspect = 1.0f;
    const ImVec2 grabNdc{0.12f, 0.08f};
    const glm::vec3 grabbed = unproject(grabNdc, startRot, zoom, aspect);
    // No cursor movement ⇒ the point is already under the pointer ⇒ same orientation back.
    const ImVec2 same = dragRotation(grabbed, grabNdc, startRot, zoom, aspect);
    CHECK(same.x == doctest::Approx(startRot.x).epsilon(0.01));
    CHECK(same.y == doctest::Approx(startRot.y).epsilon(0.01));
}

TEST_CASE("dragRotation stays in range and finite past the pitch pole") {
    const ImVec2 startRot{0.5f, 0.98f}; // near the top pole
    const float zoom = 0.5f;
    const float aspect = 1.0f;
    const glm::vec3 grabbed = unproject({0.0f, 0.4f}, startRot, zoom, aspect);
    // A drag that would tip past ±90° must clamp gracefully (acos clamp + pitch clamp), never NaN.
    const ImVec2 rot = dragRotation(grabbed, {0.0f, -0.49f}, startRot, zoom, aspect);
    CHECK(std::isfinite(rot.x));
    CHECK(std::isfinite(rot.y));
    CHECK(rot.x >= 0.0f);
    CHECK(rot.x <= 1.0f);
    CHECK(rot.y >= 0.0f);
    CHECK(rot.y <= 1.0f);
}

TEST_CASE("project respects a non-default zoom and content aspect") {
    // Cover the non-trivial frustum: a wider zoom pushes the same direction further toward the edge,
    // and a non-1 aspect scales the vertical extent (vfov) but not the horizontal.
    const ImVec2 rot{0.5f, 0.5f};
    const float aspect = 16.0f / 9.0f;
    const glm::vec3 dir = unproject({0.2f, 0.2f}, rot, 0.5f, aspect);
    const Projected tight = project(dir, rot, 0.5f, aspect);
    const Projected wide = project(dir, rot, 1.0f, aspect);
    REQUIRE(tight.visible);
    REQUIRE(wide.visible);
    CHECK(std::abs(wide.ndc.x) > std::abs(tight.ndc.x)); // larger zoom ⇒ same dir maps further out
}

TEST_CASE("a direction outside the FOV stays visible but projects past the edge") {
    const ImVec2 rot{0.5f, 0.5f};
    const float zoom = 0.5f;
    const float aspect = 1.0f;
    // `visible` means "in front of the camera", not "inside the FOV": a direction past the screen
    // edge still projects (visible == true) with |ndc| > 0.5, so the caller clips it instead of
    // popping the overlay out. (Culling at the edge is what caused the reported pop-in/out.)
    const Projected inside = project(unproject({0.45f, 0.0f}, rot, zoom, aspect), rot, zoom, aspect);
    CHECK(inside.visible);
    CHECK(std::abs(inside.ndc.x) <= 0.5f);
    const Projected wide = project(unproject({2.0f, 0.0f}, rot, zoom, aspect), rot, zoom, aspect);
    CHECK(wide.visible);
    CHECK(wide.ndc.x > 0.5f);
}
