#include "Window.h"
#include "Platform/Headless.h"
#include "Util/Log.h"
#include <algorithm>
#include <glad/gl.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <dwmapi.h>   // DwmSetWindowAttribute for Win11 rounded corners
#include <shellapi.h> // SHAppBarMessage for auto-hide taskbar detection
#include <windows.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#endif

namespace ofs {
// Smallest window we ever create or restore to (also the SDL minimum size). A persisted geometry from a
// larger display is clamped up to this floor so a degenerate saved size can't produce an unusable window.
constexpr int kMinWindowWidth = 640;
constexpr int kMinWindowHeight = 480;

#ifdef _WIN32
namespace {
// Property key under which we stash SDL's original WndProc when subclassing the borderless window.
const wchar_t *kPrevWndProcProp = L"OfsPrevWndProc";

// Px to leave uncovered on a maximized window's taskbar edge so an auto-hide taskbar can re-trigger.
// Matches Chromium's kAutoHideTaskbarThicknessPx.
constexpr int kAutoHideTaskbarThicknessPx = 1;

// Returns the screen edge (ABE_*) of an auto-hide taskbar on the given monitor, or -1 if none.
// An auto-hide taskbar reports its work area (rcWork) as the full monitor, so a borderless window
// maximized to fill it covers the taskbar's trigger edge. Worse, a window whose rect exactly covers
// the monitor reads to the shell as a full-screen app, which suppresses the auto-hide pop entirely.
// Shrinking the maximized window by 1px on that edge (see WM_GETMINMAXINFO) both frees the trigger
// line and keeps the window sub-monitor so the bar still re-appears.
int autoHideTaskbarEdge(const RECT &monitorRect) {
    APPBARDATA state = {};
    state.cbSize = sizeof(state);
    if (!(SHAppBarMessage(ABM_GETSTATE, &state) & ABS_AUTOHIDE))
        return -1; // no auto-hide taskbar anywhere; the work-area clamp already leaves room

    for (UINT edge : {ABE_BOTTOM, ABE_TOP, ABE_LEFT, ABE_RIGHT}) {
        APPBARDATA query = {};
        query.cbSize = sizeof(query);
        query.uEdge = edge;
        query.rc = monitorRect; // restrict the query to this monitor
        if (SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &query))
            return static_cast<int>(edge);
    }

