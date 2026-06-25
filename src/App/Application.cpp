#include "Application.h"
#include "Platform/Headless.h"
#include "Platform/imgui_impl_null.h"
#include "Platform/imgui_impl_opengl3.h"
#include "Platform/imgui_impl_sdl3.h"
#include "UI/Heatmap.h"
#include "UI/ImGuiHelpers.h"
#include "UI/Theme.h"
#include "Util/FrameAllocator.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"
#include "Util/Resources.h"
#include "Util/Version.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <glad/gl.h>
#include <imgui.h>
#include <imnodes.h>
#include <spdlog/fmt/fmt.h>
#include <string>

#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <crtdbg.h>
#include <cstdlib>
#endif

namespace ofs {

static double clockSeconds() {
    static const auto start = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - start;
    return elapsed.count();
}

// The themed imnodes style at 1× DPI. Captured by onThemeApplied() (which runs at
// the tail of every theme::apply via the post-apply hook) so a later DPI change can
// re-scale from the unscaled themed base — mirroring how defaultStyle works for ImGui.
static ImNodesStyle gThemedNodesUnscaled;
static bool gHasThemedNodes = false;

// Apply DPI scaling to the imnodes geometry that should scale. NodeCornerRounding is
// identity (a theme/identity choice), so it is intentionally not scaled. Colors and the
// remaining geometry are copied from the unscaled themed base verbatim.
static void applyDpiToImNodes(float scale) {
    if (!gHasThemedNodes || ImNodes::GetCurrentContext() == nullptr)
        return;
    ImNodesStyle ns = gThemedNodesUnscaled;
    ns.GridSpacing *= scale;
    ns.NodePadding.x *= scale;
    ns.NodePadding.y *= scale;
    ns.NodeBorderThickness *= scale;
    ns.LinkThickness *= scale;
    ns.PinCircleRadius *= scale;
    ImNodes::GetStyle() = ns;
}

// Route CRT assertion/abort output to stderr instead of a modal message box. IM_ASSERT
// expands to the C `assert()`, which on the Windows GUI subsystem pops a dialog that
// hangs a headless run (and is easy to miss interactively). Sending it to stderr makes
// failed assertions show up in standard output everywhere — app, CI, and UI tests.
// Process-global and idempotent; safe to call once at startup.
static void routeAssertsToStderr() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR); // assert() text -> stderr, not a message box
    // abort(): keep the message (to stderr) but suppress the Windows Error Reporting box.
    _set_abort_behavior(_WRITE_ABORT_MSG, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    for (int report : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(report, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
    }
#endif
}

Application::Application() = default;

Application::~Application() {
    SDL_RemoveEventWatch(onSizeMoveEventWatch, this);
    shutdownImGui();
}

bool Application::init() {
    routeAssertsToStderr();

    // Title carries the build's git identity; the welcome-screen header reuses the same string.
    WindowConfig windowConfig;
    windowConfig.title = versionTitle();
    window = std::make_unique<Window>(std::move(windowConfig));
    if (!window->init()) {
        return false;
    }

    initImGui();
    return true;
}

void Application::initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Arrow-key directional nav + Enter/Space to activate, so menus, combos, the command palette and
    // modal dialogs are selectable from the keyboard. This makes ImGui claim the unmodified arrow/Space
    // keys (raising io.WantCaptureKeyboard) whenever a navigable window is focused, which is why the
    // editor's own arrow/Space shortcuts yield to nav while a panel has focus (see BindingSystem's
    // keyboardCaptured gate).
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Tab-cycling and Ctrl+Tab window-switching are active regardless of NavEnableKeyboard and
    // conflict with the app's own shortcut system, so disable them both.
    ImGuiContext &g = *ImGui::GetCurrentContext();
    g.ConfigNavEnableTabbing = false;
    g.ConfigNavWindowingKeyNext = ImGuiKey_None;
    g.ConfigNavWindowingKeyPrev = ImGuiKey_None;

    // Disable ImGui's automatic imgui.ini. Docking-layout persistence is owned by the app's layout
    // system (layouts.json): ImGui's auto-save/load would otherwise fight it — restoring stale window
    // positions on launch and making the "active layout" diverge from what's on screen.
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();

    float dpiScale = SDL_GetWindowDisplayScale(window->getNativeWindow());
    if (dpiScale <= 0.f) {
        dpiScale = 1.f;
    }

    defaultStyle = ImGui::GetStyle();
    ImGuiStyle &style = ImGui::GetStyle();
    style.FontScaleDpi = dpiScale;
    style.ScaleAllSizes(dpiScale);
    currentDpiScale = dpiScale;

    loadFonts();

    if constexpr (ofs::kHeadless) {
        // Null renderer: the SDL3 platform backend still drives DisplaySize, input and timing from the
        // dummy-driver window, but no GL renderer is installed (see Platform/imgui_impl_null.h).
        ImGui_ImplSDL3_InitForOther(window->getNativeWindow());
        ImGui_ImplNull_Init();
    } else {
        ImGui_ImplSDL3_InitForOpenGL(window->getNativeWindow(), window->getGLContext());
        ImGui_ImplOpenGL3_Init("#version 330");
    }

    ImNodes::CreateContext();

    // theme::apply() writes unscaled (1×) ImGui + imnodes styles; re-capture them as the
    // DPI base and re-scale on every apply so a later DPI change never wipes the theme.
    ofs::theme::setPostApplyHook([this] { onThemeApplied(); });
}

