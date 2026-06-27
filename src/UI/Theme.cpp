#include "Theme.h"
#include "Util/FileUtil.h"
#include "Util/JsonImGui.h"
#include "Util/JsonUtil.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"
#include "Util/Resources.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ofs::theme {

namespace {

Theme gActiveTheme = {};
std::function<void()> gPostApplyHook;

constexpr const char *kCustomColorNames[static_cast<int>(AppCol_COUNT) - static_cast<int>(ImGuiCol_COUNT)] = {
    "AxisL0",
    "AxisL1",
    "AxisL2",
    "AxisR0",
    "AxisR1",
    "AxisR2",
    "AxisV0",
    "AxisV1",
    "AxisA0",
    "AxisA1",
    "AxisS0",
    "AxisS1",
    "AxisS2",
    "AxisS3",
    "AxisS4",
    "AxisS5",
    "AxisS6",
    "AxisS7",
    "AxisS8",
    "AxisS9",
    "AxisDimL0",
    "AxisDimL1",
    "AxisDimL2",
    "AxisDimR0",
    "AxisDimR1",
    "AxisDimR2",
    "AxisDimV0",
    "AxisDimV1",
    "AxisDimA0",
    "AxisDimA1",
    "AxisDimS0",
    "AxisDimS1",
    "AxisDimS2",
    "AxisDimS3",
    "AxisDimS4",
    "AxisDimS5",
    "AxisDimS6",
    "AxisDimS7",
    "AxisDimS8",
    "AxisDimS9",
    "TextShadow",
    "BandBarStripe",
    "BandBarText",
    "RegionBand",
    "Bookmark",
    "BookmarkDot",
    "BookmarkDotHovered",
    "PlayCursor",
    "HudBg",
    "StripBg",
    "StripActiveBg",
    "StripHoverBg",
    "StripSeparator",
    "StripDivider",
    "LockIndicator",
    "CurveBgTop",
    "CurveBgBottom",
    "CurveHoverBg",
    "Waveform",
    "GridLine",
    "GridLineMid",
    "SelectedLine",
    "DragPreview",
    "DragPreviewOutline",
    "SelectionBox",
    "SelectionBoxFill",
    "TimelineOutline",
    "TimelinePoint",
    "TimelinePointSelected",
    "OverlayLineMajor",
    "OverlayLineMinor",
    "TempoMeasureLine",
    "ProcessingPanelBg",
    "NodeIO",
    "NodeDiscrete",
    "NodeFunctional",
    "NodeMath",
    "LinkDiscrete",
    "LinkFunctional",
    "SimViewBg",
    "SimTintTop",
    "SimTintSide",
    "SimDivider",
    "SimCrosshair",
    "SimArcRef",
    "SimArc",
    "Sim2DFront",
    "Sim2DBack",
    "Sim2DBorder",
    "Sim2DText",
    "Sim2DLines",
    "Sim2DIndicator",
    "TimelineCursorOuter",
    "TimelineCursorInner",
    "VideoTimelineFill",
    "TimelineVisibleRegionFill",
    "TimelineVisibleRegionBorder",
    "BookmarkOutline",
    "HeatmapBase",
    "UnsavedIndicator",
    "ScriptSeekCursor",
    "ScriptPlayCursor",
    "Success",
    "Warning",
    "Error"};
static_assert(std::size(kCustomColorNames) == static_cast<int>(AppCol_COUNT) - static_cast<int>(ImGuiCol_COUNT),
              "kCustomColorNames out of sync with AppCol enum");

// Names for the custom (>= ImGuiStyleVar_COUNT) style-var slots, in AppVar order.
// Must match the appVars keys emitted by tools/gen_theme.py.
constexpr const char *kCustomVarNames[static_cast<int>(AppVar_COUNT) - static_cast<int>(ImGuiStyleVar_COUNT)] = {
    "ScriptSeekCursorWidth", "ScriptPlayCursorWidth", "GridLineMidWidth",  "OverlayLineMajorWidth",
    "WaveformScale",         "SimGlobalOpacity",      "NodeGridSpacing",   "NodeCornerRounding",
    "NodePadding",           "NodeBorderThickness",   "NodeLinkThickness", "NodePinRadius"};
static_assert(std::size(kCustomVarNames) == static_cast<int>(AppVar_COUNT) - static_cast<int>(ImGuiStyleVar_COUNT),
              "kCustomVarNames out of sync with AppVar enum");

// The only custom style-var slot stored as a full ImVec2; the rest are scalars in .x.
bool isAppVarVec2(int appVarIdx) {
    return appVarIdx == AppVar_NodePadding;
}

// Names for the ImGui built-in style vars, indexed by ImGuiStyleVar. ImGui exposes
// no public GetStyleVarName, so this table is hand-maintained — it MUST track the
// ImGuiStyleVar_ enum in lib/imgui/imgui.h (the static_assert guards the count).
// Names match the imguiVars keys emitted by tools/gen_theme.py.
constexpr const char *kImGuiStyleVarNames[ImGuiStyleVar_COUNT] = {"Alpha",
                                                                  "DisabledAlpha",
                                                                  "WindowPadding",
                                                                  "WindowRounding",
                                                                  "WindowBorderSize",
                                                                  "WindowMinSize",
                                                                  "WindowTitleAlign",
                                                                  "ChildRounding",
                                                                  "ChildBorderSize",
                                                                  "PopupRounding",
                                                                  "PopupBorderSize",
                                                                  "FramePadding",
                                                                  "FrameRounding",
                                                                  "FrameBorderSize",
                                                                  "ItemSpacing",
                                                                  "ItemInnerSpacing",
                                                                  "IndentSpacing",
                                                                  "CellPadding",
                                                                  "ScrollbarSize",
                                                                  "ScrollbarRounding",
                                                                  "ScrollbarPadding",
                                                                  "GrabMinSize",
                                                                  "GrabRounding",
                                                                  "ImageRounding",
                                                                  "ImageBorderSize",
                                                                  "TabRounding",
                                                                  "TabBorderSize",
                                                                  "TabMinWidthBase",
                                                                  "TabMinWidthShrink",
                                                                  "TabBarBorderSize",
                                                                  "TabBarOverlineSize",
                                                                  "TableAngledHeadersAngle",
                                                                  "TableAngledHeadersTextAlign",
                                                                  "TreeLinesSize",
                                                                  "TreeLinesRounding",
                                                                  "DragDropTargetRounding",
                                                                  "ButtonTextAlign",
                                                                  "SelectableTextAlign",
                                                                  "SeparatorSize",
                                                                  "SeparatorTextBorderSize",
                                                                  "SeparatorTextAlign",
                                                                  "SeparatorTextPadding",
                                                                  "DockingSeparatorSize"};
static_assert(std::size(kImGuiStyleVarNames) == ImGuiStyleVar_COUNT,
              "kImGuiStyleVarNames out of sync with ImGuiStyleVar_ enum (lib/imgui/imgui.h)");

// Names for the imnodes structural colors, indexed by ImNodesCol. Matches the
// nodes.colors keys emitted by tools/gen_theme.py.
constexpr const char *kImNodesColNames[ImNodesCol_COUNT] = {"NodeBackground",
                                                            "NodeBackgroundHovered",
                                                            "NodeBackgroundSelected",
                                                            "NodeOutline",
                                                            "TitleBar",
                                                            "TitleBarHovered",
                                                            "TitleBarSelected",
                                                            "Link",
                                                            "LinkHovered",
                                                            "LinkSelected",
                                                            "Pin",
                                                            "PinHovered",
                                                            "BoxSelector",
                                                            "BoxSelectorOutline",
                                                            "GridBackground",
                                                            "GridLine",
                                                            "GridLinePrimary",
                                                            "MiniMapBackground",
                                                            "MiniMapBackgroundHovered",
                                                            "MiniMapOutline",
                                                            "MiniMapOutlineHovered",
                                                            "MiniMapNodeBackground",
                                                            "MiniMapNodeBackgroundHovered",
                                                            "MiniMapNodeBackgroundSelected",
                                                            "MiniMapNodeOutline",
                                                            "MiniMapLink",
                                                            "MiniMapLinkSelected",
                                                            "MiniMapCanvas",
                                                            "MiniMapCanvasOutline"};
static_assert(std::size(kImNodesColNames) == ImNodesCol_COUNT, "kImNodesColNames out of sync with ImNodesCol_ enum");

nlohmann::json markToJson(const ImGradientMark &m) {
    return {{"pos", m.position}, {"color", ofs::jsonimgui::colorToHex(m.color[0], m.color[1], m.color[2], m.color[3])}};
}

ImGradientMark markFromJson(const nlohmann::json &j) {
    float r = 0.5f, g = 0.5f, b = 0.5f, a = 1.f;
    if (const auto c = j.find("color"); c != j.end() && c->is_string())
        ofs::jsonimgui::colorFromHex(c->get<std::string>(), r, g, b, a);
    return {r, g, b, a, j.value("pos", 0.f)};
}

// S*0.7, V*0.55 — mirrors gen_axis_colors.py dim() logic
ImColor dimAxisColor(ImColor c) {
    float h = 0.f, s = 0.f, v = 0.f;
    ImGui::ColorConvertRGBtoHSV(c.Value.x, c.Value.y, c.Value.z, h, s, v);
    float r = 0.f, g = 0.f, b = 0.f;
    ImGui::ColorConvertHSVtoRGB(h, s * 0.7f, v * 0.55f, r, g, b);
    return {r, g, b, c.Value.w};
}

void fillAxisDimColors(Theme *dst) {
    constexpr int kAxisCount = AppCol_AxisDimL0 - AppCol_AxisL0;
    for (int i = 0; i < kAxisCount; ++i)
        dst->colors[AppCol_AxisDimL0 + i] = dimAxisColor(dst->colors[AppCol_AxisL0 + i]);
}

void fillDefaultGradients(Theme *dst) {
    dst->heatmapColors.clear();
    dst->heatmapColors.addMark(0.f, ImColor(235 / 255.f, 235 / 255.f, 245 / 255.f, 1.f));
    dst->heatmapColors.addMark(0.2f, ImColor(0x1E / 255.f, 0x90 / 255.f, 1.f, 1.f));
    dst->heatmapColors.addMark(0.4f, ImColor(0.f, 1.f, 1.f, 1.f));
    dst->heatmapColors.addMark(0.6f, ImColor(0.f, 1.f, 0.f, 1.f));
    dst->heatmapColors.addMark(0.8f, ImColor(1.f, 1.f, 0.f, 1.f));
    dst->heatmapColors.addMark(1.f, ImColor(1.f, 0.f, 0.f, 1.f));
    dst->heatmapMaxSpeed = 400.0f;
    dst->backgroundAxisOpacity = 0.3f;
}

// Fill the ImGui range of vars[] with the live ImGui style-var defaults, read via
// the GetStyleVarInfo metadata table. MUST run before overlaying a theme's
// imguiVars, because the tool emits identity OVERRIDES only — absent keys must
// resolve to the ImGui default.
void captureImGuiVarDefaults(Theme *dst) {
    const ImGuiStyle tmp; // default-constructed: ImGui's default sizes
    for (int i = 0; i < ImGuiStyleVar_COUNT; ++i) {
        const ImGuiStyleVarInfo *info = ImGui::GetStyleVarInfo(i);
        const auto *src = static_cast<const float *>(info->GetVarPtr(const_cast<ImGuiStyle *>(&tmp)));
        dst->vars[i].x = src[0];
        dst->vars[i].y = info->Count == 2 ? src[1] : 0.f;
    }
}

// Mirror the AppVar node-geometry slots from a (default or themed) ImNodesStyle.
void mirrorNodeGeometryToVars(Theme *dst) {
    dst->vars[AppVar_NodeGridSpacing] = {dst->nodes.GridSpacing, 0.f};
    dst->vars[AppVar_NodeCornerRounding] = {dst->nodes.NodeCornerRounding, 0.f};
    dst->vars[AppVar_NodePadding] = dst->nodes.NodePadding;
    dst->vars[AppVar_NodeBorderThickness] = {dst->nodes.NodeBorderThickness, 0.f};
    dst->vars[AppVar_NodeLinkThickness] = {dst->nodes.LinkThickness, 0.f};
    dst->vars[AppVar_NodePinRadius] = {dst->nodes.PinCircleRadius, 0.f};
}

ImColor u32ToColor(unsigned int c) {
    return {ImGui::ColorConvertU32ToFloat4(c)};
}

unsigned int colorToU32(const ImColor &c) {
    return ImGui::ColorConvertFloat4ToU32(c.Value);
}

nlohmann::json varToJson(const ImVec2 &v, bool vec2) {
    if (vec2)
        return nlohmann::json::array({v.x, v.y});
    return v.x;
}

void varFromJson(const nlohmann::json &j, ImVec2 *out) {
    if (j.is_array()) {
        if (!j.empty() && j[0].is_number())
            out->x = j[0].get<float>();
        if (j.size() >= 2 && j[1].is_number())
            out->y = j[1].get<float>();
    } else if (j.is_number()) {
        out->x = j.get<float>();
    }
}

// Serialize a full Theme to the versioned schema.
nlohmann::json themeToJson(const Theme &t) {
    nlohmann::json imColors = nlohmann::json::object();
    for (int i = 0; i < ImGuiCol_COUNT; ++i)
        imColors[ImGui::GetStyleColorName(i)] = t.colors[i];

    nlohmann::json appColors = nlohmann::json::object();
    for (int i = ImGuiCol_COUNT; i < AppCol_COUNT; ++i) {
        // Dim axis colors are always derived from base colors in apply(); skip them.
        if (i >= AppCol_AxisDimL0 && i <= AppCol_AxisDimS9)
            continue;
        appColors[kCustomColorNames[i - ImGuiCol_COUNT]] = t.colors[i];
    }

    nlohmann::json imVars = nlohmann::json::object();
    for (int i = 0; i < ImGuiStyleVar_COUNT; ++i) {
        const bool vec2 = ImGui::GetStyleVarInfo(i)->Count == 2;
        imVars[kImGuiStyleVarNames[i]] = varToJson(t.vars[i], vec2);
    }

    nlohmann::json appVars = nlohmann::json::object();
    for (int i = ImGuiStyleVar_COUNT; i < AppVar_COUNT; ++i)
        appVars[kCustomVarNames[i - ImGuiStyleVar_COUNT]] = varToJson(t.vars[i], isAppVarVec2(i));

    nlohmann::json nodeColors = nlohmann::json::object();
    for (int i = 0; i < ImNodesCol_COUNT; ++i)
        nodeColors[kImNodesColNames[i]] = u32ToColor(t.nodes.Colors[i]);

    nlohmann::json bgMarks = nlohmann::json::array();
    for (const auto &m : t.heatmapColors.getMarks())
        bgMarks.push_back(markToJson(m));

    char seedHex[8];
    std::snprintf(seedHex, sizeof(seedHex), "#%06X", t.seed & 0xFFFFFF);

    nlohmann::json j;
    j["version"] = t.version;
    j["name"] = t.name;
    j["isDark"] = t.isDark;
    j["seed"] = seedHex;
    j["imguiColors"] = imColors;
    j["colors"] = appColors;
    j["imguiVars"] = imVars;
    j["appVars"] = appVars;
    j["nodes"] = {{"colors", nodeColors}};
    j["heatmapColors"] = bgMarks;
    j["heatmapMaxSpeed"] = t.heatmapMaxSpeed;
    j["backgroundAxisOpacity"] = t.backgroundAxisOpacity;
    return j;
}

// Overlay a theme JSON onto dst (which must already hold base defaults). Forward-
// tolerant: missing keys keep the base default; unknown keys are skipped.
void parseTheme(const nlohmann::json &j, Theme *dst) {
    using ofs::util::jsonArrayIf;
    using ofs::util::jsonObjectIf;
    dst->version = j.value("version", dst->version);
    dst->name = j.value("name", dst->name);
    dst->isDark = j.value("isDark", dst->isDark);
    if (const auto it = j.find("seed"); it != j.end() && it->is_string()) {
        const std::string s = it->get<std::string>();
        const char *p = s.c_str() + (!s.empty() && s.front() == '#' ? 1 : 0);
        char *end = nullptr;
        const unsigned long v = std::strtoul(p, &end, 16);
        if (end != p)
            dst->seed = static_cast<ImU32>(v);
    }

    if (const auto *it = jsonObjectIf(j, "imguiColors")) {
        for (int i = 0; i < ImGuiCol_COUNT; ++i) {
            const char *name = ImGui::GetStyleColorName(i);
            if (const auto c = it->find(name); c != it->end())
                dst->colors[i] = c->get<ImColor>();
        }
    }
    if (const auto *it = jsonObjectIf(j, "colors")) {
        for (int i = ImGuiCol_COUNT; i < AppCol_COUNT; ++i) {
            const char *name = kCustomColorNames[i - ImGuiCol_COUNT];
            if (const auto c = it->find(name); c != it->end())
                dst->colors[i] = c->get<ImColor>();
        }
    }
    if (const auto *it = jsonObjectIf(j, "imguiVars")) {
        for (int i = 0; i < ImGuiStyleVar_COUNT; ++i) {
            if (const auto v = it->find(kImGuiStyleVarNames[i]); v != it->end())
                varFromJson(*v, &dst->vars[i]);
        }
    }
    if (const auto *it = jsonObjectIf(j, "appVars")) {
        for (int i = ImGuiStyleVar_COUNT; i < AppVar_COUNT; ++i) {
            if (const auto v = it->find(kCustomVarNames[i - ImGuiStyleVar_COUNT]); v != it->end())
                varFromJson(*v, &dst->vars[i]);
        }
    }
    if (const auto *nodes = jsonObjectIf(j, "nodes")) {
        if (const auto *cols = jsonObjectIf(*nodes, "colors")) {
            for (int i = 0; i < ImNodesCol_COUNT; ++i) {
                if (const auto c = cols->find(kImNodesColNames[i]); c != cols->end())
                    dst->nodes.Colors[i] = colorToU32(c->get<ImColor>());
            }
        }
    }
    dst->heatmapMaxSpeed = j.value("heatmapMaxSpeed", dst->heatmapMaxSpeed);
    dst->backgroundAxisOpacity = j.value("backgroundAxisOpacity", dst->backgroundAxisOpacity);
    if (const auto *it = jsonArrayIf(j, "heatmapColors")) {
        dst->heatmapColors.clear();
        for (const auto &item : *it) {
            auto m = markFromJson(item);
            dst->heatmapColors.addMark(m.position, ImColor(m.color[0], m.color[1], m.color[2], m.color[3]));
        }
    }
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// Shipped base themes are read-only. Dark/Light resolve to the bundled JSON;
// Retro Dark/Light are pure code-defaults with no JSON file.
bool isShippedName(std::string_view name) {
    return iequals(name, "Dark") || iequals(name, "Light") || iequals(name, "Retro Dark") ||
           iequals(name, "Retro Light");
}

std::filesystem::path userThemesDir() {
    return ofs::util::getPrefPath() / "themes";
}

std::filesystem::path userThemeFile(std::string_view name) {
    return userThemesDir() / ofs::util::fromUtf8(fmt::format("{}.json", name));
}

// Overlay the shipped base theme named <stem> onto dst. The JSON is read from the assets archive
// (ofs::res), so this is independent of any on-disk data/themes dir (user themes live in <pref>/themes).
bool loadShippedJsonInto(const char *stem, Theme *dst) {
    auto json = ofs::res::readText(fmt::format("data/themes/{}.json", stem));
    if (!json)
        return false;
    try {
        parseTheme(nlohmann::json::parse(*json), dst);
        return true;
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to parse shipped theme '{}': {}", stem, e.what());
        return false;
    }
}

// Shared across dark and light baselines: the same deeper/more-saturated axis set
// reads well on both. Dim colors are derived; do not write them separately.
void fillBaseAxisColors(Theme *dst) {
    dst->colors[AppCol_AxisL0] = ImColor(220, 50, 50, 255);
    dst->colors[AppCol_AxisL1] = ImColor(210, 130, 0, 255);
    dst->colors[AppCol_AxisL2] = ImColor(190, 170, 0, 255);
    dst->colors[AppCol_AxisR0] = ImColor(40, 160, 40, 255);
    dst->colors[AppCol_AxisR1] = ImColor(0, 140, 160, 255);
    dst->colors[AppCol_AxisR2] = ImColor(60, 60, 210, 255);
    dst->colors[AppCol_AxisV0] = ImColor(130, 50, 210, 255);
    dst->colors[AppCol_AxisV1] = ImColor(180, 50, 160, 255);
    dst->colors[AppCol_AxisA0] = ImColor(120, 170, 40, 255);
    dst->colors[AppCol_AxisA1] = ImColor(40, 170, 140, 255);
    dst->colors[AppCol_AxisS0] = ImColor(130, 130, 130, 255);
    dst->colors[AppCol_AxisS1] = ImColor(100, 125, 145, 255);
    dst->colors[AppCol_AxisS2] = ImColor(145, 110, 110, 255);
    dst->colors[AppCol_AxisS3] = ImColor(145, 135, 95, 255);
    dst->colors[AppCol_AxisS4] = ImColor(105, 145, 105, 255);
    dst->colors[AppCol_AxisS5] = ImColor(95, 140, 140, 255);
    dst->colors[AppCol_AxisS6] = ImColor(125, 105, 145, 255);
    dst->colors[AppCol_AxisS7] = ImColor(145, 120, 95, 255);
    dst->colors[AppCol_AxisS8] = ImColor(100, 145, 125, 255);
    dst->colors[AppCol_AxisS9] = ImColor(130, 115, 150, 255);
    fillAxisDimColors(dst);
}

// Fallback values for the custom (non-axis) AppCol slots. The M3 Dark/Light themes
// overwrite every one of these via loadShippedJsonInto(), so this matters only for
// code-only themes (Retro Dark/Light, which have no embedded JSON): it guarantees
// they are visually complete — e.g. the overlay/grid lines actually
// render instead of drawing transparent. Derived from the ImGui palette already in
// dst->colors so one body adapts to both dark and light. Requires fillBaseAxisColors
// and the ImGui color copy to have run first.
void fillBaseAppColors(Theme *dst) {
    const ImVec4 text = dst->colors[ImGuiCol_Text].Value;
    const ImVec4 win = dst->colors[ImGuiCol_WindowBg].Value;
    const ImVec4 accent = dst->colors[ImGuiCol_HeaderActive].Value;

    auto t = [&](float a) { return ImColor(text.x, text.y, text.z, a); };
    auto lit = [](float r, float g, float b, float a = 1.f) { return ImColor(r, g, b, a); };
    auto mix = [](const ImVec4 &a, const ImVec4 &b, float k) {
        return ImColor(a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k, a.z + (b.z - a.z) * k, 1.f);
    };

    dst->colors[AppCol_TextShadow] = lit(0.f, 0.f, 0.f, 0.5f);

    dst->colors[AppCol_BandBarStripe] = t(0.10f);
    dst->colors[AppCol_BandBarText] = t(1.0f);
    dst->colors[AppCol_RegionBand] = mix(win, text, 0.22f);

    dst->colors[AppCol_Bookmark] = ImColor(accent);
    dst->colors[AppCol_BookmarkDot] = t(0.90f);
    dst->colors[AppCol_BookmarkDotHovered] = t(1.0f);

    dst->colors[AppCol_PlayCursor] = lit(0.90f, 0.20f, 0.20f);
    dst->colors[AppCol_HudBg] = lit(0.f, 0.f, 0.f, 0.55f);

    dst->colors[AppCol_StripBg] = mix(win, text, 0.04f);
    dst->colors[AppCol_StripActiveBg] = mix(win, accent, 0.20f);
    dst->colors[AppCol_StripHoverBg] = t(0.06f);
    dst->colors[AppCol_StripSeparator] = t(0.30f);
    dst->colors[AppCol_StripDivider] = t(0.30f);
    dst->colors[AppCol_LockIndicator] = ImColor(accent);

    dst->colors[AppCol_CurveBgTop] = mix(win, text, 0.03f);
    dst->colors[AppCol_CurveBgBottom] = ImColor(win);
    dst->colors[AppCol_CurveHoverBg] = t(0.02f);
    // Opaque but muted: a blend toward the text color reads clearly on the recessed curve track
    // without overpowering the curves drawn on top.
    dst->colors[AppCol_Waveform] = mix(win, text, 0.62f);
    dst->colors[AppCol_GridLine] = t(0.18f);
    dst->colors[AppCol_GridLineMid] = t(0.30f);

    dst->colors[AppCol_SelectedLine] = ImColor(accent);
    dst->colors[AppCol_DragPreview] = lit(0.85f, 0.60f, 0.25f);
    dst->colors[AppCol_DragPreviewOutline] = t(1.0f);
    dst->colors[AppCol_SelectionBox] = ImColor(accent);
    dst->colors[AppCol_SelectionBoxFill] = ImColor(accent.x, accent.y, accent.z, 0.30f);

    // Seed-independent action-point identity (mirrored in gen_theme.py's _TIMELINE_POINT).
    dst->colors[AppCol_TimelineOutline] = lit(0.f, 0.f, 0.f);
    dst->colors[AppCol_TimelinePoint] = lit(1.f, 0.f, 0.f);
    dst->colors[AppCol_TimelinePointSelected] = lit(11 / 255.f, 252 / 255.f, 3 / 255.f);

    dst->colors[AppCol_OverlayLineMajor] = t(0.47f);
    dst->colors[AppCol_OverlayLineMinor] = t(0.20f);

    // Tempo measure (downbeat) line color. Seed-independent (mirrored in gen_theme.py's _TEMPO_MEASURE_COLOR).
    dst->colors[AppCol_TempoMeasureLine] = lit(0xe7 / 255.f, 0x97 / 255.f, 0x5c / 255.f);

    dst->colors[AppCol_ProcessingPanelBg] = ImColor(win.x, win.y, win.z, 0.92f);
    dst->colors[AppCol_NodeIO] = lit(0.60f, 0.60f, 0.60f);
    dst->colors[AppCol_NodeDiscrete] = lit(0.30f, 0.60f, 0.90f);
    dst->colors[AppCol_NodeFunctional] = lit(0.50f, 0.80f, 0.40f);
    dst->colors[AppCol_NodeMath] = lit(0.90f, 0.60f, 0.30f);
    dst->colors[AppCol_LinkDiscrete] = lit(0.30f, 0.60f, 0.90f);
    dst->colors[AppCol_LinkFunctional] = lit(0.50f, 0.80f, 0.40f);

    dst->colors[AppCol_SimViewBg] = ImColor(win);
    dst->colors[AppCol_SimTintTop] = ImColor(accent.x, accent.y, accent.z, 0.12f);
    dst->colors[AppCol_SimTintSide] = ImColor(accent.x, accent.y, accent.z, 0.12f);
    dst->colors[AppCol_SimDivider] = t(0.30f);
    dst->colors[AppCol_SimCrosshair] = t(0.40f);
    dst->colors[AppCol_SimArcRef] = t(0.60f);
    dst->colors[AppCol_SimArc] = ImColor(accent);
    // 2D simulator bar — the fixed cyan/navy/red identity, shared by every theme. Seed-independent
    // on purpose: the M3 Dark/Light themes carry the same literals (gen_theme.py _SIM2D), so the
    // bar looks identical across all themes regardless of seed.
    dst->colors[AppCol_Sim2DFront] = ImColor(0x01, 0xBA, 0xEF, 255);
    dst->colors[AppCol_Sim2DBack] = ImColor(0x10, 0x10, 0x10, 0xBF);
    dst->colors[AppCol_Sim2DBorder] = ImColor(0x0B, 0x4F, 0x6C, 255);
    dst->colors[AppCol_Sim2DText] = ImColor(255, 255, 255, 255);
    dst->colors[AppCol_Sim2DLines] = ImColor(0x0B, 0x4F, 0x6C, 255);
    dst->colors[AppCol_Sim2DIndicator] = ImColor(0xFF, 0x4F, 0x6C, 255);

    dst->colors[AppCol_TimelineCursorOuter] = t(1.0f);
    dst->colors[AppCol_TimelineCursorInner] = ImColor(win);
    dst->colors[AppCol_VideoTimelineFill] = lit(0.93f, 0.66f, 0.32f);
    dst->colors[AppCol_TimelineVisibleRegionFill] = t(0.24f);
    dst->colors[AppCol_TimelineVisibleRegionBorder] = t(0.47f);

    dst->colors[AppCol_BookmarkOutline] = lit(0.f, 0.f, 0.f, 0.60f);
    dst->colors[AppCol_HeatmapBase] = ImColor(win);
    dst->colors[AppCol_UnsavedIndicator] = lit(0.90f, 0.30f, 0.20f);
    dst->colors[AppCol_ScriptSeekCursor] = t(1.0f);
    dst->colors[AppCol_ScriptPlayCursor] = lit(0.90f, 0.30f, 0.20f);

    dst->colors[AppCol_Success] = lit(0.45f, 0.80f, 0.45f);
    dst->colors[AppCol_Warning] = lit(0.95f, 0.70f, 0.20f);
    dst->colors[AppCol_Error] = lit(0.90f, 0.40f, 0.35f);
}

// Base defaults used by both schemes: stock ImGui colors + seed-independent app data.
// fillBaseAppColors() supplies fallback values for the custom AppCol slots; the M3
// Dark/Light themes overwrite them via loadShippedJsonInto(). Code-only themes (Retro
// Dark/Light, which have no embedded JSON) rely on those fallbacks to render correctly.
void makeBaseDefaults(Theme *dst, bool dark) {
    dst->name = dark ? "Dark" : "Light";
    dst->isDark = dark;

    ImGuiStyle tmp;
    if (dark)
        ImGui::StyleColorsDark(&tmp);
    else
        ImGui::StyleColorsLight(&tmp);
    for (int i = 0; i < ImGuiCol_COUNT; ++i)
        dst->colors[i] = tmp.Colors[i];

    fillBaseAxisColors(dst);
    fillBaseAppColors(dst);

    captureImGuiVarDefaults(dst);
    dst->vars[AppVar_ScriptSeekCursorWidth] = {2.f, 0.f};
    dst->vars[AppVar_ScriptPlayCursorWidth] = {1.f, 0.f};
    dst->vars[AppVar_GridLineMidWidth] = {1.f, 0.f};
    dst->vars[AppVar_OverlayLineMajorWidth] = {1.f, 0.f};
    dst->vars[AppVar_WaveformScale] = {0.9f, 0.f};
    dst->vars[AppVar_SimGlobalOpacity] = {0.75f, 0.f};

    if (dark)
        ImNodes::StyleColorsDark(&dst->nodes);
    else
        ImNodes::StyleColorsLight(&dst->nodes);
    mirrorNodeGeometryToVars(dst);

    fillDefaultGradients(dst);
}

} // namespace

void apply(const Theme &theme) {
    // Cache first; derive dim axis colors from the (possibly just-edited) base colors
    // so they are never stale regardless of what the caller stored in theme.colors[].
    gActiveTheme = theme;
    fillAxisDimColors(&gActiveTheme);

    ImGuiStyle &style = ImGui::GetStyle();

    // 1. ImGui colors
    for (int i = 0; i < ImGuiCol_COUNT; ++i)
        style.Colors[i] = gActiveTheme.colors[i];

    // 2. ImGui vars via the metadata table (handles float/vec2 uniformly).
    for (int i = 0; i < ImGuiStyleVar_COUNT; ++i) {
        const ImGuiStyleVarInfo *info = ImGui::GetStyleVarInfo(i);
        auto *dst = static_cast<float *>(info->GetVarPtr(&style));
        dst[0] = gActiveTheme.vars[i].x;
        if (info->Count == 2)
            dst[1] = gActiveTheme.vars[i].y;
    }

    // 3. App colors
    for (int i = ImGuiCol_COUNT; i < AppCol_COUNT; ++i)
        detail::gCustomColors[i - ImGuiCol_COUNT] = gActiveTheme.colors[i];

    // 4. App vars
    for (int i = ImGuiStyleVar_COUNT; i < AppVar_COUNT; ++i)
        detail::gCustomVars[i - ImGuiStyleVar_COUNT] = gActiveTheme.vars[i];

    // 5. AppVar node-geometry slots are the source of truth; sync back so
    // getActive().nodes and the imnodes style stay consistent with vars[].
    gActiveTheme.nodes.GridSpacing = gActiveTheme.vars[AppVar_NodeGridSpacing].x;
    gActiveTheme.nodes.NodeCornerRounding = gActiveTheme.vars[AppVar_NodeCornerRounding].x;
    gActiveTheme.nodes.NodePadding = gActiveTheme.vars[AppVar_NodePadding];
    gActiveTheme.nodes.NodeBorderThickness = gActiveTheme.vars[AppVar_NodeBorderThickness].x;
    gActiveTheme.nodes.LinkThickness = gActiveTheme.vars[AppVar_NodeLinkThickness].x;
    gActiveTheme.nodes.PinCircleRadius = gActiveTheme.vars[AppVar_NodePinRadius].x;

    // 6. imnodes global style. Guarded so apply() is safe without a context
    // (e.g. unit tests that never create one).
    if (ImNodes::GetCurrentContext() != nullptr)
        ImNodes::GetStyle() = gActiveTheme.nodes;

    // 7. post-apply hook (DPI re-capture + heatmap LUT rebuild; see
    // Application::onThemeApplied). Keeping GL-touching work in the app side lets
    // Theme.cpp stay free of GL/Heatmap link deps (so it links into unit tests).
    if (gPostApplyHook)
        gPostApplyHook();
}

void setPostApplyHook(std::function<void()> hook) {
    gPostApplyHook = std::move(hook);
}

Theme &getActive() {
    return gActiveTheme;
}

std::vector<ThemeInfo> list() {
    std::vector<ThemeInfo> out;
    out.push_back({.name = "Dark", .shipped = true, .isDark = true});
    out.push_back({.name = "Light", .shipped = true, .isDark = false});
    out.push_back({.name = "Retro Dark", .shipped = true, .isDark = true});
    out.push_back({.name = "Retro Light", .shipped = true, .isDark = false});

    std::error_code ec;
    const auto dir = userThemesDir();
    if (std::filesystem::exists(dir, ec)) {
        for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json")
                continue;
            const std::string name = ofs::util::toUtf8(entry.path().stem());
            if (isShippedName(name)) // shipped names never shadowed by a user file
                continue;
            ThemeInfo info{.name = name, .shipped = false, .isDark = true};
            try {
                auto text = ofs::util::readFile(entry.path());
                if (text)
                    info.isDark = nlohmann::json::parse(*text).value("isDark", true);
            } catch (const std::exception &) {
            }
            out.push_back(info);
        }
    }
    return out;
}

