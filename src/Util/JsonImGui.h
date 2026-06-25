#pragma once

#include "imgui.h"
#include <cstdio>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

// Colors serialize as hex strings — "#RRGGBB" when opaque, "#RRGGBBAA" when the alpha
// channel is < 1. Channel order is RGBA (matching CSS / ImGui's hex edit). Precision is
// 8 bits per channel, which is the rendering precision anyway. These helpers are the one
// place that formatting/parsing lives; both adl_serializer<ImColor> and the heatmap-mark
// (de)serialization in Theme.cpp go through them.
namespace ofs::jsonimgui {

inline std::string colorToHex(float r, float g, float b, float a) {
    auto q = [](float v) {
        const float clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<int>(clamped * 255.0f + 0.5f);
    };
    char buf[10];
    if (q(a) >= 255)
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", q(r), q(g), q(b));
    else
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", q(r), q(g), q(b), q(a));
    return buf;
}

// Parse "#RGB", "#RGBA", "#RRGGBB", "#RRGGBBAA" (leading '#' optional) into rgba in [0,1].
// Returns false (leaving the outputs untouched) on a malformed string.
inline bool colorFromHex(std::string_view s, float &r, float &g, float &b, float &a) {
    if (!s.empty() && s.front() == '#')
        s.remove_prefix(1);
    auto nibble = [](char ch, int &out) {
        if (ch >= '0' && ch <= '9')
            out = ch - '0';
        else if (ch >= 'a' && ch <= 'f')
            out = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F')
            out = ch - 'A' + 10;
        else
            return false;
        return true;
    };
    float out[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (s.size() == 6 || s.size() == 8) {
        const size_t channels = s.size() / 2;
        for (size_t i = 0; i < channels; ++i) {
            int hi = 0, lo = 0;
            if (!nibble(s[i * 2], hi) || !nibble(s[i * 2 + 1], lo))
                return false;
            out[i] = static_cast<float>(hi * 16 + lo) / 255.0f;
        }
    } else if (s.size() == 3 || s.size() == 4) {
        for (size_t i = 0; i < s.size(); ++i) {
            int v = 0;
            if (!nibble(s[i], v))
                return false;
            out[i] = static_cast<float>(v * 16 + v) / 255.0f;
        }
    } else {
        return false;
    }
    r = out[0];
    g = out[1];
    b = out[2];
    a = out[3];
    return true;
}

} // namespace ofs::jsonimgui

// ImVec2 and ImColor are in the global namespace.
// To use ADL, these must be in the same namespace as the type or in nlohmann namespace.
namespace nlohmann {
template <> struct adl_serializer<ImVec2> {
    static void to_json(json &j, const ImVec2 &v) { j = json{{"x", v.x}, {"y", v.y}}; }
    static void from_json(const json &j, ImVec2 &v) {
        v.x = j.value("x", 0.0f);
        v.y = j.value("y", 0.0f);
    }
};

template <> struct adl_serializer<ImColor> {
    static void to_json(json &j, const ImColor &c) {
        j = ofs::jsonimgui::colorToHex(c.Value.x, c.Value.y, c.Value.z, c.Value.w);
    }
    static void from_json(const json &j, ImColor &c) {
        c.Value = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
        if (j.is_string()) {
            const std::string s = j.get<std::string>();
            ofs::jsonimgui::colorFromHex(s, c.Value.x, c.Value.y, c.Value.z, c.Value.w);
        }
    }
};
} // namespace nlohmann
