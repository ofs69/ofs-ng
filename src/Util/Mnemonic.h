#pragma once
#include <array>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <string_view>

namespace ofs {

// Generates a short two-word mnemonic ("Bold Arc", "Calm Beat", "Dark Crest", …) from a
// sequential index. Consecutive indices advance both words by one ("Bold Arc" → "Calm Beat"),
// so adjacent objects read as a clear progression. After a full pass of 50 the noun column is
// shifted by one (a diagonal Latin square), so all 50×50 = 2500 pairs are produced before any
// repeat. Suitable for auto-naming any project object — not coupled to any domain.
inline std::string generateMnemonic(int index) {
    constexpr std::array<std::string_view, 50> kAdj = {
        "Bold",   "Calm",   "Dark",   "Deep",  "Fast",  "Firm",  "Loud",  "Slow",   "Soft",   "Warm",
        "Bright", "Sharp",  "Smooth", "Wild",  "Mild",  "Pure",  "Keen",  "Swift",  "Sleek",  "Crisp",
        "Pale",   "Cool",   "Neat",   "Odd",   "Brave", "Wise",  "Cozy",  "Golden", "Silver", "Amber",
        "Bronze", "Cobalt", "Neon",   "Rusty", "Dusty", "Vivid", "Vague", "Grand",  "Tiny",   "Huge",
        "Proud",  "Rare",   "Fine",   "True",  "Safe",  "Quick", "Quiet", "Gentle", "Heavy",  "Light"};
    constexpr std::array<std::string_view, 50> kNoun = {
        "Arc",  "Beat", "Crest", "Dip",  "Edge",  "Flow", "Knot",  "Peak", "Pulse", "Ramp",  "Step", "Wave", "Path",
        "Loop", "Ring", "Hook",  "Link", "Chain", "Node", "Core",  "Grid", "Spark", "Flame", "Glow", "Beam", "Ray",
        "Star", "Moon", "Sun",   "Wind", "Rain",  "Snow", "Frost", "Ice",  "Rock",  "Stone", "Clay", "Sand", "Hill",
        "Vale", "Cave", "Cove",  "Bay",  "Reef",  "Tide", "Surf",  "Mist", "Cloud", "Sky",   "Zone"};

    // Walk the palette as a diagonal Latin square: the adjective is the index mod 50, and the
    // noun is shifted by one extra step on each full pass so the (adj, noun) pair never repeats
    // until all 2500 combinations are used.
    const size_t n = kNoun.size(); // == kAdj.size()
    const auto s = static_cast<size_t>(index < 0 ? -index : index);
    const std::string_view adj = kAdj[s % n];
    const std::string_view noun = kNoun[(s + s / n) % n];

    return fmt::format("{} {}", adj, noun);
}

} // namespace ofs
