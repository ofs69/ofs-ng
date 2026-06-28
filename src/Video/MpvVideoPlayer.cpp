#include "MpvVideoPlayer.h"
#include "MpvLoader.h"
#include "MpvRenderTarget.h"
#include "Platform/Headless.h"
#include "Util/Log.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <glad/gl.h>
#include <string>

namespace ofs {
// reply_userdata tags for the properties we observe; matched in the MPV_EVENT_PROPERTY_CHANGE switch.
enum MpvObservedProp : uint64_t {
    MpvDuration,
    MpvPosition,
    MpvSpeed,
    MpvVideoWidth,
    MpvVideoHeight,
    MpvPauseState,
    MpvFilePath,
    MpvContainerFps,
    MpvVolume,
};

MpvVideoPlayer::MpvVideoPlayer(bool hwdecEnabled, EventQueue &eventQueue)
    : eventQueue(eventQueue), hwdecEnabled(hwdecEnabled) {}

MpvVideoPlayer::~MpvVideoPlayer() {
    if (mpvGl) {
        MpvLoader::mpv_render_context_free(mpvGl);
    }
    if (mpv) {
        MpvLoader::mpv_destroy(mpv);
    }
    if (framebuffer) {
        glDeleteFramebuffers(1, &framebuffer);
    }
    if (frameTexture) {
        glDeleteTextures(1, &frameTexture);
    }
}

bool MpvVideoPlayer::init() {
    eventQueue.on<PlayPauseEvent>([this](const PlayPauseEvent &e) { onPlayPauseEvent(e); });
    eventQueue.on<SeekEvent>([this](const SeekEvent &e) { onSeekEvent(e); });
    eventQueue.on<SetPauseOnSeekEvent>([this](const SetPauseOnSeekEvent &e) { onSetPauseOnSeekEvent(e); });
    eventQueue.on<PlaybackSpeedEvent>([this](const PlaybackSpeedEvent &e) { onPlaybackSpeedEvent(e); });
    eventQueue.on<LoadVideoEvent>([this](const LoadVideoEvent &e) { onLoadVideoEvent(e); });
    eventQueue.on<CloseVideoEvent>([this](const CloseVideoEvent &e) { onCloseVideoEvent(e); });
    eventQueue.on<VolumeChangedEvent>([this](const VolumeChangedEvent &e) { onVolumeChangedEvent(e); });
    eventQueue.on<SetRenderSizeEvent>([this](const SetRenderSizeEvent &e) { setRenderSize(e.width, e.height); });

    if (!MpvLoader::load()) {
        return false;
    }

    mpv = MpvLoader::mpv_create();
    if (!mpv) {
        OFS_CORE_ERROR("Failed to create mpv instance");
        return false;
    }

    if (MpvLoader::mpv_initialize(mpv) != 0) {
        OFS_CORE_ERROR("Failed to initialize mpv instance");
        return false;
    }

    MpvLoader::mpv_set_property_string(mpv, "vo", "libmpv");
    MpvLoader::mpv_set_property_string(mpv, "loop-file", "inf");
    MpvLoader::mpv_set_property_string(mpv, "hwdec", hwdecEnabled ? "auto-safe" : "no");
    // Reduces latency by 1-2 frames; breaks interpolation (which we don't use)
    MpvLoader::mpv_set_property_string(mpv, "video-latency-hacks", "yes");
    // Skip deblocking filter on non-keyframes — major speedup for exact seeks in H.264
    MpvLoader::mpv_set_property_string(mpv, "vd-lavc-skiploopfilter", "nonkey");
    // Fast decode mode (minor quality tradeoff on B-frames, not visible for scripting)
    MpvLoader::mpv_set_property_string(mpv, "vd-lavc-fast", "yes");

    // The mpv GL render context needs a live GL context (it resolves entry points via
    // SDL_GL_GetProcAddress). Under the null backend there is none, so skip it: mpvGl stays null and
    // the player still handles seek/duration/playback events (it just produces no video frames). The
    // wakeup callback below is GL-independent and stays registered so those events still flow.
    if constexpr (!ofs::kHeadless) {
        mpv_opengl_init_params initParams = {.get_proc_address = [](void *ctx, const char *name) -> void * {
            return (void *)SDL_GL_GetProcAddress(name);
        }};

        uint32_t enable = 1;
        mpv_render_param renderParams[] = {
            {.type = MPV_RENDER_PARAM_API_TYPE, .data = (void *)MPV_RENDER_API_TYPE_OPENGL},
            {.type = MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, .data = &initParams},
            {.type = MPV_RENDER_PARAM_ADVANCED_CONTROL, .data = &enable},
            {.type = MPV_RENDER_PARAM_INVALID, .data = nullptr}};

        if (MpvLoader::mpv_render_context_create(&mpvGl, mpv, renderParams) < 0) {
            OFS_CORE_ERROR("Failed to initialize mpv GL context");
            return false;
        }
    }

    MpvLoader::mpv_set_wakeup_callback(mpv, onMpvEvents, this);
    if (mpvGl)
        MpvLoader::mpv_render_context_set_update_callback(mpvGl, onMpvRenderUpdate, this);

    MpvLoader::mpv_observe_property(mpv, MpvVideoHeight, "height", MPV_FORMAT_INT64);
    MpvLoader::mpv_observe_property(mpv, MpvVideoWidth, "width", MPV_FORMAT_INT64);
    MpvLoader::mpv_observe_property(mpv, MpvDuration, "duration", MPV_FORMAT_DOUBLE);
    MpvLoader::mpv_observe_property(mpv, MpvPosition, "time-pos", MPV_FORMAT_DOUBLE);
    MpvLoader::mpv_observe_property(mpv, MpvSpeed, "speed", MPV_FORMAT_DOUBLE);
    MpvLoader::mpv_observe_property(mpv, MpvPauseState, "pause", MPV_FORMAT_FLAG);
    MpvLoader::mpv_observe_property(mpv, MpvFilePath, "path", MPV_FORMAT_STRING);
    MpvLoader::mpv_observe_property(mpv, MpvContainerFps, "container-fps", MPV_FORMAT_DOUBLE);
    MpvLoader::mpv_observe_property(mpv, MpvVolume, "volume", MPV_FORMAT_DOUBLE);

    MpvLoader::mpv_set_property_string(mpv, "pause", "yes");

    return true;
}

void MpvVideoPlayer::openVideo(const std::string &path) {
    OFS_CORE_INFO("Opening video: {}", path);
    duration = 0.0;
    // Clear dimensions up front so the previous file's last frame can't linger when the new file has
    // no video (e.g. an audio file): mpv reports width/height as *unavailable* for audio rather than
    // sending a 0, so the observers would otherwise never overwrite the stale values. Real video
    // dimensions arrive via the width/height property observers below.
    videoWidth = 0;
    videoHeight = 0;
    // A freshly loaded file starts at 0. Reset the cursor now so the previous file's position can't
    // linger: OfsApp copies getLogicalPosition() into playback.cursorPos every frame, and the new file's
    // decoder frames (which would re-anchor it) only arrive several frames later. A saved resume position
    // is applied afterwards as a deferred SeekEvent once the real duration is known.
    cursor.reposition(0.0, 0.0);
    eventQueue.push(DurationChangedEvent{0.0});
    const char *cmd[] = {"loadfile", path.c_str(), nullptr};
    MpvLoader::mpv_command_async(mpv, 0, cmd);
    // A freshly loaded file must land paused — opening a project should never start playing, even when
    // the pause-on-seek policy is off (its deferred resume seek would then not pause). mpv's pause
    // property persists across loadfile, so force it here independent of onSeekEvent.
    setPaused(true);
    videoPath = path;
}

void MpvVideoPlayer::closeVideo() {
    const char *cmd[] = {"stop", nullptr};
    MpvLoader::mpv_command_async(mpv, 0, cmd);
    videoLoaded = false;
    // mpv reports `path` as unavailable (null data) on stop, so the property observer never fires the
    // unload — emit the empty-path transition here so listeners (and the plugin MediaPath cache) clear.
    videoPath.clear();
    eventQueue.push(MediaChangedEvent{videoPath});
}

void MpvVideoPlayer::update(float dt) {
    if (hasEvents.exchange(0) > 0) {
        processEvents();
    }

    if (mpvGl && renderUpdate.exchange(0) > 0) {
        uint64_t flags = MpvLoader::mpv_render_context_update(mpvGl);
        if (flags & MPV_RENDER_UPDATE_FRAME) {
            renderFrame();
            // Feed the cursor phase anchor only on genuine render-update signals. Sampling on every
            // renderFrame() (e.g. the out-of-band repaint in setRenderSize during a zoom lerp) injects
            // off-cadence samples with stale present times that jitter the cursor.
            cursor.onPositionSample(mpvFramePos, lastRenderSignalTimeNs.load(std::memory_order_relaxed), paused,
                                    duration);
        }
    }

    cursor.advance(dt, paused, duration);
}

void MpvVideoPlayer::setPaused(bool paused) {
    if (this->paused == paused)
        return;
    int64_t val = paused;
    MpvLoader::mpv_set_property(mpv, "pause", MPV_FORMAT_FLAG, &val);
}

bool MpvVideoPlayer::isPaused() const {
    return paused;
}

void MpvVideoPlayer::setPosition(double seconds) {
    seconds = std::clamp(seconds, 0.0, duration);
    // Capture whether the decoder is already there before reposition() adopts the target as the anchor.
    const bool alreadyThere = getActualPosition() == seconds;
    // Reposition immediately so PlaybackState::cursorPos (the edit cursor) reflects the target this
    // frame — a seek is an explicit user reposition, not a smoothing snap.
    cursor.reposition(seconds, duration);
    if (alreadyThere)
        return;
    std::string posStr = std::to_string(seconds);
    const char *cmd[] = {"seek", posStr.c_str(), "absolute+exact", nullptr};
    MpvLoader::mpv_command_async(mpv, 0, cmd);
}

double MpvVideoPlayer::getDuration() const {
    return duration;
}

void MpvVideoPlayer::setPlaybackSpeed(float speed) {
    if (this->playbackSpeed == speed)
        return;
    speed = std::clamp(speed, kMinPlaybackSpeed, kMaxPlaybackSpeed);
    // Record the new setpoint. The cursor advances at min(measured, setpoint), so a slow-down binds
    // immediately while a speed-up is tracked from the frame samples — see PlaybackCursor.
    cursor.onSpeedCommand(speed);
    double val = speed;
    MpvLoader::mpv_set_property(mpv, "speed", MPV_FORMAT_DOUBLE, &val);
}

float MpvVideoPlayer::getPlaybackSpeed() const {
    return (float)playbackSpeed;
}

void MpvVideoPlayer::setVolume(float volume) {
    if (this->volume == volume)
        return;
    volume = std::clamp(volume, 0.0f, 1.3f);
    double val = volume * 100.0;
    MpvLoader::mpv_set_property(mpv, "volume", MPV_FORMAT_DOUBLE, &val);
}

float MpvVideoPlayer::getVolume() const {
    return volume;
}

void MpvVideoPlayer::notifySwap() {
    if (mpvGl) {
        MpvLoader::mpv_render_context_report_swap(mpvGl);
    }
}

void MpvVideoPlayer::setRenderSize(int width, int height) {
    if (targetWidth != width || targetHeight != height) {
        targetWidth = width;
        targetHeight = height;
        updateRenderTexture();
        renderFrame();
    }
}

void MpvVideoPlayer::processEvents() {
    for (;;) {
        mpv_event *event = MpvLoader::mpv_wait_event(mpv, 0.0);
        if (event->event_id == MPV_EVENT_NONE)
            break;

        switch (event->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto *prop = static_cast<mpv_event_property *>(event->data);
            if (!prop->data)
                break;

            switch (event->reply_userdata) {
            case MpvVideoWidth:
                videoWidth = static_cast<int>(*static_cast<int64_t *>(prop->data));
                updateRenderTexture();
                break;
            case MpvVideoHeight:
                videoHeight = static_cast<int>(*static_cast<int64_t *>(prop->data));
                updateRenderTexture();
                break;
            case MpvDuration:
                duration = *static_cast<double *>(prop->data);
                eventQueue.push(DurationChangedEvent{duration});
                break;
            case MpvPosition:
                mpvFramePos = *static_cast<double *>(prop->data);
                // Feed the cursor phase anchor on every time-pos update. For video this rides alongside
                // the render-driven samples — same mpvFramePos, so the duplicate is ignored and the
                // render-thread present time stays the anchor. For audio it is the *only* sample source:
                // a plain mp3 renders no frames, and an mp3 with embedded cover art renders a single
                // still one mpv reports as "video" (videoHeight > 0), so neither produces successive
                // frames. Paused, this keeps getActualPosition() current and lets advance() ease the
                // cursor toward a frame-step without a jump.
                cursor.onPositionSample(mpvFramePos, SDL_GetTicksNS(), paused, duration);
                break;
            case MpvSpeed:
                // The speed property is the setpoint, not mpv's instantaneous rate; the cursor's
                // frequency-lock tracks the real rate from samples, so don't seed it from this.
                playbackSpeed = *static_cast<double *>(prop->data);
                eventQueue.push(SpeedChangedEvent{(float)playbackSpeed});
                break;
            case MpvPauseState:
                paused = *static_cast<int64_t *>(prop->data);
                if (paused) {
                    // Arm the one-shot pause-settle so advance() eases logicalPos to the true stop frame,
                    // smoothing out the overshoot from the async pause. Only a genuine play→pause reaches
                    // here — a paused frame-step calls setPaused(true) as a no-op, so it never re-arms the
                    // settle and the cursor holds the stepped time instead of chasing the frame boundary.
                    cursor.beginPauseSettle();
                } else {
                    // Re-anchor the phase clock to now and seed the rate so the first frame after unpause
                    // extrapolates from a fresh elapsed (not the stale pre-pause arrival time) at roughly
                    // the right speed; the frequency-lock refines it from the samples that follow.
                    cursor.setRate(playbackSpeed);
                    cursor.resync(SDL_GetTicksNS());
                }
                eventQueue.push(PlayStateChangedEvent{!paused});
                break;
            case MpvFilePath:
                videoPath = *static_cast<const char **>(prop->data);
                videoLoaded = true;
                eventQueue.push(MediaChangedEvent{videoPath});
                break;
            case MpvContainerFps:
                containerFps = *static_cast<double *>(prop->data);
                break;
            case MpvVolume:
                volume = static_cast<float>(*static_cast<double *>(prop->data) / 100.0);
                break;
            }
            break;
        }
        default:
            break;
        }
    }
}