bool load(std::string_view name, Theme *dst) {
    if (isShippedName(name)) {
        if (iequals(name, "Retro Dark"))
            makeBaseDefaults(dst, true);
        else if (iequals(name, "Retro Light"))
            makeBaseDefaults(dst, false);
        else if (iequals(name, "Light"))
            makeLightTheme(dst);
        else
            makeDarkTheme(dst);
        return true;
    }
    try {
        const auto file = userThemeFile(name);
        auto text = ofs::util::readFile(file);
        if (!text)
            return false;
        nlohmann::json j = nlohmann::json::parse(*text);
        const int version = j.value("version", kThemeSchemaVersion);
        if (version > kThemeSchemaVersion) {
            OFS_CORE_ERROR("Theme '{}' version {} is newer than supported version {}.", std::string(name), version,
                           kThemeSchemaVersion);
            return false;
        }
        if (j.value("isDark", true))
            makeDarkTheme(dst);
        else
            makeLightTheme(dst);
        parseTheme(j, dst);
        return true;
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to load theme '{}': {}", std::string(name), e.what());
        return false;
    }
}

bool saveAs(const Theme &theme, std::string_view name) {
    try {
        std::filesystem::create_directories(userThemesDir());
        Theme copy = theme;
        copy.name = std::string(name);
        ofs::util::writeFileAtomic(userThemeFile(name), themeToJson(copy).dump(4));
        return true;
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to save theme '{}': {}", std::string(name), e.what());
        return false;
    }
}

