#pragma once

#include "Core/TranscodeEvents.h"
#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ofs {

class EventQueue;
class JobSystem;
struct ScriptProject;

// Pure, SDL-free helpers for building the ffmpeg/ffprobe command lines and parsing ffmpeg's
// machine-readable `-progress` output. Exposed (not file-static) so they can be unit-tested without
// spawning a process (see Step 12).
namespace transcode {

// The `-vf scale=...` filter expression for a downscale factor; empty for Full (omit -vf entirely).
// trunc(iw*N/D/2)*2 forces even output dimensions, which libx264/yuv420p requires for any input size.
std::string scaleFilter(ScaleFactor scale);

// Full ffmpeg argv (as strings) for an all-intra transcode of `cfg`. args[0] is `ffmpegBin`.
std::vector<std::string> buildFfmpegArgs(const std::string &ffmpegBin, const TranscodeConfig &cfg);

// ffprobe argv that prints the first video stream's width/height/frame-rate plus container duration as
// JSON on stdout — the source properties used for both the modal preview and the duration checks.
std::vector<std::string> buildFfprobeInfoArgs(const std::string &ffprobeBin, const std::string &path);

// Parse the JSON stdout of buildFfprobeInfoArgs into a MediaInfo (missing/unparseable fields stay 0).
MediaInfo parseInfoOutput(std::string_view out);

// The even-pixel output dimensions a scale factor produces for a w×h source — mirrors scaleFilter's
// trunc(...*N/D/2)*2 exactly so the modal preview matches what ffmpeg will write. Full returns w,h.
std::pair<int, int> scaledDimensions(int w, int h, ScaleFactor scale);

// Running totals folded out of ffmpeg's `-progress` key=value stream.
struct ProgressAccum {
    long long outTimeUs = 0; // microseconds encoded so far (out_time_us)
    double speed = 0.0;      // encode speed multiple (speed=2.4x → 2.4)
    bool ended = false;      // saw progress=end
};

// Fold one `key=value` line into `acc` (ignores lines it doesn't recognize). ffmpeg's `-progress`
// stream is line-oriented key=value (it has no JSON form), so this stays a hand parser unlike ffprobe.
void applyProgressLine(std::string_view line, ProgressAccum &acc);

} // namespace transcode

// Owns the intra-frame transcode lifecycle: runs ffprobe/ffmpeg as external processes on a JobSystem
// worker, mirrors progress into ScriptProject::transcode, and wires the finished copy into the project.
// Like every service: handlers run on the main thread; the worker only reads its config copy and
// EventQueue::push()es results (never touches ScriptProject or another service).
class VideoTranscoder {
  public:
    VideoTranscoder(ScriptProject &project, EventQueue &eq, JobSystem &jobSystem);
    // Signals any in-flight transcode to cancel so JobSystem's destructor (which waits for running
    // tasks) doesn't stall shutdown for the length of a minutes-long encode.
    ~VideoTranscoder();

  private:
    void onRequest(const TranscodeRequestEvent &ev);
    void onProgress(const TranscodeProgressEvent &ev);
    void onComplete(const TranscodeCompleteEvent &ev);
    void onFailed(const TranscodeFailedEvent &ev);
    void onCancel(const CancelTranscodeEvent &ev);
    void onRequestMediaInfo(const RequestMediaInfoEvent &ev);

    ScriptProject &project;
    EventQueue &eq;
    JobSystem &jobSystem;

    // Set by onCancel (main thread), polled by the worker — the cross-thread cancel channel. A
    // shared_ptr (not a member atomic) so the flag survives even if the service is torn down while a
    // transcode is still running: the worker holds its own copy. Recreated per request.
    std::shared_ptr<std::atomic<bool>> cancel_;
    bool switchAfter_ = true;     // remembered from the running config, applied in onComplete
    std::future<bool> worker_;    // kept so the task handle isn't dropped while the transcode runs
    std::future<bool> infoProbe_; // ffprobe-for-the-modal task handle; runs independently of a transcode
};

} // namespace ofs
