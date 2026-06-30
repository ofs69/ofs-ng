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

    float getVolume() const override { return volume; }

    uint32_t getFrameTexture() const override { return fakeTexture_; }
    int getWidth() const override { return fakeWidth_; }
    int getHeight() const override { return fakeHeight_; }

    bool isVideoLoaded() const override { return active; }

    // The dummy timeline normally has no decodable media, so the video window shows "No video loaded"
    // rather than the audio-only placeholder. Tests can opt into a fake decodable frame (below) to make
    // the window render a real image rect + the simulator overlay.
    bool hasMedia() const override { return fakeWidth_ > 0 && fakeHeight_ > 0; }

    // Test-only: report a fake decodable video (non-zero size + texture id) so VideoPlayerWindow renders
    // its image and runs the simulator-overlay interaction, which needs a live video viewport. Default
    // (0,0,0) keeps the production "no media" behavior, so only tests that call this are affected.
    void setFakeVideoForTesting(int width, int height, uint32_t texture) {
        fakeWidth_ = width;
        fakeHeight_ = height;
        fakeTexture_ = texture;
    }

    double getFps() const override { return 0.0; }

    void notifySwap() override {}

    void setRenderSize(int, int) override {}

  private:
    void openVideo(const std::string &) override {}
    void closeVideo() override {}
    void setVolume(float v) override { volume = std::clamp(v, 0.0f, 1.0f); }

    void setPaused(bool p) override { paused = p; }

    void setPosition(double t) override { position = std::clamp(t, 0.0, duration); }

    void setPlaybackSpeed(float s) override { speed = std::clamp(s, kMinPlaybackSpeed, kMaxPlaybackSpeed); }

    bool active = false;
    double duration = 0.0;
    double position = 0.0;
    bool paused = true;
    float speed = 1.0f;
    float volume = 1.0f;

    // Fake decodable-frame dimensions/texture for tests (see setFakeVideoForTesting). 0 → no media.
    int fakeWidth_ = 0;
    int fakeHeight_ = 0;
    uint32_t fakeTexture_ = 0;

    EventQueue &eventQueue;
};

} // namespace ofs
