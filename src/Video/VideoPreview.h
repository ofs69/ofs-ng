#pragma once

#include "Core/EventQueue.h"
#include "Core/Events.h"
#include <atomic>
#include <cstdint>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <optional>
#include <string>

namespace ofs {

// A second, independent mpv instance dedicated to the timeline hover preview. It tracks the main
// player's media (via MediaChangedEvent) but seeks freely to the hovered time without disturbing
// main playback. Created lazily — only while the feature is enabled — and torn down on disable so
// no second decoder runs when the feature is off.
//
// Threading mirrors MpvVideoPlayer: mpv's wakeup/render callbacks fire on mpv threads and only
// bump atomics; everything else (event pump, render, seek issue, create/destroy) runs on the main
// thread in update() or in drained event handlers.
class VideoPreview {
  public:
    VideoPreview(bool hwdecEnabled, EventQueue &eventQueue);
    ~VideoPreview();

    // Registers event handlers and loads the mpv library. Does NOT create the mpv instance — that
    // happens lazily in setEnabled(true). MUST be called before EventQueue::freeze().
    bool init();

    // Pump mpv events + render updates and issue the next coalesced seek. No-op until the engine
    // is created (feature enabled). Main thread only.
    void update(float dt);

    // Forward a buffer swap to the preview render context (keeps mpv's render timing model happy).
    void reportSwap();

    bool isEnabled() const { return enabled; }
    // True once a real frame has been rendered for the current media — UI gates the image on this
    // and otherwise falls back to a text-only tooltip.
    bool isReady() const { return active && frameReady && videoWidth > 0; }
    uint32_t getFrameTexture() const { return frameTexture; }
    int getWidth() const { return videoWidth; }
    int getHeight() const { return videoHeight; }

  private:
    void onMediaChangedEvent(const MediaChangedEvent &event);
    void onCloseVideoEvent(const CloseVideoEvent &event);
    void onPreviewSeekRequestEvent(const PreviewSeekRequestEvent &event);
    void onSetPreviewEnabledEvent(const SetPreviewEnabledEvent &event);

    void setEnabled(bool enable);
    bool createEngine();
    void destroyEngine();
    void loadMedia();

    void processEvents();
    void renderFrame();
    void updateRenderTexture();

    static void onMpvEvents(void *ctx);
    static void onMpvRenderUpdate(void *ctx);

    EventQueue &eventQueue;
    bool hwdecEnabled = true;

    mpv_handle *mpv = nullptr;
    mpv_render_context *mpvGl = nullptr;
    uint32_t framebuffer = 0;
    uint32_t frameTexture = 0;

    int videoWidth = 0;
    int videoHeight = 0;
    int targetWidth = 0; // preview FBO size — derived once from the first reported dims
    int targetHeight = 0;

    std::atomic<int> renderUpdate{0};
    std::atomic<int> hasEvents{0};

    bool enabled = false; // feature toggle (mirrors AppSettings.showTimelinePreview)
    bool active = false;  // media loaded into the preview instance
    std::string mediaPath;

    // Seek coalescing: keep only the latest requested hover time and issue the next seek once the
    // previous one has produced a frame. Replaces ofs_old's fixed throttle with latest-wins.
    std::optional<double> pendingSeek;
    bool seekInFlight = false;
    bool frameReady = false;
    double lastSeekTarget = -1.0;
};
} // namespace ofs