void Application::onThemeApplied() {
    ImGuiStyle &style = ImGui::GetStyle();
    defaultStyle = style; // themed, unscaled — the base future DPI changes scale from
    style.ScaleAllSizes(currentDpiScale);
    style.FontScaleDpi = currentDpiScale;

    if (ImNodes::GetCurrentContext() != nullptr) {
        gThemedNodesUnscaled = ImNodes::GetStyle(); // apply() just wrote the unscaled themed nodes
        gHasThemedNodes = true;
        applyDpiToImNodes(currentDpiScale);
    }

    Heatmap::rebuildColorLut(); // theme drives the heatmap gradient LUT
}

namespace {

// Fixed reference size that merged-glyph offsets/advances are expressed against.
// ImGui scales a merge source's GlyphOffset by (renderedSize / Sources[0]->SizePixels),
// so the base source must carry a non-zero reference or icon offsets won't track the
// user's font size. This is only an anchor — actual render size is driven each frame by
// style.FontSizeBase (see beginFrame), so this value does not fix the on-screen size.
constexpr float kFontRefSize = 18.0f;

// Loads a font from the assets archive (ofs::res) into the atlas. AddFontFromMemoryTTF takes ownership
// of the buffer and frees it with ImGui's allocator when the atlas is destroyed, so it must be
// IM_ALLOC'd — and it must persist for the atlas lifetime, which ImGui now owns.
ImFont *addFontFromArchive(const char *name, float size, const ImFontConfig *cfg) {
    auto bytes = ofs::res::read(name);
    if (!bytes) {
        OFS_CORE_ERROR("Failed to load font: {}", name);
        return nullptr;
    }
    void *mem = IM_ALLOC(bytes->size());
    std::memcpy(mem, bytes->data(), bytes->size());
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(mem, static_cast<int>(bytes->size()), size, cfg);
}

} // namespace

void Application::loadFonts() {
    ImGuiIO &io = ImGui::GetIO();

    ofs::ui::gFontDefault = addFontFromArchive("data/fonts/JetBrainsMono-Regular.ttf", kFontRefSize, nullptr);

    {
        ImFontConfig cfg;
        cfg.MergeMode = true;
        // Lucide glyphs are centered on a full em box and sit high against the text
        // baseline. Nudge down to vertically center them in buttons/labels. Expressed at
        // kFontRefSize; ImGui scales it with the runtime font size. Tune if it looks off.
        cfg.GlyphOffset.y = 4.0f;
        addFontFromArchive("data/fonts/lucide.ttf", kFontRefSize, &cfg);
    }

    // The CJK glyph font (NotoSansCJKjp, ~68 ms to inflate from the archive) is *not* loaded here. It is
    // merged in later by loadCjkFont() so a Latin-script UI pays nothing for it at startup; ImGui 1.92's
    // dynamic atlas lets the source be added at runtime and rasterizes its glyphs on demand. OfsApp loads
    // it eagerly only when the UI language itself is CJK (see OfsApp::init).
    io.FontDefault = ofs::ui::gFontDefault;
}

