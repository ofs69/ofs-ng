#pragma once

#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "VideoPlayer.h"
#include <algorithm>

namespace ofs {

class DummyVideoPlayer : public VideoPlayer {
  public:
    explicit DummyVideoPlayer(EventQueue &eventQueue);

    bool init() override;

    void update(float dt) override;

    bool isPaused() const override { return paused; }

    double getLogicalPosition() const override { return position; }
    double getActualPosition() const override { return position; }

    double getDuration() const override { return duration; }

    float getPlaybackSpeed() const override { return speed; }

    float getVolume() const override { return 1.0f; }

    uint32_t getFrameTexture() const override { return 0; }
    int getWidth() const override { return 0; }
    int getHeight() const override { return 0; }

    bool isVideoLoaded() const override { return active; }

    // The dummy timeline has no decodable media, so the video window shows "No video loaded"
    // rather than the audio-only placeholder.
    bool hasMedia() const override { return false; }

    double getFps() const override { return 0.0; }

    void notifySwap() override {}

    void setRenderSize(int, int) override {}

  private:
    void openVideo(const std::string &) override {}
    void closeVideo() override {}
    void setVolume(float) override {}

    void setPaused(bool p) override { paused = p; }

    void setPosition(double t) override { position = std::clamp(t, 0.0, duration); }

    void setPlaybackSpeed(float s) override { speed = std::clamp(s, 0.1f, 2.0f); }

    bool active = false;
    double duration = 0.0;
    double position = 0.0;
    bool paused = true;
    float speed = 1.0f;

    EventQueue &eventQueue;
};

} // namespace ofs