void MpvVideoPlayer::renderFrame() {
    if (videoWidth <= 0 || videoHeight <= 0)
        return;

    int w = targetWidth > 0 ? targetWidth : videoWidth;
    int h = targetHeight > 0 ? targetHeight : videoHeight;
    mpv::renderToFbo(mpvGl, framebuffer, w, h);
}

void MpvVideoPlayer::updateRenderTexture() {
    if (!mpvGl) // no render context (headless, or GL render-context creation failed) ⇒ nothing to fill
        return;
    if (videoWidth <= 0 || videoHeight <= 0)
        return;

    int w = targetWidth > 0 ? targetWidth : videoWidth;
    int h = targetHeight > 0 ? targetHeight : videoHeight;

    OFS_CORE_TRACE("Updating render texture resolution to {}x{}", w, h);
    mpv::allocRenderTarget(framebuffer, frameTexture, w, h, "Framebuffer");
}

void MpvVideoPlayer::onMpvEvents(void *ctx) {
    // Runs on mpv's own thread, from the moment the wakeup callback is installed in init() — before the
    // EventQueue is frozen. MUST only signal an atomic here: never eventQueue.push() and never touch
    // ScriptProject. The actual push()es happen on the main thread in processEvents() (drained from
    // update()). Pushing here would be a silent data race on the handler map (see EventQueue::push).
    auto *self = static_cast<MpvVideoPlayer *>(ctx);
    self->hasEvents.fetch_add(1);
}