void Application::loadCjkFont() {
    if (cjkFontLoaded_ || ofs::ui::gFontDefault == nullptr)
        return;
    cjkFontLoaded_ = true;

    // Merge the CJK source into the existing default font. DstFont targets it explicitly (rather than
    // relying on "merge into the last-added font"), so this is correct regardless of what else has been
    // added to the atlas since startup. The atlas is never Locked in normal operation, and the dynamic
    // texture backend (ImGuiBackendFlags_RendererHasTextures) picks up the new source on the next frame.
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.DstFont = ofs::ui::gFontDefault;
    addFontFromArchive("data/fonts/NotoSansCJKjp-Regular.otf", 0.0f, &cfg);
}

void Application::shutdownImGui() {
    // The destructor calls this unconditionally, but initImGui() runs only after window->init()
    // succeeds. If window creation failed there is no ImGui context (nor ImNodes/backends), and the
    // backend Shutdown() calls would dereference never-initialized state — turning a clean init
    // failure into a crash. A null current context means initImGui() never ran: nothing to tear down
    // (ImNodes is created right after the ImGui context, so it exists iff it does).
    if (ImGui::GetCurrentContext() == nullptr)
        return;
    ImNodes::DestroyContext();
    if constexpr (ofs::kHeadless)
        ImGui_ImplNull_Shutdown();
    else
        ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void Application::beginFrame() {
    float newScale = SDL_GetWindowDisplayScale(window->getNativeWindow());
    if (newScale <= 0.f) {
        newScale = 1.f;
    }
    if (newScale != currentDpiScale) {
        ImGuiStyle &style = ImGui::GetStyle();
        style = defaultStyle;
        style.ScaleAllSizes(newScale);
        style.FontScaleDpi = newScale;
        applyDpiToImNodes(newScale);
        currentDpiScale = newScale;
    }
    if (float fsb = fontSizeBase(); fsb > 0.f) {
        ImGui::GetStyle().FontSizeBase = fsb;
    }

    FrameAllocator::instance().reset();
    if constexpr (ofs::kHeadless)
        ImGui_ImplNull_NewFrame();
    else
        ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void Application::endFrame() {
    ImGui::Render();

    if constexpr (ofs::kHeadless) {
        ImGui_ImplNull_RenderDrawData(ImGui::GetDrawData());
        return;
    }

    glViewport(0, 0, window->getWidth(), window->getHeight());
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        SDL_Window *backupCurrentWindow = SDL_GL_GetCurrentWindow();
        SDL_GLContext backupCurrentContext = SDL_GL_GetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        SDL_GL_MakeCurrent(backupCurrentWindow, backupCurrentContext);
    }

    // After the (optional) secondary-viewport pass restores the main context: the imgui SDL3 backend
    // sets interval 0 on each platform window's own context, so the interval must be (re)asserted on
    // the main context here, just before its swap.
    updateSwapInterval();
#ifdef _WIN32
    if (dwmPacing && !window->waitForCompositorVBlank(vblankWaitCount)) {
        // Composition was disabled mid-session (rare). Stop pacing on it and let updateSwapInterval
        // re-assert driver vsync from the next frame on.
        dwmPacing = false;
        dwmPacingUnavailable = true;
    }
#endif
    window->swapBuffers();
}

void Application::updateSwapInterval() {
#ifdef OFS_TEST_ENGINE
    // ui-tests drive frames programmatically through imgui_test_engine; there is no display
    // to tear and no human watching, so the loop should run as fast as the GPU allows. Vsync (or, worse,
    // the UI fps cap's multi-vblank interval — interval 2 pins a 60 Hz display at 30 FPS) would throttle
    // the whole headless run. Force the swap unthrottled and skip the cap logic entirely. (The real app
    // build never defines OFS_TEST_ENGINE, so its pacing is untouched.)
    if (appliedSwapInterval != 0 && SDL_GL_SetSwapInterval(0))
        appliedSwapInterval = 0;
    return;
#else
    int desired = 1; // full refresh (vsync) — also the playback and unlimited case
    const int cap = frameCapFps();
    if (cap > 0 && !isVideoPlaybackActive() && !multiVblankUnsupported) {
        float refresh = 0.f;
        if (const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window->getNativeWindow())))
            refresh = mode->refresh_rate;
        if (refresh > 1.f) {
            // Present every Nth vblank. The cap is a refresh divisor (the preset list guarantees it),
            // so this rounds to an exact N; lround keeps it sane on an odd-refresh display. Clamp to a
            // safe upper bound so a hand-edited tiny maxFps can't ask for an absurd interval.
            const int n = static_cast<int>(std::lround(refresh / static_cast<float>(cap)));
            desired = std::clamp(n, 1, 8);
        }
    }

