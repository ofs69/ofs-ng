#pragma once

// Localized axis display names. Kept out of Core/StandardAxis.h on purpose: composing the name pulls
// in the runtime Translator + frame arena, neither of which Core (or the plugin-tests target) links.

#include <cstdint>

namespace ofs {
enum class StandardAxis : uint8_t;
}

namespace ofs::loc {

// Axis display name with its descriptor translated to the active language, e.g. "L0 (Stroke)" →
// "L0 (Hub)". The code ("L0") is a fixed TCode identifier and stays literal; only the parenthetical
// descriptor is translated. Scratch axes (S0–S9) have no descriptor and return just their code.
// Composed into the frame arena like fmtScratch — main-thread render path only, valid until the next
// FrameAllocator::reset(). For the canonical, language-independent id use ofs::standardAxisName().
const char *localizedAxisName(StandardAxis axis);

} // namespace ofs::loc