    // ABM_GETAUTOHIDEBAREX often returns NULL even when an auto-hide bar is present (it is picky about
    // the monitor rect, and multi-monitor setups confuse it). Fall back to the primary taskbar's own
    // reported position — it carries the edge directly — provided that bar actually sits on this
    // monitor. Covers the common single-taskbar case the EX query misses.
    APPBARDATA pos = {};
    pos.cbSize = sizeof(pos);
    if (SHAppBarMessage(ABM_GETTASKBARPOS, &pos)) {
        const bool onThisMonitor = pos.rc.left < monitorRect.right && pos.rc.right > monitorRect.left &&
                                   pos.rc.top < monitorRect.bottom && pos.rc.bottom > monitorRect.top;
        if (onThisMonitor)
            return static_cast<int>(pos.uEdge);
    }
    return -1;
}

// Shave kAutoHideTaskbarThicknessPx off a maximized extent (width, height by ref) on the dimension
// *perpendicular* to an auto-hide taskbar, so the window no longer covers the monitor fully and the
// shell's full-screen detection can't bury the bar. No-op when the extent is already sub-monitor or no
// auto-hide bar sits on this monitor — see borderlessWndProc for why a perpendicular 1px is the trim.
void trimSizeForAutoHide(const RECT &monitorRect, int &width, int &height) {
    const int monW = monitorRect.right - monitorRect.left;
    const int monH = monitorRect.bottom - monitorRect.top;
    if (width < monW || height < monH)
        return; // only when we'd cover the whole monitor
    switch (autoHideTaskbarEdge(monitorRect)) {
    case ABE_TOP:
    case ABE_BOTTOM:
        width -= kAutoHideTaskbarThicknessPx; // horizontal bar: shrink the width
        break;
    case ABE_LEFT:
    case ABE_RIGHT:
        height -= kAutoHideTaskbarThicknessPx; // vertical bar: shrink the height
        break;
    default:
        break; // no auto-hide taskbar on this monitor
    }
}

// Subclass proc for the borderless main window. Three maximize fix-ups, each chaining to SDL first:
//   WM_GETMINMAXINFO     — SDL reports the maximized size as the *full* monitor (SM_CXSCREEN/SM_CYSCREEN
//                          in SDL_windowsevents.c); we clamp it to the work area so a normal (reserved)
//                          taskbar stays visible instead of being covered by an undrawn black strip.
//   WM_WINDOWPOSCHANGING — under an auto-hide taskbar, trims the maximized *window* rect 1px on the edge
//                          perpendicular to the bar so we never cover the monitor fully (full cover
//                          makes the shell bury the auto-hide bar behind us).
//   WM_NCCALCSIZE        — trims the maximized *client* rect the same 1px; SDL otherwise pins it to the
//                          full monitor, and that presented surface is what the fullscreen path checks.
// Every maximize path — caption button, double-click, edge-snap, Win+Up — routes through these.
LRESULT CALLBACK borderlessWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *prev = reinterpret_cast<WNDPROC>(GetPropW(hwnd, kPrevWndProcProp));
    const auto forward = [&] {
        return prev ? CallWindowProcW(prev, hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
    };

    if (msg == WM_GETMINMAXINFO) {
        const LRESULT res = forward(); // SDL populates min-track size and its (full-screen) max
        if (HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)) {
            MONITORINFO mi = {};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfo(mon, &mi)) {
                auto *mmi = reinterpret_cast<MINMAXINFO *>(lParam);
                // ptMaxPosition = work-area origin relative to the monitor (taskbar may be on any
                // edge); ptMaxSize = work-area extent. Windows uses min(ptMaxSize, max-track), so
                // SDL's max-track size still bounds manual resize across monitors.
                mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
                mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
                mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
                mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
            }
        }
        return res;
    }

    // A maximized borderless window that covers the monitor fully trips the shell's full-screen
    // detection, which demotes an auto-hide taskbar to the bottom of the z-order — it slides out behind
    // us and can never be reached. Shrinking the window a hair off full-cover defeats that detection.
    // ptMaxSize can't do it: SDL re-pins the maximized client rect to the full work area in its own
    // WM_NCCALCSIZE (and rcWork == the whole monitor when the taskbar auto-hides), so SDL wins. The
    // window *placement* is authoritative here though — SDL leaves WINDOWPOS untouched while maximizing
    // (expected_resize path) — so we trim it. Gated on a full-monitor maximize, so the SHAppBarMessage
    // probe only runs at that moment.
    if (msg == WM_WINDOWPOSCHANGING) {
        const LRESULT res = forward();
        auto *wp = reinterpret_cast<WINDOWPOS *>(lParam);
        if (!(wp->flags & SWP_NOSIZE) && IsZoomed(hwnd)) {
            if (HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)) {
                MONITORINFO mi = {};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfo(mon, &mi)) {
                    // Trim a 1px sliver off an edge *perpendicular* to the taskbar. Windows reserves a 1px
                    // line at the auto-hide bar's own edge, so shrinking there still reads as a full
                    // work-area cover and FSO stays on (verified: bottom=mon-1 was still fullscreen). A
                    // perpendicular edge has no reservation, so 1px there genuinely breaks the full-cover
                    // test while leaving the taskbar's edge fully spanned — the bar still triggers.
                    trimSizeForAutoHide(mi.rcMonitor, wp->cx, wp->cy);
                }
            }
        }
        return res;
    }

    // SDL pins a maximized borderless window's *client* rect to the full work area in its own
    // WM_NCCALCSIZE (== the whole monitor under an auto-hide taskbar). That client surface is what DWM
    // presents, and what the fullscreen-optimizations path keys off — so trimming the window rect alone
    // (above) doesn't help; SDL keeps the presented surface full-monitor. Chain after SDL and shrink the
    // client by the same 1px on the same perpendicular edge so the surface is genuinely sub-monitor.
    if (msg == WM_NCCALCSIZE && wParam == TRUE) {
        const LRESULT res = forward();
        if (IsZoomed(hwnd)) {
            if (HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)) {
                MONITORINFO mi = {};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfo(mon, &mi)) {
                    auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(lParam);
                    RECT &client = params->rgrc[0];
                    int w = client.right - client.left;
                    int h = client.bottom - client.top;
                    trimSizeForAutoHide(mi.rcMonitor, w, h);
                    client.right = client.left + w;
                    client.bottom = client.top + h;
                }
            }
        }
        return res;
    }
    return forward();
}
} // namespace
#endif

