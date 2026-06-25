#pragma once

namespace ofs {

// Compile-time null-backend selector. The headless ui-tests binary is built with OFS_HEADLESS defined
// (a dedicated CMake target); every other build — the shipping app, the real-GL ui-tests — leaves it
// false. Being constexpr, each `if (kHeadless)` branch folds at compile time: the shipping app carries
// no headless code, and the headless tests never execute (and the compiler can drop) the GL paths.
//
// It is a constant, not a runtime flag, on purpose: "is there a GL context?" is fixed for a given
// binary, so a mutable global with a setter would be hidden, toggleable process state for no benefit.
// GL-boundary code (Window, Heatmap, the scene graph, mpv video) consults this to skip work that would
// otherwise call through never-loaded GL entry points and crash.
#ifdef OFS_HEADLESS
inline constexpr bool kHeadless = true;
#else
inline constexpr bool kHeadless = false;
#endif

} // namespace ofs
