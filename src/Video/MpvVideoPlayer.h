#pragma once

#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "PlaybackCursor.h"
#include "VideoPlayer.h"
#include <atomic>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <string>

namespace ofs {
class MpvVideoPlayer : public VideoPlayer {
  public:
    MpvVideoPlayer(bool hwdecEnabled, EventQueue &eventQueue);

    ~MpvVideoPlayer() override;

    bool init() override;

    void update(float dt) override;

    bool isPaused() const override;

    double getLogicalPosition() const override { return cursor.getLogicalPosition(); }
    double getActualPosition() const override { return cursor.getActualPosition(); }

    double getDuration() const override;

    float getPlaybackSpeed() const override;

    float getVolume() const override;

    uint32_t getFrameTexture() const override { return frameTexture; }
    int getWidth() const override { return videoWidth; }
    int getHeight() const override { return videoHeight; }

    bool isVideoLoaded() const override { return videoLoaded; }

    bool hasMedia() const override { return videoLoaded; }

    double getFps() const override { return containerFps; }

    void notifySwap() override;

    void setRenderSize(int width, int height) override;

  private:
    void openVideo(const std::string &path) override;

    void closeVideo() override;

    void setVolume(float volume) override;

    void setPaused(bool paused) override;

    void setPosition(double seconds) override;

    void setPlaybackSpeed(float speed) override;

    void onPlayPauseEvent(const PlayPauseEvent &event);
    void onSeekEvent(const SeekEvent &event);
    void onPlaybackSpeedEvent(const PlaybackSpeedEvent &event);
    void onLoadVideoEvent(const LoadVideoEvent &event);
    void onCloseVideoEvent(const CloseVideoEvent &event);
    void onVolumeChangedEvent(const VolumeChangedEvent &event);

    void processEvents();

    void renderFrame();

    void updateRenderTexture();

    mpv_handle *mpv = nullptr;
    mpv_render_context *mpvGl = nullptr;

    uint32_t framebuffer = 0;
    uint32_t frameTexture = 0;

    int targetWidth = 0;
    int targetHeight = 0;

    std::atomic<int> renderUpdate{0};
    std::atomic<int> hasEvents{0};

    bool videoLoaded = false;
    int videoWidth = 0;
    int videoHeight = 0;
    double duration = 0.0;
    double playbackSpeed = 1.0;
    bool paused = true;
    float volume = 1.0f;
    std::string videoPath;

    double mpvFramePos = 0.0;
    double containerFps = 0.0;

    std::atomic<uint64_t> lastRenderSignalTimeNs{0};
    PlaybackCursor cursor;

    static void onMpvEvents(void *ctx);

    static void onMpvRenderUpdate(void *ctx);

    EventQueue &eventQueue;
    bool hwdecEnabled = true;
};
} // namespace ofs
