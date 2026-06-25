#pragma once

#include <SDL3/SDL_timer.h>
#include <cstdint>

namespace ofs::util {

// High-resolution stopwatch backed by SDL's performance counter (SDL_GetPerformanceCounter) — the same
// monotonic, platform-best timer SDL uses internally (QueryPerformanceCounter on Windows, mach/clock
// elsewhere). Its resolution is well below a microsecond, so it stays meaningful for the sub-millisecond
// work this profiles (packing/unpacking an undo snapshot), where SDL_GetTicks()' millisecond granularity
// would just read 0. Construct to start timing; read elapsed*() as often as needed; restart() to re-zero.
class Stopwatch {
  public:
    Stopwatch() : start_(SDL_GetPerformanceCounter()) {}

    void restart() { start_ = SDL_GetPerformanceCounter(); }

    [[nodiscard]] double elapsedMs() const { return elapsedCounts() * 1.0e3 / freq(); }
    [[nodiscard]] double elapsedUs() const { return elapsedCounts() * 1.0e6 / freq(); }

  private:
    [[nodiscard]] double elapsedCounts() const { return static_cast<double>(SDL_GetPerformanceCounter() - start_); }

    // Counts/second is fixed for the process lifetime, so query the frequency once and cache it.
    static double freq() {
        static const double f = static_cast<double>(SDL_GetPerformanceFrequency());
        return f;
    }

    uint64_t start_;
};

} // namespace ofs::util
