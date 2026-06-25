#pragma once

#include "Video/VideoPlayer.h"
#include <string>

namespace ofs::test {

// Stub VideoPlayer that records calls for assertion in service integration tests.
// No actual video decoding; all state queries return fixed defaults.
class FakeVideoPlayer : public VideoPlayer {
  public:
    int seekCount = 0;
    int playCount = 0;
    int pauseCount = 0;
    double lastSeekPos = 0.0;
    bool paused = true;

    bool init() override { return true; }
    void update(float) override {}

    bool isPaused() const override { return paused; }
    double getLogicalPosition() const override { return lastSeekPos; }
    double getActualPosition() const override { return lastSeekPos; }
    double getDuration() const override { return 0.0; }
    float getPlaybackSpeed() const override { return 1.0f; }
    float getVolume() const override { return 1.0f; }
    uint32_t getFrameTexture() const override { return 0; }
    int getWidth() const override { return 0; }
    int getHeight() const override { return 0; }
    bool isVideoLoaded() const override { return false; }
    bool hasMedia() const override { return false; }
    double getFps() const override { return 30.0; }
    void notifySwap() override {}
    void setRenderSize(int, int) override {}

  private:
    void openVideo(const std::string &) override {}
    void closeVideo() override {}
    void setVolume(float) override {}
    void setPaused(bool p) override {
        paused = p;
        if (!p)
            ++playCount;
        else
            ++pauseCount;
    }
    void setPosition(double seconds) override {
        ++seekCount;
        lastSeekPos = seconds;
    }
    void setPlaybackSpeed(float) override {}
};

} // namespace ofs::test