void MpvVideoPlayer::onMpvRenderUpdate(void *ctx) {
    auto *self = static_cast<MpvVideoPlayer *>(ctx);
    // Capture the signal time here on mpv's thread so the main thread
    // doesn't measure its own variable event-processing latency.
    self->lastRenderSignalTimeNs.store(SDL_GetTicksNS(), std::memory_order_relaxed);
    self->renderUpdate.fetch_add(1);
}

void MpvVideoPlayer::onPlayPauseEvent(const PlayPauseEvent &event) {
    setPaused(!isPaused());
}

void MpvVideoPlayer::onSeekEvent(const SeekEvent &event) {
    if (pauseOnSeek)
        setPaused(true);
    setPosition(event.time);
}

void MpvVideoPlayer::onSetPauseOnSeekEvent(const SetPauseOnSeekEvent &event) {
    pauseOnSeek = event.enabled;
}

void MpvVideoPlayer::onPlaybackSpeedEvent(const PlaybackSpeedEvent &event) {
    setPlaybackSpeed(event.speed);
}

void MpvVideoPlayer::onLoadVideoEvent(const LoadVideoEvent &event) {
    openVideo(event.path);
}

void MpvVideoPlayer::onCloseVideoEvent(const CloseVideoEvent &) {
    closeVideo();
}

void MpvVideoPlayer::onVolumeChangedEvent(const VolumeChangedEvent &event) {
    setVolume(event.volume);
}

} // namespace ofs