#ifdef _WIN32
    // Preferred path (Windows only): let the desktop compositor pace us. Present with swap interval 0
    // (SwapBuffers returns immediately, never spinning) and block on DwmFlush in endFrame instead — a
    // real wait that parks the thread, which the driver's vsync fails to do under NVIDIA "Threaded
    // Optimization" (it burns a full core). The cap's refresh divisor N becomes the DwmFlush count.
    if (!dwmPacingUnavailable && window->compositorPacingAvailable()) {
        vblankWaitCount = desired;
        if (appliedSwapInterval != 0 && SDL_GL_SetSwapInterval(0))
            appliedSwapInterval = 0;
        if (appliedSwapInterval == 0) {
            dwmPacing = true;
            return;
        }
        dwmPacingUnavailable = true; // interval 0 refused — give up on compositor pacing for this session
    }
    dwmPacing = false; // fall through to driver vsync below
#endif

    // Driver vsync: present every Nth vblank.
    if (desired == appliedSwapInterval)
        return;

    // SDL_GL_SetSwapInterval only contracts for 0/1/-1; >1 ("present every Nth vblank") is an optional
    // driver feature (WGL_EXT_swap_control) that can be refused. We only ever pass 1 or a clamped N≥2,
    // never 0 or a negative (adaptive) interval. If a >1 interval is rejected, fall back to plain vsync
    // and latch it off so the failing call isn't retried every frame.
    if (SDL_GL_SetSwapInterval(desired)) {
        appliedSwapInterval = desired;
    } else if (desired > 1) {
        multiVblankUnsupported = true;
        if (SDL_GL_SetSwapInterval(1))
            appliedSwapInterval = 1;
    }
#endif
}

void Application::idleBySleeping() {
    fpsIdling.isIdling = false;

    double now = clockSeconds();
    if (!canAppIdle()) {
        fpsIdling.lastActiveTime = now;
    }

    if (fpsIdling.fpsIdle > 0.0f && fpsIdling.enableIdling && canAppIdle()) {
        if (now - fpsIdling.lastActiveTime < FpsIdling::idleDelay) {
            return;
        }

        double beforeWait = clockSeconds();
        double waitTimeout = 1.0 / static_cast<double>(fpsIdling.fpsIdle);

        window->waitEvents(static_cast<int>(waitTimeout * 1000.0));

        double afterWait = clockSeconds();
        double waitDuration = afterWait - beforeWait;
        double waitIdleExpected = 1.0 / static_cast<double>(fpsIdling.fpsIdle);
        fpsIdling.isIdling = (waitDuration > waitIdleExpected * 0.9);
    }
}

