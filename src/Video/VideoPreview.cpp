#include "VideoPreview.h"
#include "MpvLoader.h"
#include "Platform/Headless.h"
#include "Util/Log.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <glad/gl.h>
#include <string>

namespace ofs {
namespace {
// reply_userdata ids for the two properties the preview observes
enum PreviewProp : uint64_t { PreviewWidth, PreviewHeight };

// Cap the preview FBO height; width follows the source aspect. Keeps decode/upload cheap — the
// popup is small, and for VR the equirect only feeds a downscaled projection source.
constexpr int kPreviewMaxHeight = 480;

// Ignore hover jitter smaller than this (seconds) so sub-frame mouse wiggle doesn't re-seek.
constexpr double kSeekEpsilon = 0.04;
} // namespace

VideoPreview::VideoPreview(bool hwdecEnabled, EventQueue &eventQueue)
    : eventQueue(eventQueue), hwdecEnabled(hwdecEnabled) {}

VideoPreview::~VideoPreview() {
    destroyEngine();
}

bool VideoPreview::init() {
    eventQueue.on<MediaChangedEvent>([this](const MediaChangedEvent &e) { onMediaChangedEvent(e); });
    eventQueue.on<CloseVideoEvent>([this](const CloseVideoEvent &e) { onCloseVideoEvent(e); });
    eventQueue.on<PreviewSeekRequestEvent>([this](const PreviewSeekRequestEvent &e) { onPreviewSeekRequestEvent(e); });
    eventQueue.on<SetPreviewEnabledEvent>([this](const SetPreviewEnabledEvent &e) { onSetPreviewEnabledEvent(e); });

    // Just ensure the library is loadable; the mpv instance is created lazily on enable.
    return MpvLoader::load();
}

void VideoPreview::onMediaChangedEvent(const MediaChangedEvent &event) {
    mediaPath = event.path;
    if (enabled && mpv)
        loadMedia();
}

void VideoPreview::onCloseVideoEvent(const CloseVideoEvent &) {
    mediaPath.clear();
    active = false;
    frameReady = false;
    pendingSeek.reset();
    seekInFlight = false;
    if (mpv) {
        const char *cmd[] = {"stop", nullptr};
        MpvLoader::mpv_command_async(mpv, 0, cmd);
    }
}

void VideoPreview::onPreviewSeekRequestEvent(const PreviewSeekRequestEvent &event) {
    if (enabled && mpv && active)
        pendingSeek = event.time; // latest-wins; the coalescer in update() issues it
}

void VideoPreview::onSetPreviewEnabledEvent(const SetPreviewEnabledEvent &event) {
    setEnabled(event.enabled);
}

void VideoPreview::setEnabled(bool enable) {
    if (enable == enabled)
        return;
    enabled = enable;
    if (enable) {
        if (createEngine() && !mediaPath.empty())
            loadMedia();
    } else {
        destroyEngine();
    }
}

bool VideoPreview::createEngine() {
    if (mpv)
        return true;

    // The preview exists only to render seek-hover frames through a GL render context. With no GL
    // (null backend) there is nothing it can do, so stay uninitialized — exactly as on a normal
    // createEngine failure (e.g. libmpv absent), a state the rest of VideoPreview already tolerates.
    if constexpr (ofs::kHeadless)
        return false;

    mpv = MpvLoader::mpv_create();
    if (!mpv) {
        OFS_CORE_ERROR("Failed to create preview mpv instance");
        return false;
    }
    if (MpvLoader::mpv_initialize(mpv) != 0) {
        OFS_CORE_ERROR("Failed to initialize preview mpv instance");
        MpvLoader::mpv_destroy(mpv);
        mpv = nullptr;
        return false;
    }

    MpvLoader::mpv_set_property_string(mpv, "vo", "libmpv");
    MpvLoader::mpv_set_property_string(mpv, "hwdec", hwdecEnabled ? "auto-safe" : "no");
    MpvLoader::mpv_set_property_string(mpv, "aid", "no");    // preview is silent
    MpvLoader::mpv_set_property_string(mpv, "sid", "no");    // no subtitle pipeline
    MpvLoader::mpv_set_property_string(mpv, "pause", "yes"); // never plays — we only seek
    // Keep the second instance lean: it only seeks and grabs single frames, so the demuxer
    // read-ahead cache (mpv's default forward buffer is ~150 MiB) is pure dead weight. Disabling
    // the cache and clamping read-ahead is what keeps this from "doubling" the main player's RAM.
    MpvLoader::mpv_set_property_string(mpv, "cache", "no");
    MpvLoader::mpv_set_property_string(mpv, "demuxer-readahead-secs", "0");
    MpvLoader::mpv_set_property_string(mpv, "demuxer-max-bytes", "8MiB");
    MpvLoader::mpv_set_property_string(mpv, "demuxer-max-back-bytes", "0");
    // Aggressive fast-decode: the preview frame is throwaway, so skip more than the main player.
    MpvLoader::mpv_set_property_string(mpv, "vd-lavc-skiploopfilter", "all");
    MpvLoader::mpv_set_property_string(mpv, "vd-lavc-fast", "yes");
    MpvLoader::mpv_set_property_string(mpv, "video-latency-hacks", "yes");

    mpv_opengl_init_params initParams = {
        .get_proc_address = [](void *, const char *name) -> void * { return (void *)SDL_GL_GetProcAddress(name); }};

    uint32_t enableAdvanced = 1;
    mpv_render_param renderParams[] = {{.type = MPV_RENDER_PARAM_API_TYPE, .data = (void *)MPV_RENDER_API_TYPE_OPENGL},
                                       {.type = MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, .data = &initParams},
                                       {.type = MPV_RENDER_PARAM_ADVANCED_CONTROL, .data = &enableAdvanced},
                                       {.type = MPV_RENDER_PARAM_INVALID, .data = nullptr}};

    if (MpvLoader::mpv_render_context_create(&mpvGl, mpv, renderParams) < 0) {
        OFS_CORE_ERROR("Failed to initialize preview mpv GL context");
        MpvLoader::mpv_destroy(mpv);
        mpv = nullptr;
        return false;
    }

    MpvLoader::mpv_set_wakeup_callback(mpv, onMpvEvents, this);
    MpvLoader::mpv_render_context_set_update_callback(mpvGl, onMpvRenderUpdate, this);

    MpvLoader::mpv_observe_property(mpv, PreviewWidth, "width", MPV_FORMAT_INT64);
    MpvLoader::mpv_observe_property(mpv, PreviewHeight, "height", MPV_FORMAT_INT64);

    return true;
}

void VideoPreview::destroyEngine() {
    if (mpvGl) {
        MpvLoader::mpv_render_context_free(mpvGl);
        mpvGl = nullptr;
    }
    if (mpv) {
        MpvLoader::mpv_destroy(mpv);
        mpv = nullptr;
    }
    if (framebuffer) {
        glDeleteFramebuffers(1, &framebuffer);
        framebuffer = 0;
    }
    if (frameTexture) {
        glDeleteTextures(1, &frameTexture);
        frameTexture = 0;
    }
    videoWidth = videoHeight = 0;
    targetWidth = targetHeight = 0;
    active = false;
    frameReady = false;
    seekInFlight = false;
    pendingSeek.reset();
    lastSeekTarget = -1.0;
    hasEvents.store(0);
    renderUpdate.store(0);
}

void VideoPreview::loadMedia() {
    if (!mpv || mediaPath.empty())
        return;
    frameReady = false;
    seekInFlight = false;
    pendingSeek.reset();
    lastSeekTarget = -1.0;
    const char *cmd[] = {"loadfile", mediaPath.c_str(), nullptr};
    MpvLoader::mpv_command_async(mpv, 0, cmd);
    active = true;
}

void VideoPreview::update(float) {
    if (!mpv)
        return;

    if (hasEvents.exchange(0) > 0)
        processEvents();

    if (renderUpdate.exchange(0) > 0) {
        uint64_t flags = MpvLoader::mpv_render_context_update(mpvGl);
        if (flags & MPV_RENDER_UPDATE_FRAME) {
            renderFrame();
            frameReady = true;
            seekInFlight = false; // a frame landed — the previous seek has settled
        }
    }

    // Coalesced seek: issue the latest pending target once the previous seek produced a frame.
    if (active && pendingSeek && !seekInFlight) {
        double t = *pendingSeek;
        pendingSeek.reset();
        if (lastSeekTarget < 0.0 || std::abs(t - lastSeekTarget) >= kSeekEpsilon) {
            lastSeekTarget = t;
            seekInFlight = true;
            std::string posStr = std::to_string(t);
            // Keyframe (not exact) seeks — far cheaper than the main player's exact seeks and plenty
            // for a scrub thumbnail; the settle gate above keeps fast flicks from flooding mpv.
            const char *cmd[] = {"seek", posStr.c_str(), "absolute+keyframes", nullptr};
            MpvLoader::mpv_command_async(mpv, 0, cmd);
        }
    }
}

void VideoPreview::reportSwap() {
    if (mpvGl)
        MpvLoader::mpv_render_context_report_swap(mpvGl);
}

void VideoPreview::processEvents() {
    for (;;) {
        mpv_event *event = MpvLoader::mpv_wait_event(mpv, 0.0);
        if (event->event_id == MPV_EVENT_NONE)
            break;
        if (event->event_id != MPV_EVENT_PROPERTY_CHANGE)
            continue;

        auto *prop = static_cast<mpv_event_property *>(event->data);
        if (!prop->data)
            continue;

        switch (event->reply_userdata) {
        case PreviewWidth:
            videoWidth = static_cast<int>(*static_cast<int64_t *>(prop->data));
            updateRenderTexture();
            break;
        case PreviewHeight:
            videoHeight = static_cast<int>(*static_cast<int64_t *>(prop->data));
            updateRenderTexture();
            break;
        }
    }
}

void VideoPreview::renderFrame() {
    if (!framebuffer || targetWidth <= 0 || targetHeight <= 0)
        return;

    mpv_opengl_fbo fbo = {
        .fbo = static_cast<int>(framebuffer), .w = targetWidth, .h = targetHeight, .internal_format = GL_RGBA8};

    uint32_t disable = 0;
    mpv_render_param params[] = {{.type = MPV_RENDER_PARAM_OPENGL_FBO, .data = &fbo},
                                 {.type = MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, .data = &disable},
                                 {.type = MPV_RENDER_PARAM_INVALID, .data = nullptr}};
    MpvLoader::mpv_render_context_render(mpvGl, params);
}

void VideoPreview::updateRenderTexture() {
    if (!mpvGl) // no render context (headless, or the engine never created) ⇒ no FBO to build
        return;
    if (videoWidth <= 0 || videoHeight <= 0)
        return;

    // Derive the FBO size once: cap height, keep aspect, never upscale past the source.
    int h = std::min({kPreviewMaxHeight, videoHeight});
    int w = std::max(1, static_cast<int>(static_cast<int64_t>(videoWidth) * h / videoHeight));
    if (w == targetWidth && h == targetHeight && framebuffer)
        return;
    targetWidth = w;
    targetHeight = h;

    if (!framebuffer)
        glGenFramebuffers(1, &framebuffer);
    if (!frameTexture)
        glGenTextures(1, &frameTexture);

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glBindTexture(GL_TEXTURE_2D, frameTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, targetWidth, targetHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, frameTexture, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        OFS_CORE_ERROR("Preview framebuffer is not complete: {:x}", status);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void VideoPreview::onMpvEvents(void *ctx) {
    // Runs on mpv's own thread the instant the callback is installed (before freeze). MUST only bump an
    // atomic — never eventQueue.push() or touch ScriptProject. Real push()es run on the main thread in
    // processEvents() (drained from update()). See MpvVideoPlayer::onMpvEvents and EventQueue::push.
    static_cast<VideoPreview *>(ctx)->hasEvents.fetch_add(1);
}

void VideoPreview::onMpvRenderUpdate(void *ctx) {
    static_cast<VideoPreview *>(ctx)->renderUpdate.fetch_add(1);
}

} // namespace ofs