Window::Window(WindowConfig config) : config(std::move(config)) {}

Window::~Window() {
    if (glContext) {
        SDL_GL_DestroyContext(glContext);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
}

bool Window::init() {
    if constexpr (ofs::kHeadless) {
        // Select SDL's "dummy" video driver: a windowless backend that needs no X/Wayland server and
        // creates no GL — so the ui-tests run on a bare CI box with no display and no Mesa/EGL. The app
        // still gets a real SDL_Window (for ImGui's DisplaySize, input and timing); it just never
        // renders. Every GL-touching code path is gated on ofs::kHeadless (see Platform/Headless.h).
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    }

    // Gamepad is *not* initialized here: SDL_INIT_GAMEPAD enumerates HID/XInput devices and parses the
    // controller-mapping DB, ~360 ms on Windows — the single largest startup cost. It contributes nothing
    // to the first frame, so it is deferred to initGamepadSubsystem() once the window is on screen.
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        OFS_CORE_ERROR("SDL_Init Error: {}", SDL_GetError());
        return false;
    }

    if constexpr (ofs::kHeadless) {
        // No GL attributes, no SDL_WINDOW_OPENGL, no GL context, no glad loader. A plain hidden window
        // of the target size is all ImGui needs for its DisplaySize; the rest of init() is GL setup.
        SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
        window = SDL_CreateWindow(config.title.c_str(), config.width, config.height, SDL_WINDOW_HIDDEN);
        if (!window) {
            OFS_CORE_ERROR("SDL_CreateWindow Error: {}", SDL_GetError());
            return false;
        }
        SDL_SetWindowMinimumSize(window, kMinWindowWidth, kMinWindowHeight);
        return true;
    }

    // Closing the last window (Alt+F4, taskbar "Close Window") otherwise makes SDL auto-post
    // SDL_EVENT_QUIT alongside SDL_EVENT_WINDOW_CLOSE_REQUESTED, which would tear the loop down
    // immediately and skip the unsaved-changes prompt. Suppress it so the app routes the close
    // through its own guarded-exit flow (onEvent -> RequestExitEvent); the flow calls stop() once
    // the user has had a chance to save.
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");

#ifndef NDEBUG
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
#endif
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 2);

    int targetWidth = config.width;
    int targetHeight = config.height;
    bool shouldMaximize = false;

    // Size against the *usable* bounds (work area minus taskbar/dock), not the full display, so the
    // default window never spills under the taskbar or off-screen on a small/laptop screen — the
    // common case being a 1080p panel whose work area is ~1920x1032.
    //
    // We keep a margin between a floating window and the work-area edges (kEdgeMarginDiv). It reads as
    // a window rather than a baked-on full-screen surface, and — critically — keeps the floating/restore
    // size strictly smaller than the monitor: a borderless window that *exactly* covers the monitor
    // (preferred size == work area, e.g. a 1920x1080 panel with an auto-hide taskbar) is created
    // restored, not maximized, and so slips past the maximize-only fix-ups in borderlessWndProc (gated
    // on IsZoomed) that keep an auto-hide taskbar reachable and fullscreen-optimizations off.
    //
    // When the preferred size doesn't fit within that margin the screen is small relative to our
    // window (a 1080p/laptop panel), so we clamp the restore size to the margined bounds and start
    // maximized — filling the work area is the right call there, and the margined restore size keeps
    // un-maximize from landing on a full-cover restored window.
    SDL_Rect usable;
    const bool haveUsable = SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);
    if (haveUsable) {
        constexpr int kEdgeMarginDiv = 16; // per-axis inset as a fraction of the work area (~6%)
        const int fitWidth = usable.w - usable.w / kEdgeMarginDiv;
        const int fitHeight = usable.h - usable.h / kEdgeMarginDiv;
        if (targetWidth > fitWidth || targetHeight > fitHeight)
            shouldMaximize = true;
        targetWidth = std::min(targetWidth, fitWidth);
        targetHeight = std::min(targetHeight, fitHeight);
    }

    // Born borderless and hidden. We always run a custom in-app title bar (enableCustomTitleBar), so
    // creating the window already-borderless means the native caption never flashes on screen during
    // the rest of init. Hidden until the first frame is painted and swapped (see Window::show, called
    // from Application::run) so the OS's default white window background never flashes either.
    SDL_WindowFlags windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                  SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIDDEN;
    if (shouldMaximize) {
        windowFlags |= SDL_WINDOW_MAXIMIZED;
    }

    window = SDL_CreateWindow(config.title.c_str(), targetWidth, targetHeight, windowFlags);
    if (!window) {
        OFS_CORE_ERROR("SDL_CreateWindow Error: {}", SDL_GetError());
        return false;
    }

    SDL_SetWindowMinimumSize(window, kMinWindowWidth, kMinWindowHeight);

    // Seed the persisted restore rect with the chosen size, centered on the work area. If the window is
    // born maximized, this is the geometry it returns to on un-maximize and the size it persists if the
    // user never restores it; restoreGeometry() overwrites it when a saved geometry is applied.
    restoreGeom_.width = targetWidth;
    restoreGeom_.height = targetHeight;
    if (haveUsable) {
        restoreGeom_.x = usable.x + (usable.w - targetWidth) / 2;
        restoreGeom_.y = usable.y + (usable.h - targetHeight) / 2;
    }

    glContext = SDL_GL_CreateContext(window);
    if (!glContext) {
        OFS_CORE_ERROR("SDL_GL_CreateContext Error: {}", SDL_GetError());
        return false;
    }

    SDL_GL_MakeCurrent(window, glContext);
    SDL_GL_SetSwapInterval(1); // Enable vsync

    int version = gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress);
    if (version == 0) {
        OFS_CORE_ERROR("Failed to initialize OpenGL context");
        return false;
    }

    OFS_CORE_INFO("Loaded OpenGL {}.{}", GLAD_VERSION_MAJOR(version), GLAD_VERSION_MINOR(version));