int Application::run() {
    lastFrameTicks = SDL_GetTicks();

    // Registered here, not in init(): the watch re-enters renderFrame() during an OS modal pump. A
    // modal opened in init() (e.g. the plugin trust dialog) would otherwise drive a full frame —
    // draining the event queue and submitting jobs — before init() finishes and the worker pool is
    // started. By run() the app is fully initialized, so re-entrant resize/move frames are safe.
    SDL_AddEventWatch(onSizeMoveEventWatch, this);

    // Drop ambient controller noise (sub-deadzone stick jitter, per-cycle update/sensor markers) before
    // it reaches the queue, so it can't wake the idle wait. See shouldDropAmbientEvent.
    SDL_SetEventFilter(eventFilter, this);

    // The window is created hidden + borderless (Window::init) so neither the native caption nor the
    // OS's blank white background flashes during startup. Paint and swap one full frame, then reveal
    // it — the user's first sight of the window is real content, not a half-initialized flash.
    renderFrame();
    window->show();

    // The first frame was painted while the window was still hidden, so its buffer was never presented
    // to the screen. Paint once more now that the window is visible — this present reaches the compositor
    // (DWM composites it asynchronously), so the user sees real content immediately.
    renderFrame();

    // Heavy, first-frame-irrelevant init — SDL's gamepad subsystem (~250-360 ms of HID/DB enumeration)
    // plus any subclass-owned init (plugin + script host) — is deferred to onStartupComplete(). We do NOT
    // run it here: instead we let the main loop spin a few frames first, then fire it once. The reason is
    // a reopen-on-launch: its project load is async and dispatches the mpv video-load command only after a
    // couple of event-queue drains. Running these blocking inits here would freeze the main thread (no
    // drains) and strand that video-load command behind ~365 ms of work; letting the loop drain first lets
    // mpv start decoding in parallel. The loop is kept hot (no idle wait) until the init has run, so it
    // fires promptly regardless of project size or whether the user interacts.
    constexpr int kDeferredInitAfterFrames = 3;
    bool deferredInitDone = false;
    int framesSinceShown = 0;

#ifdef OFS_TEST_ENGINE
    // The test engine starts advancing queued tests from the first frame, and plugin-dependent UI tests
    // need the plugin host loaded up front. The wiped test environment has no reopen-on-launch, so the
    // video-load overlap this deferral protects doesn't apply — run it now for deterministic tests.
    window->initGamepadSubsystem();
    onStartupComplete();
    deferredInitDone = true;
#endif

    while (running) {
        if (deferredInitDone)
            idleBySleeping();

        window->pollEvents(running, [this](SDL_Event *event) {
            // Ambient controller noise is dropped by eventFilter before it reaches here, so any event we
            // see is genuine activity and resets the idle timer.
            fpsIdling.lastActiveTime = clockSeconds();
            ImGui_ImplSDL3_ProcessEvent(event);
            onEvent(event);
        });

        if (!running) {
            break;
        }

        renderFrame();

        if (!deferredInitDone && ++framesSinceShown >= kDeferredInitAfterFrames) {
            window->initGamepadSubsystem();
            onStartupComplete();
            deferredInitDone = true;
        }
    }
    return 0;
}

void Application::renderFrame() {
    if (renderingFrame) // re-entered from onSizeMoveEventWatch mid-frame; one frame at a time
        return;
    renderingFrame = true;

    uint64_t currentTime = SDL_GetTicks();
    float deltaTime = static_cast<float>(currentTime - lastFrameTicks) / 1000.0f;
    lastFrameTicks = currentTime;

    beginFrame();

    onUpdate(deltaTime);
    onRender();
    renderTitleBar(); // submitted before onImGuiRender so the bar stacks above the menu bar
    onImGuiRender();

    endFrame();
    onPostRender();

    renderingFrame = false;
}

bool SDLCALL Application::onSizeMoveEventWatch(void *userdata, SDL_Event *event) {
    // Fires synchronously while SDL_PollEvent is blocked inside the OS modal resize/move loop. Keep
    // rendering so the content tracks the new size instead of freezing until mouse release.
    if (event->type == SDL_EVENT_WINDOW_EXPOSED || event->type == SDL_EVENT_WINDOW_RESIZED) {
        auto *self = static_cast<Application *>(userdata);
        if (self->running)
            self->renderFrame();
    }
    return true; // leave the event in the queue for normal processing
}

bool SDLCALL Application::eventFilter(void *userdata, SDL_Event *event) {
    auto *self = static_cast<Application *>(userdata);
    return !self->shouldDropAmbientEvent(*event); // false ⇒ drop (never queued, never wakes the wait)
}
} // namespace ofs
