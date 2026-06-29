#pragma once
#include "Platform/Window.h"
#include <SDL3/SDL_events.h>
#include <cstdint>
#include <imgui.h>
#include <imnodes.h>
#include <memory>

namespace ofs {

struct FpsIdling {
    float fpsIdle = 9.0f;
    bool enableIdling = true;
    bool isIdling = false;
    double lastActiveTime = 0.0;
    static constexpr double idleDelay = 0.5;
};

class Application {
  public:
    Application();

    virtual ~Application();

    virtual bool init();

    int run();

    void stop() { running = false; }

  protected:
    virtual void onUpdate(float deltaTime) {}

    virtual void onRender() {}

    virtual void onImGuiRender() {}

    // Frame-loop hook for an in-app title bar. Called within the ImGui frame, just before
    // onImGuiRender(), so the bar stacks above anything that window renders (e.g. a main menu bar).
    virtual void renderTitleBar() {}

    virtual void onPostRender() {}

    // Called once from run(), immediately after the first frame is painted and the window is shown.
    // The place to run heavy, first-frame-irrelevant init (e.g. loading the plugin host) so the window
    // appears before that cost is paid, not after.
    virtual void onStartupComplete() {}

    virtual void onEvent(SDL_Event *event) {}

    virtual bool canAppIdle() const { return true; }

    // Whether an SDL event is ambient noise to drop from the queue entirely (SDL event filter). A
    // connected controller never rests perfectly: its sticks/triggers emit a continuous stream of
    // sub-deadzone axis-motion events (plus per-cycle UPDATE_COMPLETE / SENSOR markers). Merely not
    // counting these as activity is not enough — every queued event still wakes SDL_WaitEventTimeout,
    // so the idle loop would spin at the pad's poll rate. Dropping them at the filter keeps them out of
    // the queue, so the wait actually sleeps. Default: drop nothing. OfsApp overrides this. Safe because
    // analog holds are driven by polling SDL_GetGamepadAxis in onUpdate, not by these events, and a real
    // past-deadzone push is NOT dropped — it wakes the loop, which then polls the axis.
    // NOTE: SDL may invoke the event filter from a background thread for some backends; keep the
    // override free of non-thread-safe state (it reads only the deadzone, a benign float read).
    virtual bool shouldDropAmbientEvent(const SDL_Event &event) const { return false; }

    virtual float fontSizeBase() const { return 0.f; }

    // Fired from beginFrame when the display content scale (DPI) changes at runtime — e.g. the window is
    // dragged to a monitor at a different scaling. The style has already been re-scaled to `newScale`
    // (FontScaleDpi + ScaleAllSizes) by the time this runs. Default: nothing. OfsApp overrides it to
    // rebuild the DPI-derived default dock layout so panel sizes track the new scale without a restart.
    virtual void onDisplayScaleChanged(float newScale) {}

    // UI frame-rate cap in FPS (0 = unlimited / full refresh). Realized tear-free as an integer
    // swap-interval divisor of the display refresh in updateSwapInterval().
    virtual int frameCapFps() const { return 0; }

    // Merge the CJK glyph font into the default font. Split out of loadFonts() because inflating it from
    // the asset archive (~68 ms) is pure waste for a Latin-script UI. Idempotent: OfsApp calls it eagerly
    // when the UI language is CJK, otherwise from onStartupComplete() once the window is on screen.
    void loadCjkFont();

    std::unique_ptr<Window> window;
    bool running = true;
    FpsIdling fpsIdling;

  private:
    void initImGui();

    // Re-capture the just-applied (unscaled, themed) style as the DPI base and re-scale to the
    // current DPI. Invoked at the tail of every theme::apply() via the post-apply hook.
    void onThemeApplied();

    // Re-scale the imnodes geometry that should scale from the unscaled themed base. Counterpart to
    // ScaleAllSizes for ImGui; reads themedNodesUnscaled / hasThemedNodes.
    void applyDpiToImNodes(float scale);

    static void loadFonts();
    bool cjkFontLoaded_ = false;

    static void shutdownImGui();

    void beginFrame();

    ImGuiStyle defaultStyle;
    ImNodesStyle themedNodesUnscaled; // the themed imnodes style at 1× DPI; base for DPI re-scaling
    bool hasThemedNodes = false;      // set once onThemeApplied runs with an imnodes context
    float currentDpiScale = 0.f;

    void endFrame();

    // Re-derive the present pacing from the frame cap each frame and apply it only on change. The cap
    // (frameCapFps) is a divisor of the current display's refresh, so a non-zero cap maps to an integer
    // divisor N≥2; with no cap it stays 1 (full refresh). On Windows we present with
    // swap interval 0 and block on DwmFlush instead of the driver's vsync — the latter busy-waits a core
    // under NVIDIA "Threaded Optimization". Other platforms (the #ifdef _WIN32 block below is absent
    // there) use driver vsync (swap interval N).
    void updateSwapInterval();
    int appliedSwapInterval = 1;         // last value passed to SDL_GL_SetSwapInterval (1 set in Window::init)
    bool multiVblankUnsupported = false; // set once a >1 interval is refused, so we stop retrying it
#ifdef _WIN32
    // Windows-only compositor (DwmFlush) pacing — see updateSwapInterval / endFrame.
    bool dwmPacing = false;            // this frame is paced by DwmFlush (interval 0)
    bool dwmPacingUnavailable = false; // latched on if DwmFlush ever fails, so we stop retrying it
    int vblankWaitCount = 1;           // DwmFlush calls per present == refresh divisor N (when dwmPacing)
#endif

    // Runs one complete frame (update + render + present). Factored out of run() so it can also be
    // driven from onSizeMoveEventWatch during the Win32 modal resize/move loop, which otherwise
    // blocks the main loop and freezes the content until the mouse is released.
    void renderFrame();

    // SDL event watch: fires synchronously while SDL_PollEvent is blocked inside the OS resize/move
    // loop, so we can keep rendering. Re-renders on window resize/expose events.
    static bool SDLCALL onSizeMoveEventWatch(void *userdata, SDL_Event *event);

    // SDL event filter (SDL_SetEventFilter): returns false to drop ambient controller noise before it
    // enters the queue, so the idle wait isn't woken by it. Forwards to shouldDropAmbientEvent.
    static bool SDLCALL eventFilter(void *userdata, SDL_Event *event);

    void idleBySleeping();

    uint64_t lastFrameTicks = 0;
    bool renderingFrame = false; // reentrancy guard for renderFrame()
};
} // namespace ofs