#ifndef NDEBUG
    auto glDebugCallback = [](GLenum, GLenum type, GLuint, GLenum severity, GLsizei, const GLchar *message,
                              const void *) {
        switch (severity) {
        case GL_DEBUG_SEVERITY_HIGH:
            OFS_CORE_ERROR("[GL] {}", message);
            break;
        case GL_DEBUG_SEVERITY_MEDIUM:
            OFS_CORE_WARN("[GL] {}", message);
            break;
        case GL_DEBUG_SEVERITY_LOW:
            OFS_CORE_INFO("[GL] {}", message);
            break;
        default:
            break; // ignore notifications
        }
    };
    if (GLAD_GL_KHR_debug) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(glDebugCallback, nullptr);
    } else if (GLAD_GL_ARB_debug_output) {
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_ARB);
        glDebugMessageCallbackARB(glDebugCallback, nullptr);
    }
#endif

    glEnable(GL_MULTISAMPLE);

    return true;
}

void Window::initGamepadSubsystem() {
    if (SDL_WasInit(SDL_INIT_GAMEPAD))
        return;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        OFS_CORE_WARN("SDL_InitSubSystem(GAMEPAD) failed: {}", SDL_GetError());
}

void Window::swapBuffers() {
    if constexpr (ofs::kHeadless) // no GL context / surface to present under the null backend
        return;
    SDL_GL_SwapWindow(window);
}

#ifdef _WIN32
bool Window::compositorPacingAvailable() const {
    if constexpr (ofs::kHeadless)
        return false;
    return true; // DWM composition is always on for windowed apps on Win8+
}

bool Window::waitForCompositorVBlank(int count) {
    if constexpr (ofs::kHeadless)
        return false;
    // DwmFlush blocks until the compositor's next vblank — a real wait that parks the thread, unlike the
    // driver's vsync spin under threaded optimization. N flushes == "present every Nth vblank" (interval N).
    for (int i = 0; i < count; ++i) {
        if (FAILED(DwmFlush()))
            return false; // composition disabled; caller falls back to driver vsync
    }
    return true;
}
#endif // _WIN32

void Window::show() {
    if (window)
        SDL_ShowWindow(window);
}

