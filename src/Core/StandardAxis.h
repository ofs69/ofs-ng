#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ofs {

enum class StandardAxis : uint8_t {
    L0 = 0,
    L1,
    L2,
    R0,
    R1,
    R2,
    V0,
    V1,
    A0,
    A1,
    // User-created scratch axes
    S0,
    S1,
    S2,
    S3,
    S4,
    S5,
    S6,
    S7,
    S8,
    S9,
    Count
};

inline constexpr size_t kStandardAxisCount = static_cast<size_t>(StandardAxis::Count);

// User-creatable scratch slots S0–S9; the cap on how many scratch axes can exist at once.
inline constexpr int kMaxScratchAxes = static_cast<int>(StandardAxis::Count) - static_cast<int>(StandardAxis::S0);

constexpr bool isScratchAxis(StandardAxis ax) noexcept {
    return ax >= StandardAxis::S0 && ax <= StandardAxis::S9;
}

constexpr int scratchIndex(StandardAxis ax) noexcept {
    return static_cast<int>(ax) - static_cast<int>(StandardAxis::S0);
}

namespace detail {
struct AxisNames {
    std::string_view shortName; // canonical tag, e.g. "L0"
    std::string_view fullName;  // short name + role suffix, e.g. "L0 (Stroke)"
};
// Indexed by StandardAxis; scratch axes have no role suffix so short == full.
inline constexpr std::array<AxisNames, kStandardAxisCount> kAxisNames = {{
    {"L0", "L0 (Stroke)"}, {"L1", "L1 (Surge)"}, {"L2", "L2 (Sway)"},  {"R0", "R0 (Twist)"}, {"R1", "R1 (Roll)"},
    {"R2", "R2 (Pitch)"},  {"V0", "V0 (Vibe)"},  {"V1", "V1 (Vibe2)"}, {"A0", "A0 (Air)"},   {"A1", "A1 (Air2)"},
    {"S0", "S0"},          {"S1", "S1"},         {"S2", "S2"},         {"S3", "S3"},         {"S4", "S4"},
    {"S5", "S5"},          {"S6", "S6"},         {"S7", "S7"},         {"S8", "S8"},         {"S9", "S9"},
}};
} // namespace detail

constexpr std::string_view standardAxisName(StandardAxis a) noexcept {
    const auto i = static_cast<size_t>(a);
    return i < kStandardAxisCount ? detail::kAxisNames[i].fullName : std::string_view{};
}

constexpr std::string_view standardAxisShortName(StandardAxis a) noexcept {
    const auto i = static_cast<size_t>(a);
    return i < kStandardAxisCount ? detail::kAxisNames[i].shortName : std::string_view{};
}

std::string_view standardAxisTag(StandardAxis a) noexcept;
std::optional<StandardAxis> standardAxisFromTag(std::string_view tag) noexcept;

} // namespace ofs
