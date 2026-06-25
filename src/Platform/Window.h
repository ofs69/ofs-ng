#pragma once
#include <SDL3/SDL.h>
#include <functional>
#include <string>

namespace ofs {
struct WindowConfig {
    // Default fallback; Application::init() overrides this with the git-identity title.
    std::string title = "ofs-ng";
    int width = 1920;
    int height = 1080;
};

class Window {
  public:
    Window(WindowConfig config);

    ~Window();

    bool init();

    // Bring up SDL's gamepad subsystem. Split out of init() because SDL_INIT_GAMEPAD's device/DB
    // enumeration is the largest single startup cost (~360 ms) and is irrelevant to the first frame;
    // Application::run() calls this once after the window is shown. Idempotent.
    void initGamepadSubsystem();

    void swapBuffers();

#ifdef _WIN32
    // Windows-only: pace presents on the desktop compositor (DWM) rather than the GL driver's vsync.
    // This sidesteps the NVIDIA "Threaded Optimization" busy-wait, where SwapBuffers spins a core under
    // vsync instead of parking the thread. Non-Windows builds compile this path out entirely (here and
    // in Application's swap logic) and keep ordinary driver vsync.

    // True when frames can be paced on the compositor's vblank — windowed apps on Win8+ are always
    // composited. False under the headless test backend.
    [[nodiscard]] bool compositorPacingAvailable() const;

    // Block until the compositor has presented `count` more vblanks (DwmFlush), parking the thread the
    // way the driver's vsync should but doesn't under threaded optimization. `count` is the refresh
    // divisor N (present every Nth vblank); pass 1 for full refresh. Returns false if composition is
    // disabled (pre-Win8 / unredirected exclusive fullscreen) so the caller can revert to driver vsync.
    bool waitForCompositorVBlank(int count);
#endif

    // Reveal the window. It is created hidden (see init()); call once the first frame has been
    // rendered and swapped so neither the native caption nor the OS's blank background ever flashes.
    void show();

    void pollEvents(bool &running, const std::function<void(SDL_Event *)> &eventHandler = nullptr);

    void waitEvents(int timeoutMs);

    [[nodiscard]] SDL_Window *getNativeWindow() const { return window; }
    [[nodiscard]] SDL_GLContext getGLContext() const { return glContext; }

    [[nodiscard]] int getWidth() const;

    [[nodiscard]] int getHeight() const;

    // The window title (git-identity string built in Application::init()). Used by the custom title
    // bar, which replaces the native caption that would otherwise show this text.
    [[nodiscard]] const std::string &getTitle() const { return config.title; }

    // --- Custom (in-app) title bar support ---
    // Drops the native title bar/border and installs a hit test so the OS still handles window move
    // (via the title bar strip), edge resize, and double-click-maximize. Call once after init().
    void enableCustomTitleBar();

    // Per-frame: tells the hit test which part of the top strip is draggable. `buttonsLeftX` is the x
    // (in window points, from the left edge) where the caption buttons begin; everything in the strip
    // to its left is the drag handle. `[searchLeftX, searchRightX)` is carved back out as interactive
    // (e.g. the command-palette search box) so clicks there aren't consumed as a window drag; pass
    // equal values for no carve-out. No-op unless enableCustomTitleBar() succeeded.
    void setTitleBarLayout(float height, float buttonsLeftX, float searchLeftX = 0.0f, float searchRightX = 0.0f);

    void minimize();
    void toggleMaximize();
    [[nodiscard]] bool isMaximized() const;

    // Toggles borderless desktop fullscreen. No display mode is set, so SDL uses fullscreen-desktop
    // (the window grows to cover the display) rather than an exclusive video mode change.
    void toggleFullscreen();
    [[nodiscard]] bool isFullscreen() const;

    // --- Persisted window geometry (size/position restore across sessions) ---
    // The restore (non-maximized) rect plus whether the window is maximized. Position is in SDL global
    // screen coordinates, size in window (logical) units — the same space as SDL_CreateWindow and
    // SDL_GetDisplayUsableBounds, so it is DPI-independent and round-trips safely.
    struct Geometry {
        int x = 0;
        int y = 0;
        int width = 0; // 0 = unset
        int height = 0;
        bool maximized = false;
    };

    // Apply a persisted geometry to the freshly-created (still hidden) window, replacing the start-size
    // heuristic. Validated against the live displays first: the size is clamped to the target monitor's
    // usable bounds, and the rect is re-centered if it would land off every connected display (an
    // unplugged second monitor / shrunk resolution), so a stale geometry can never strand the window
    // where it can't be grabbed. A zero/empty size is treated as "unset" and leaves the heuristic
    // result in place. No-op under the headless backend.
    void restoreGeometry(const Geometry &g);

    // The geometry to persist: the last *normal* (non-maximized, non-fullscreen) position+size tracked
    // from the window move/resize events in pollEvents, plus whether the window is currently maximized.
    [[nodiscard]] Geometry currentRestoreGeometry() const;

  private:
    // Snapshot the current position+size as the restore rect while the window is in its normal state; a
    // no-op while maximized/fullscreen (so the last normal rect is preserved) and under headless. Driven
    // off the window move/resize events in pollEvents.
    void updateRestoreGeometry();

    // Draggable/resizable regions for the borderless hit test, refreshed each frame.
    struct TitleBarHitInfo {
        float height = 0.0f;       // title bar strip height (window points)
        float buttonsLeftX = 0.0f; // caption buttons start here; drag region is to the left
        float searchLeftX = 0.0f;  // interactive carve-out (search box): left edge
        float searchRightX = 0.0f; // interactive carve-out (search box): right edge
        bool maximized = false;    // suppress edge-resize zones while maximized
        bool enabled = false;      // true once the borderless custom title bar is active
    };
    static SDL_HitTestResult SDLCALL hitTest(SDL_Window *win, const SDL_Point *area, void *data);

    // Re-asserts the Win11 DWM rounded-corner opt-in (set once in enableCustomTitleBar). SDL's
    // fullscreen restyle silently drops it, so it is re-applied on the fullscreen enter/leave events.
    // No-op off Windows.
    void applyRoundedCorners(bool rounded);

    WindowConfig config;
    SDL_Window *window = nullptr;
    SDL_GLContext glContext = nullptr;
    TitleBarHitInfo titleBarHit;
    Geometry restoreGeom_; // last normal (non-maximized) geometry, tracked for persistence
};
} // namespace ofs
