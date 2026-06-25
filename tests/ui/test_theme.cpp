#include <doctest/doctest.h>

#include "UI/Theme.h"
#include "Util/PathUtil.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

// The unit-test binary compiles with OFS_TEST_PREF_SUBDIR, so getPrefPath() (and
// therefore the <pref>/themes dir these tests write to) resolves to a temp dir.

namespace {
// Colors serialize as 8-bit hex (see JsonImGui.h), so a round-trip is exact only to ~1/255.
constexpr double kColorEps = 1.0 / 255.0;
bool colorsEqual(const ImColor &a, const ImColor &b) {
    return a.Value.x == doctest::Approx(b.Value.x).epsilon(kColorEps) &&
           a.Value.y == doctest::Approx(b.Value.y).epsilon(kColorEps) &&
           a.Value.z == doctest::Approx(b.Value.z).epsilon(kColorEps) &&
           a.Value.w == doctest::Approx(b.Value.w).epsilon(kColorEps);
}
} // namespace

TEST_CASE("Theme round-trips colors, vars, nodes and gradient through saveAs/load") {
    ofs::theme::Theme in;
    ofs::theme::makeDarkTheme(&in);

    // Mutate a representative spread of every namespace.
    in.seed = 0xEE935C;
    in.colors[ImGuiCol_WindowBg] = ImColor(0.10f, 0.07f, 0.05f, 1.0f);
    in.colors[AppCol_AxisL0] = ImColor(0.20f, 0.40f, 0.60f, 0.80f);
    in.vars[ImGuiStyleVar_WindowRounding] = {6.0f, 0.0f}; // scalar ImGui var
    in.vars[ImGuiStyleVar_FramePadding] = {9.0f, 5.0f};   // vec2 ImGui var
    in.vars[AppVar_SimGlobalOpacity] = {0.42f, 0.0f};     // scalar App var
    in.vars[AppVar_NodePadding] = {11.0f, 7.0f};          // vec2 App var
    in.nodes.Colors[ImNodesCol_GridBackground] = ImGui::ColorConvertFloat4ToU32({0.02f, 0.06f, 0.08f, 1.0f});
    in.vars[AppVar_NodeGridSpacing] = {33.0f, 0.0f};
    in.vars[AppVar_NodePinRadius] = {5.5f, 0.0f};
    in.heatmapColors.clear();
    in.heatmapColors.addMark(0.0f, ImColor(0.9f, 0.9f, 0.96f, 1.0f));
    in.heatmapColors.addMark(1.0f, ImColor(1.0f, 0.0f, 0.0f, 1.0f));

    REQUIRE(ofs::theme::saveAs(in, "roundtrip"));

    ofs::theme::Theme out;
    REQUIRE(ofs::theme::load("roundtrip", &out));

    CHECK(out.name == "roundtrip");
    CHECK(out.isDark == in.isDark);
    CHECK(out.seed == in.seed);

    for (int i = 0; i < AppCol_COUNT; ++i)
        CHECK(colorsEqual(out.colors[i], in.colors[i]));

    for (int i = 0; i < AppVar_COUNT; ++i) {
        CHECK(out.vars[i].x == doctest::Approx(in.vars[i].x));
        CHECK(out.vars[i].y == doctest::Approx(in.vars[i].y));
    }

    for (int i = 0; i < ImNodesCol_COUNT; ++i)
        CHECK(out.nodes.Colors[i] == in.nodes.Colors[i]);

    const auto &inMarks = in.heatmapColors.getMarks();
    const auto &outMarks = out.heatmapColors.getMarks();
    REQUIRE(outMarks.size() == inMarks.size());
    for (size_t i = 0; i < inMarks.size(); ++i) {
        CHECK(outMarks[i].position == doctest::Approx(inMarks[i].position));
        CHECK(outMarks[i].color[0] == doctest::Approx(inMarks[i].color[0]).epsilon(kColorEps));
        CHECK(outMarks[i].color[3] == doctest::Approx(inMarks[i].color[3]).epsilon(kColorEps));
    }

    ofs::theme::remove("roundtrip");
}

