#pragma once

#include "Video/VideoMode.h"
#include <cstdint>
#include <string>

namespace ofs {

// Playback-speed bounds, shared by every VideoPlayer implementation's setPlaybackSpeed clamp.
inline constexpr float kMinPlaybackSpeed = 0.1f;
inline constexpr float kMaxPlaybackSpeed = 2.0f;

class VideoPlayer {
  public:
    virtual ~VideoPlayer() = default;

    virtual bool init() = 0;

    virtual void update(float dt) = 0;

    virtual bool isPaused() const = 0;

    // Sub-frame smooth cursor — what all UI should read.
    virtual double getLogicalPosition() const = 0;

    // Last frame boundary reported by the decoder.
    virtual double getActualPosition() const = 0;

    virtual double getDuration() const = 0;

    virtual float getPlaybackSpeed() const = 0;

    virtual float getVolume() const = 0;

    virtual uint32_t getFrameTexture() const = 0;

    virtual int getWidth() const = 0;

    virtual int getHeight() const = 0;

    virtual bool isVideoLoaded() const = 0;

    // True only when real decodable media (audio or video) is loaded. Distinguishes the dummy
    // "scripting without video" timeline (no media at all) from an audio-only file: both report
    // width/height == 0, but only the latter should show the "audio only" placeholder.
    virtual bool hasMedia() const = 0;

    virtual double getFps() const = 0;

    virtual void notifySwap() = 0;

    virtual void setRenderSize(int width, int height) = 0;

  private:
    virtual void openVideo(const std::string &path) = 0;

    virtual void closeVideo() = 0;

    virtual void setVolume(float volume) = 0;

    virtual void setPaused(bool paused) = 0;

    virtual void setPosition(double seconds) = 0;

    virtual void setPlaybackSpeed(float speed) = 0;
};
} // namespace ofs
