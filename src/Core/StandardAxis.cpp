#include "StandardAxis.h"
#include <algorithm>
#include <string>

namespace ofs {

static_assert(kStandardAxisCount == 20, "StandardAxis enum changed — update all switch statements");

// The tag is exactly the canonical short name.
std::string_view standardAxisTag(StandardAxis a) noexcept {
    return standardAxisShortName(a);
}

std::optional<StandardAxis> standardAxisFromTag(std::string_view tag) noexcept {
    // Canonical short-name tags (case-sensitive, fast path)
    for (size_t i = 0; i < kStandardAxisCount; ++i)
        if (tag == detail::kAxisNames[i].shortName)
            return static_cast<StandardAxis>(i);

    // Case-insensitive alias matching for funscript filename suffixes. The lowercase short name
    // ("l0".."s9") is generated from the name table; only the role-word aliases are listed here.
    struct Alias {
        std::string_view name;
        StandardAxis axis;
    };
    using enum StandardAxis;
    static constexpr std::array<Alias, 12> kRoleAliases = {{
        {.name = "stroke", .axis = L0},
        {.name = "surge", .axis = L1},
        {.name = "sway", .axis = L2},
        {.name = "twist", .axis = R0},
        {.name = "roll", .axis = R1},
        {.name = "pitch", .axis = R2},
        {.name = "vibe", .axis = V0},
        {.name = "vibrate", .axis = V0},
        {.name = "vibe2", .axis = V1},
        {.name = "vibrate2", .axis = V1},
        {.name = "air", .axis = A0},
        {.name = "air2", .axis = A1},
    }};

    std::string lower(tag);
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return std::tolower(c); });
    for (size_t i = 0; i < kStandardAxisCount; ++i) {
        std::string s(detail::kAxisNames[i].shortName);
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
        if (lower == s)
            return static_cast<StandardAxis>(i);
    }
    for (const auto &al : kRoleAliases)
        if (lower == al.name)
            return al.axis;

    return std::nullopt;
}

} // namespace ofs