TEST_CASE("Theme load is forward-tolerant: missing keys keep base default, unknown keys ignored") {
    // Base default for the key we will drop.
    ofs::theme::Theme base;
    ofs::theme::makeDarkTheme(&base);
    const ImColor baseWindowBg = base.colors[ImGuiCol_WindowBg];

    // Write a theme whose WindowBg differs from the base default.
    ofs::theme::Theme in;
    ofs::theme::makeDarkTheme(&in);
    in.colors[ImGuiCol_WindowBg] = ImColor(0.99f, 0.01f, 0.50f, 1.0f);
    in.colors[AppCol_AxisL0] = ImColor(0.123f, 0.456f, 0.789f, 1.0f);
    REQUIRE(ofs::theme::saveAs(in, "forward"));

    // Drop WindowBg and inject an unknown key directly in the file.
    const auto file = ofs::util::getPrefPath() / "themes" / "forward.json";
    nlohmann::json j;
    {
        std::ifstream f(file);
        f >> j;
    }
    j["imguiColors"].erase("WindowBg");
    j["thisKeyDoesNotExist"] = 42;
    j["imguiColors"]["alsoUnknown"] = "#FFFFFF";
    {
        std::ofstream f(file, std::ios::trunc);
        f << j.dump(2);
    }

    ofs::theme::Theme out;
    REQUIRE(ofs::theme::load("forward", &out)); // unknown keys must not break the load

    // Dropped key kept the base default; a present key kept its saved value.
    CHECK(colorsEqual(out.colors[ImGuiCol_WindowBg], baseWindowBg));
    CHECK(colorsEqual(out.colors[AppCol_AxisL0], in.colors[AppCol_AxisL0]));

    ofs::theme::remove("forward");
}

TEST_CASE("Theme load refuses a file newer than kThemeSchemaVersion") {
    ofs::theme::Theme in;
    ofs::theme::makeDarkTheme(&in);
    REQUIRE(ofs::theme::saveAs(in, "newer"));

    // Bump the on-disk version past what this build supports.
    const auto file = ofs::util::getPrefPath() / "themes" / "newer.json";
    nlohmann::json j;
    {
        std::ifstream f(file);
        f >> j;
    }
    j["version"] = ofs::theme::kThemeSchemaVersion + 1;
    {
        std::ofstream f(file, std::ios::trunc);
        f << j.dump(2);
    }

    ofs::theme::Theme out;
    CHECK_FALSE(ofs::theme::load("newer", &out));

    ofs::theme::remove("newer");
}

TEST_CASE("Embedded dark theme parses with the C++ loader (key names match the generator)") {
    // dark.json ships in the assets archive (read via ofs::res), so makeDarkTheme always overlays
    // it — no data/themes staging needed. The seed is present only in the generated JSON; the
    // hardcoded fallback leaves seed == 0, so a non-zero seed proves the embedded JSON parsed with
    // matching keys.
    ofs::theme::Theme t;
    ofs::theme::makeDarkTheme(&t);
    CHECK(t.seed == 0xF2A33Cu); // default amber seed from gen_theme.py (Graphite & Amber)
    CHECK(t.isDark);
}

TEST_CASE("Theme list reports the shipped base themes") {
    const auto themes = ofs::theme::list();
    bool hasDark = false;
    bool hasLight = false;
    for (const auto &info : themes) {
        if (info.name == "Dark" && info.shipped && info.isDark)
            hasDark = true;
        if (info.name == "Light" && info.shipped && !info.isDark)
            hasLight = true;
    }
    CHECK(hasDark);
    CHECK(hasLight);
}

TEST_CASE("Theme saveAs then list surfaces the user theme; remove deletes it") {
    ofs::theme::Theme t;
    ofs::theme::makeLightTheme(&t);
    REQUIRE(ofs::theme::saveAs(t, "MyUserTheme"));

    auto names = ofs::theme::list();
    bool found = false;
    for (const auto &info : names)
        if (info.name == "MyUserTheme") {
            found = true;
            CHECK_FALSE(info.shipped);
            CHECK(info.isDark == false);
        }
    CHECK(found);

    CHECK(ofs::theme::remove("MyUserTheme"));
    names = ofs::theme::list();
    for (const auto &info : names)
        CHECK(info.name != "MyUserTheme");
}