void Window::pollEvents(bool &running, const std::function<void(SDL_Event *)> &eventHandler) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (eventHandler) {
            eventHandler(&event);
        }

        // SDL_EVENT_QUIT is a hard fallback (OS-level termination). Normal window closes no longer
        // reach it — SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE is disabled in init() — so this won't fire
        // on Alt+F4 / taskbar close; those arrive as SDL_EVENT_WINDOW_CLOSE_REQUESTED and are routed
        // by the app (onEvent) through the guarded-exit flow instead of quitting here.
        if (event.type == SDL_EVENT_QUIT) {
            running = false;
        }

        // SDL's fullscreen transition restyles the borderless window, which makes DWM drop the
        // rounded-corner opt-in. Square it off while fullscreen (edge-to-edge), round it again on
        // return. These events fire after the restyle has landed, so the preference sticks.
        if (event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN)
            applyRoundedCorners(false);
        else if (event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN)
            applyRoundedCorners(true);

        // Keep the persisted restore rect current as the user moves/resizes the window. updateRestore-
        // Geometry ignores the maximized/fullscreen states, so a maximize doesn't overwrite the normal
        // rect we want to return to.
        if (event.type == SDL_EVENT_WINDOW_MOVED || event.type == SDL_EVENT_WINDOW_RESIZED)
            updateRestoreGeometry();
    }
}

void Window::waitEvents(int timeoutMs) {
    SDL_WaitEventTimeout(nullptr, timeoutMs);
}

int Window::getWidth() const {
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    return w;
}

int Window::getHeight() const {
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    return h;
}

SDL_HitTestResult SDLCALL Window::hitTest(SDL_Window *win, const SDL_Point *area, void *data) {
    const auto *info = static_cast<const TitleBarHitInfo *>(data);
    if (!info || !info->enabled)
        return SDL_HITTEST_NORMAL;

    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);

    constexpr int kBorder = 6; // edge-resize thickness, window points
    const int x = area->x;
    const int y = area->y;

    if (!info->maximized) {
        const bool left = x < kBorder;
        const bool right = x >= w - kBorder;
        const bool top = y < kBorder;
        const bool bottom = y >= h - kBorder;
        if (top && left)
            return SDL_HITTEST_RESIZE_TOPLEFT;
        if (top && right)
            return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (bottom && left)
            return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (bottom && right)
            return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (left)
            return SDL_HITTEST_RESIZE_LEFT;
        if (right)
            return SDL_HITTEST_RESIZE_RIGHT;
        if (top)
            return SDL_HITTEST_RESIZE_TOP;
        if (bottom)
            return SDL_HITTEST_RESIZE_BOTTOM;
    }

    // Interactive carve-out (e.g. the command-palette search box): stays normal client area so ImGui
    // receives the click instead of the OS starting a window drag.
    if (y < static_cast<int>(info->height) && info->searchRightX > info->searchLeftX &&
        x >= static_cast<int>(info->searchLeftX) && x < static_cast<int>(info->searchRightX))
        return SDL_HITTEST_NORMAL;

    // Title bar strip, left of the caption buttons, is the drag handle. The OS maps this to a caption,
    // giving move + double-click-maximize + snap for free.
    if (y < static_cast<int>(info->height) && x < static_cast<int>(info->buttonsLeftX))
        return SDL_HITTEST_DRAGGABLE;

    return SDL_HITTEST_NORMAL;
}

void Window::enableCustomTitleBar() {
    if (!window)
        return;
    if constexpr (ofs::kHeadless) // dummy-driver window has no native chrome / HWND to subclass
        return;

    SDL_SetWindowBordered(window, false);

    titleBarHit.enabled = true;
    if (!SDL_SetWindowHitTest(window, &Window::hitTest, &titleBarHit)) {
        OFS_CORE_ERROR("SDL_SetWindowHitTest failed: {}", SDL_GetError());
        titleBarHit.enabled = false;
        SDL_SetWindowBordered(window, true); // restore native chrome so the window stays usable
        return;
    }

#ifdef _WIN32
    // Subclass the window so a maximized borderless window clamps to the work area instead of
    // covering the taskbar (see borderlessWndProc). Stash SDL's proc so we can chain to it.
    if (auto *hwnd = static_cast<HWND>(
            SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr))) {
        auto *prev = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&borderlessWndProc)));
        SetPropW(hwnd, kPrevWndProcProp, reinterpret_cast<HANDLE>(prev));
    }
#endif
    applyRoundedCorners(true);
}