bool remove(std::string_view name) {
    if (isShippedName(name))
        return false;
    std::error_code ec;
    return std::filesystem::remove(userThemeFile(name), ec);
}

bool exportToFile(const Theme &theme, const std::filesystem::path &dest) {
    try {
        ofs::util::writeFileAtomic(dest, themeToJson(theme).dump(4));
        return true;
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to export theme to '{}': {}", ofs::util::toUtf8(dest), e.what());
        return false;
    }
}

std::optional<std::string> importFromFile(const std::filesystem::path &src) {
    try {
        auto text = ofs::util::readFile(src);
        if (!text)
            return std::nullopt;
        nlohmann::json j = nlohmann::json::parse(*text);
        const int version = j.value("version", kThemeSchemaVersion);
        if (version > kThemeSchemaVersion) {
            OFS_CORE_ERROR("Theme file '{}' version {} is newer than supported version {}.", ofs::util::toUtf8(src),
                           version, kThemeSchemaVersion);
            return std::nullopt;
        }

        // Start from the matching base so any keys the file omits keep sane defaults,
        // exactly like load() does for the per-name user files.
        Theme imported;
        if (j.value("isDark", true))
            makeDarkTheme(&imported);
        else
            makeLightTheme(&imported);
        parseTheme(j, &imported);

        // Prefer the embedded name; fall back to the file stem. A name that collides
        // with a read-only shipped theme can't be saved under that name, so suffix it.
        std::string name = j.value("name", std::string{});
        if (name.empty())
            name = ofs::util::toUtf8(src.stem());
        if (name.empty() || isShippedName(name))
            name += " (imported)";

        imported.name = name;
        if (!saveAs(imported, name))
            return std::nullopt;
        return name;
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to import theme from '{}': {}", ofs::util::toUtf8(src), e.what());
        return std::nullopt;
    }
}

void save() {
    // Shipped themes are read-only; editing one requires Save-As (editor UI).
    if (gActiveTheme.name.empty() || isShippedName(gActiveTheme.name))
        return;
    saveAs(gActiveTheme, gActiveTheme.name);
}

void makeDarkTheme(Theme *dst) {
    makeBaseDefaults(dst, true);
    loadShippedJsonInto("dark", dst);
}

void makeLightTheme(Theme *dst) {
    makeBaseDefaults(dst, false);
    loadShippedJsonInto("light", dst);
}

const Theme &defaultDark() {
    static const Theme t = [] {
        Theme x;
        makeDarkTheme(&x);
        return x;
    }();
    return t;
}

const Theme &defaultLight() {
    static const Theme t = [] {
        Theme x;
        makeLightTheme(&x);
        return x;
    }();
    return t;
}

} // namespace ofs::theme
