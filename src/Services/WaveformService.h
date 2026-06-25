#pragma once

#include "Services/WaveformPeaks.h"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace ofs {

struct ScriptProject;
class EventQueue;
class JobSystem;
struct MediaChangedEvent;
struct LoadVideoEvent;
struct CloseVideoEvent;
struct SetTimelineShowWaveformEvent;
struct WaveformReadyEvent;
struct WaveformProgressEvent;
struct WaveformFailedEvent;
struct CancelWaveformEvent;

// Extracts the loaded media's audio waveform (via bundled ffmpeg on a JobSystem worker), caches the
// peak summary to disk by content fingerprint, and owns the GL texture the timeline renders behind its
// curves. Behavior unit; holds no document state. The texture upload happens on the main thread inside
// the WaveformReadyEvent handler (GL is main-thread-only).
class WaveformService {
  public:
    WaveformService(ScriptProject &project, EventQueue &eq, JobSystem &jobSystem);
    ~WaveformService();

    WaveformService(const WaveformService &) = delete;
    WaveformService &operator=(const WaveformService &) = delete;

    // Read-only handle the renderer samples each frame. Passive accessor (no behavior), so the UI may
    // read it without violating the no-service-calls rule — mirrors VideoPreview::getFrameTexture().
    struct GpuView {
        uint32_t textureId = 0;
        uint32_t bucketCount = 0;
        uint32_t texWidth = 0;
        uint32_t texHeight = 0;
        double durationSeconds = 0.0;
        bool ready = false;
    };
    [[nodiscard]] GpuView gpuView() const { return view_; }

  private:
    void onMediaChanged(const MediaChangedEvent &e);
    void onLoadVideo(const LoadVideoEvent &e);
    void onCloseVideo(const CloseVideoEvent &e);
    void onSetShowWaveform(const SetTimelineShowWaveformEvent &e);
    void onProgress(const WaveformProgressEvent &e);
    void onReady(const WaveformReadyEvent &e);
    void onFailed(const WaveformFailedEvent &e);
    void onCancel(const CancelWaveformEvent &e);

    // Kick off extraction for `sourceUtf8` (the original media with audio). No-op if it's already the
    // current source (loaded, in-flight, or known to have failed) — only a genuine media change retries.
    void requestFor(const std::string &sourceUtf8);
    // The current original-media path with audio (empty if none). The waveform always reflects the
    // original, since an intra-optimized copy may have had its audio stripped.
    [[nodiscard]] std::string currentMediaSource() const;
    void clear();
    void upload(uint32_t bucketCount, double durationSeconds, const std::vector<float> &peaks);

    ScriptProject &project;
    EventQueue &eq;
    JobSystem &jobSystem;

    std::shared_ptr<std::atomic<bool>> cancel_;
    std::future<bool> worker_;
    std::string currentSource_; // UTF-8 path the in-flight / loaded waveform belongs to

    GpuView view_;
    uint32_t texture_ = 0;
};

} // namespace ofs