void Window::setTitleBarLayout(float height, float buttonsLeftX, float searchLeftX, float searchRightX) {
    titleBarHit.height = height;
    titleBarHit.buttonsLeftX = buttonsLeftX;
    titleBarHit.searchLeftX = searchLeftX;
    titleBarHit.searchRightX = searchRightX;
    titleBarHit.maximized = isMaximized();
}

void Window::minimize() {
    if (window)
        SDL_MinimizeWindow(window);
}

void Window::toggleMaximize() {
    if (!window)
        return;
    if (isMaximized())
        SDL_RestoreWindow(window);
    else
        SDL_MaximizeWindow(window);
}

bool Window::isMaximized() const {
    return window && (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
}

void Window::toggleFullscreen() {
    if (!window)
        return;
    // Don't touch the corner preference here: SDL3 applies the fullscreen restyle asynchronously, so
    // any DWM attribute we set now would be clobbered by it. We re-assert it on the LEAVE_FULLSCREEN
    // event (pollEvents) instead, which SDL posts after the window has actually changed state.
    SDL_SetWindowFullscreen(window, !isFullscreen());
}

bool Window::isFullscreen() const {
    return window && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
}

void Window::restoreGeometry(const Geometry &g) {
    if constexpr (ofs::kHeadless) // the hidden dummy window keeps its created size; tests stay deterministic
        return;
    if (!window || g.width <= 0 || g.height <= 0)
        return; // unset / degenerate → leave the start-size heuristic result in place

    int w = g.width;
    int h = g.height;
    SDL_Rect want{.x = g.x, .y = g.y, .w = w, .h = h};
    SDL_DisplayID disp = SDL_GetDisplayForRect(&want); // display with the largest overlap, or nearest
    if (!disp)
        disp = SDL_GetPrimaryDisplay();

    SDL_Rect usable;
    if (!SDL_GetDisplayUsableBounds(disp, &usable)) {
        SDL_SetWindowSize(window, std::max(w, kMinWindowWidth), std::max(h, kMinWindowHeight));
        restoreGeom_ = {.x = g.x, .y = g.y, .width = w, .height = h, .maximized = false};
    } else {
        w = std::clamp(w, kMinWindowWidth, usable.w); // never larger than the target monitor's work area
        h = std::clamp(h, kMinWindowHeight, usable.h);

        // Require a grabbable chunk of the window to overlap the usable area. If it doesn't, the saved
        // rect belongs to a now-absent monitor (or sits off a shrunk desktop), so re-center it here —
        // otherwise the title bar would be unreachable and the window effectively lost.
        constexpr int kMinVisible = 64; // logical px that must remain on-screen on each axis
        SDL_Rect rect{.x = g.x, .y = g.y, .w = w, .h = h};
        SDL_Rect inter;
        if (!SDL_GetRectIntersection(&rect, &usable, &inter) || inter.w < kMinVisible || inter.h < kMinVisible) {
            rect.x = usable.x + (usable.w - w) / 2;
            rect.y = usable.y + (usable.h - h) / 2;
        }
        SDL_SetWindowPosition(window, rect.x, rect.y);
        SDL_SetWindowSize(window, w, h);
        restoreGeom_ = {.x = rect.x, .y = rect.y, .width = w, .height = h, .maximized = false};
    }

    if (g.maximized) // the validated restore rect is set first, so un-maximize returns to a sane place
        SDL_MaximizeWindow(window);
}

Window::Geometry Window::currentRestoreGeometry() const {
    Geometry g = restoreGeom_;
    g.maximized = isMaximized();
    return g;
}

void Window::updateRestoreGeometry() {
    if constexpr (ofs::kHeadless)
        return;
    if (!window || isMaximized() || isFullscreen())
        return; // keep the last normal rect while zoomed/fullscreen so that is what we persist
    SDL_GetWindowPosition(window, &restoreGeom_.x, &restoreGeom_.y);
    SDL_GetWindowSize(window, &restoreGeom_.width, &restoreGeom_.height);
}

void Window::applyRoundedCorners(bool rounded) {
#ifdef _WIN32
    // Win11 (build 22000+) rounds framed windows automatically, but a borderless window must opt in
    // explicitly. Harmless / silently ignored on Windows 10 and earlier.
    if (auto *hwnd = static_cast<HWND>(
            SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr))) {
        const DWM_WINDOW_CORNER_PREFERENCE corner = rounded ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    }
#else
    (void)rounded;
#endif
}
} // namespace ofs
