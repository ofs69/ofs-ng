#include "Services/WaveformService.h"
#include "Core/EventQueue.h"
#include "Core/Events.h" // MediaChangedEvent, LoadVideoEvent, CloseVideoEvent
#include "Core/ScriptProject.h"
#include "Core/TaskEvents.h"
#include "Core/WaveformEvents.h"
#include "Localization/Translator.h"
#include "Platform/Headless.h"
#include "Services/JobSystem.h"
#include "Util/FileFingerprint.h"
#include "Util/FrameAllocator.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"
#include "Util/Subprocess.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <glad/gl.h>
#include <thread>
#include <vector>

namespace ofs {

namespace {

// Convert a freshly-read byte run of s16le PCM into mono float samples in [-1, 1) and feed them to the
// builder; returns the number of samples consumed. `carry` holds the trailing odd byte of an incomplete
// sample between reads. The common path (empty carry) parses the chunk in place; only a straddling
// sample needs the concat.
void feedSamples(waveform::PeakBuilder &builder, std::string &carry, const std::string &bytes) {
    const char *data = nullptr;
    size_t len = 0;
    std::string joined;
    if (carry.empty()) {
        data = bytes.data();
        len = bytes.size();
    } else {
        joined = carry;
        joined += bytes;
        data = joined.data();
        len = joined.size();
    }
    const size_t full = len / 2;
    for (size_t i = 0; i < full; ++i) {
        int16_t s = 0;
        std::memcpy(&s, data + i * 2, sizeof(int16_t)); // ffmpeg s16le == host little-endian on our targets
        builder.add(static_cast<float>(s) / 32768.0f);
    }
    carry.assign(data + full * 2, len - full * 2);
}

// Best-effort source duration (seconds) via ffprobe, used only to drive the progress bar. Returns 0 if
// it can't be determined — the modal then shows a 0% bar that jumps to done, which is acceptable.
double probeDurationSeconds(const std::string &ffprobeBin, const std::string &path) {
    const std::vector<std::string> args = {
        ffprobeBin, "-v", "error", "-show_entries", "format=duration", "-of", "default=nw=1:nk=1", path};
    const auto argv = ofs::util::toArgv(args);
    std::string out;
    int code = 0;
    if (ofs::util::runCaptured(argv.data(), out, code) && code == 0)
        return std::strtod(out.c_str(), nullptr); // "N/A"/empty → 0
    return 0.0;
}

void pushReady(EventQueue &eq, const std::string &source, waveform::WaveformData data) {
    eq.push(WaveformReadyEvent{.mediaPath = source,
                               .bucketCount = data.bucketCount,
                               .durationSeconds = data.durationSeconds,
                               .peaks = std::make_shared<std::vector<float>>(std::move(data.peaks))});
}

// The whole extraction, run on a JobSystem worker. Touches NO ScriptProject/service — only its captured
// config, the disk cache, and ffmpeg/ffprobe; results go back via EventQueue::push(). Returns true on
// success. A cache hit returns immediately (no progress events, so the modal never shows).
bool runExtraction(EventQueue &eq, const std::string &ffmpegBin, const std::string &ffprobeBin,
                   const std::string &source, const std::filesystem::path &cacheDir,
                   const std::shared_ptr<std::atomic<bool>> &cancelPtr) {
    using namespace std::chrono;
    std::atomic<bool> &cancel = *cancelPtr;

    const std::string fp = ofs::util::fastFileFingerprint(ofs::util::fromUtf8(source));
    std::filesystem::path cacheFile;
    if (!fp.empty())
        cacheFile = cacheDir / ofs::util::fromUtf8(fp + ".wfm");

    if (!cacheFile.empty()) {
        if (auto cached = waveform::loadCache(cacheFile)) {
            pushReady(eq, source, std::move(*cached));
            return true;
        }
    }
    if (cancel.load(std::memory_order_acquire))
        return false;

    // Cache miss: now the (visible) work begins. The Probing event raises the footer task indicator on
    // the main thread (WaveformService::onProgress translates it into the TaskStarted/Progress events).
    eq.push(WaveformProgressEvent{.mediaPath = source, .progress = 0.0, .phase = WaveformPhase::Probing});
    const double durationSec = probeDurationSeconds(ffprobeBin, source);
    const double expectedSamples = durationSec > 0.0 ? durationSec * waveform::kDecodeSampleRate : 0.0;
    if (cancel.load(std::memory_order_acquire)) {
        eq.push(WaveformFailedEvent{.mediaPath = source, .cancelled = true});
        return false;
    }

    // Decode audio to a temp file rather than streaming it back through ffmpeg's stdout pipe. Reading the
    // pipe throttled the decode to our read cadence — the pipe buffer fills and ffmpeg blocks until we
    // drain it — so a long file took far longer than the decode itself. Writing to disk lets ffmpeg run
    // flat-out; we then read the file in one fast pass. s16le (not f32le) halves the temp file: a
    // waveform envelope doesn't need 32-bit samples.
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);
    static std::atomic<uint64_t> tmpSeq{0};
    const std::filesystem::path tmpPath =
        cacheDir / ofs::util::fromUtf8(fmt::format("{}.{}.pcm.tmp", fp.empty() ? "wf" : fp,
                                                   tmpSeq.fetch_add(1, std::memory_order_relaxed)));
    const std::string tmpPathUtf8 = ofs::util::toUtf8(tmpPath);

    // Remove the temp file on every exit path (success, failure, cancel).
    struct TempGuard {
        const std::filesystem::path &path;
        ~TempGuard() {
            std::error_code rmEc;
            std::filesystem::remove(path, rmEc);
        }
    } tempGuard{tmpPath};

    const std::vector<std::string> args = {ffmpegBin, "-hide_banner", "-nostdin", "-i", source,  "-vn", "-ac",
                                           "1",       "-ar",          "16000",    "-f", "s16le", "-y",  tmpPathUtf8};
    const auto argv = ofs::util::toArgv(args);
    ofs::util::Process proc = ofs::util::Process::spawn(argv.data());
    if (!proc.valid()) {
        eq.push(WaveformFailedEvent{.mediaPath = source});
        return false;
    }

    // ffmpeg writes the PCM file unattended; we only poll for exit, cancellation, and progress. Progress
    // tracks the growing file size (bytes / 2 == s16 samples written), not bytes we have consumed.
    const auto startTime = steady_clock::now();
    auto lastPush = startTime;
    int exitCode = 0;
    for (;;) {
        if (cancel.load(std::memory_order_acquire))
            proc.kill(); // exit is detected below; reported as cancelled

        const bool done = proc.exited(&exitCode);

        if (const auto now = steady_clock::now(); duration_cast<milliseconds>(now - lastPush).count() >= 150) {
            std::error_code sizeEc;
            const auto bytes = std::filesystem::file_size(tmpPath, sizeEc);
            const uint64_t samplesDecoded = sizeEc ? 0 : static_cast<uint64_t>(bytes) / sizeof(int16_t);
            const double progress = expectedSamples > 0.0
                                        ? std::clamp(static_cast<double>(samplesDecoded) / expectedSamples, 0.0, 1.0)
                                        : 0.0;
            // ETA from the average decode rate so far (samples/s). Smoother than an instantaneous rate
            // and good enough for a "time remaining" hint; unknown until duration and some work exist.
            double etaSeconds = -1.0;
            if (const double elapsed = duration_cast<duration<double>>(now - startTime).count();
                expectedSamples > 0.0 && samplesDecoded > 0 && elapsed > 0.0) {
                const double rate = static_cast<double>(samplesDecoded) / elapsed;
                etaSeconds = std::max(0.0, (expectedSamples - static_cast<double>(samplesDecoded)) / rate);
            }
            eq.push(WaveformProgressEvent{.mediaPath = source,
                                          .progress = progress,
                                          .etaSeconds = etaSeconds,
                                          .phase = WaveformPhase::Extracting});
            lastPush = now;
        }

        if (done) {
            if (cancel.load(std::memory_order_acquire)) {
                eq.push(WaveformFailedEvent{.mediaPath = source, .cancelled = true});
                return false;
            }
            if (exitCode != 0) { // ffmpeg failed — most commonly: the media has no audio stream
                eq.push(WaveformFailedEvent{.mediaPath = source});
                return false;
            }
            break;
        }
        std::this_thread::sleep_for(20ms); // poll cadence; ffmpeg decodes to disk independently
    }

    std::error_code finalSizeEc;
    const auto tmpBytes = std::filesystem::file_size(tmpPath, finalSizeEc);
    OFS_CORE_INFO("Waveform temp file {}: {} bytes ({:.1f} MiB)", tmpPathUtf8, finalSizeEc ? 0 : tmpBytes,
                  (finalSizeEc ? 0.0 : static_cast<double>(tmpBytes)) / (1024.0 * 1024.0));

    // Decode complete: stream the temp file through the bucket builder in fixed chunks (O(buckets)
    // memory — the PCM is never held whole).
    waveform::PeakBuilder builder;
    {
        std::ifstream in(tmpPath, std::ios::binary);
        if (!in) {
            eq.push(WaveformFailedEvent{.mediaPath = source});
            return false;
        }
        std::string carry;
        std::string chunk;
        for (;;) {
            chunk.resize(1u << 16);
            in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const auto got = in.gcount();
            if (got <= 0)
                break;
            chunk.resize(static_cast<size_t>(got));
            feedSamples(builder, carry, chunk);
        }
    }

    waveform::WaveformData data = builder.finish();
    if (data.bucketCount == 0) {
        eq.push(WaveformFailedEvent{.mediaPath = source});
        return false;
    }
    if (data.bucketCount >= waveform::kMaxBuckets)
        OFS_CORE_WARN("Audio waveform truncated at {} buckets ({} s); the tail is not shown", data.bucketCount,
                      data.durationSeconds);
    if (!cacheFile.empty())
        waveform::writeCache(cacheFile, data);
    pushReady(eq, source, std::move(data));
    return true;
}

} // namespace

WaveformService::WaveformService(ScriptProject &project, EventQueue &eq, JobSystem &jobSystem)
    : project(project), eq(eq), jobSystem(jobSystem) {
    eq.on<MediaChangedEvent>([this](const MediaChangedEvent &e) { onMediaChanged(e); });
    eq.on<LoadVideoEvent>([this](const LoadVideoEvent &e) { onLoadVideo(e); });
    eq.on<CloseVideoEvent>([this](const CloseVideoEvent &e) { onCloseVideo(e); });
    eq.on<SetTimelineShowWaveformEvent>([this](const SetTimelineShowWaveformEvent &e) { onSetShowWaveform(e); });
    eq.on<WaveformProgressEvent>([this](const WaveformProgressEvent &e) { onProgress(e); });
    eq.on<WaveformReadyEvent>([this](const WaveformReadyEvent &e) { onReady(e); });
    eq.on<WaveformFailedEvent>([this](const WaveformFailedEvent &e) { onFailed(e); });
    eq.on<CancelTaskEvent>([this](const CancelTaskEvent &e) { onCancelTask(e); });
}

void WaveformService::cancelInFlight() {
    // The worker holds its own shared_ptr copy of the flag, so this is safe even mid-run.
    if (cancel_)
        cancel_->store(true, std::memory_order_release);
}

WaveformService::~WaveformService() {
    // Backstop in case cancelInFlight() wasn't called before teardown.
    cancelInFlight();
    if constexpr (!ofs::kHeadless) {
        if (texture_ != 0)
            glDeleteTextures(1, &texture_);
    }
}

std::string WaveformService::currentMediaSource() const {
    return project.state.originalMediaPath; // empty when no media is open
}

void WaveformService::onMediaChanged(const MediaChangedEvent &e) {
    if (!project.timelineView.showAudioWaveform) // opt-in: nothing to do until the user enables it
        return;
    // Always extract from the original (the intra-optimized copy may have audio stripped). Fall back to
    // the event's path when no project original is set yet (e.g. a bare LoadVideo before a project loads).
    requestFor(!project.state.originalMediaPath.empty() ? project.state.originalMediaPath : e.path);
}

void WaveformService::onLoadVideo(const LoadVideoEvent &e) {
    if (!project.timelineView.showAudioWaveform)
        return;
    requestFor(!project.state.originalMediaPath.empty() ? project.state.originalMediaPath : e.path);
}

void WaveformService::onCloseVideo(const CloseVideoEvent &) {
    clear();
}

void WaveformService::onSetShowWaveform(const SetTimelineShowWaveformEvent &e) {
    // The toggle is the explicit opt-in: enabling extracts the current media now; disabling cancels any
    // in-flight work and clears the texture so the timeline stops drawing it.
    if (e.show)
        requestFor(currentMediaSource());
    else
        clear();
}

void WaveformService::requestFor(const std::string &sourceUtf8) {
    if (sourceUtf8.empty()) {
        clear();
        return;
    }
    if (sourceUtf8 == currentSource_) // already loaded, in-flight, or known-failed for this source
        return;

    if (cancel_) // supersede any in-flight extraction
        cancel_->store(true, std::memory_order_release);
    footerTask_.end(); // drop any indicator for the superseded extraction; the worker raises a new one on cache miss
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    currentSource_ = sourceUtf8;
    view_.ready = false;

    const std::string ffmpegBin = ofs::util::resolveTool("ffmpeg");
    const std::string ffprobeBin = ofs::util::resolveTool("ffprobe");
    // Cache next to the executable (alongside the bundled ffmpeg), keyed by content fingerprint.
    const std::filesystem::path cacheDir = ofs::util::getBasePath() / "cache" / "waveforms";
    worker_ = jobSystem.submitTask(
        [&q = eq, cancel = cancel_, ffmpegBin, ffprobeBin, source = sourceUtf8, cacheDir]() -> bool {
            return runExtraction(q, ffmpegBin, ffprobeBin, source, cacheDir, cancel);
        });
}

void WaveformService::clear() {
    if (cancel_)
        cancel_->store(true, std::memory_order_release);
    footerTask_.end(); // remove the footer indicator if an extraction was in flight
    currentSource_.clear();
    view_.ready = false;
}

void WaveformService::onProgress(const WaveformProgressEvent &e) {
    if (e.mediaPath != currentSource_) // a newer request superseded this one
        return;

    // Rising edge of an extraction (a cache hit never sends progress) → raise the indicator.
    if (!footerTask_.active())
        footerTask_.start(std::string(Str::WaveformTaskLabel.c_str()), true);

    std::string detail;
    float progress = -1.0f; // indeterminate until we have a real fraction
    if (e.phase == WaveformPhase::Probing) {
        detail = Str::WaveformProbing.c_str();
    } else { // Extracting
        if (e.progress > 0.0)
            progress = static_cast<float>(e.progress);
        if (e.etaSeconds >= 0.0) {
            const auto secs = static_cast<int>(std::lround(e.etaSeconds));
            detail = Str::WaveformEta.fmt(fmtScratch("{}:{:02}", secs / 60, secs % 60));
        } else {
            detail = Str::WaveformExtracting.c_str();
        }
    }
    footerTask_.progress(std::move(detail), progress);
}

void WaveformService::onReady(const WaveformReadyEvent &e) {
    if (e.mediaPath != currentSource_ || !e.peaks) // a newer media superseded this result
        return;
    footerTask_.end();
    upload(e.bucketCount, e.durationSeconds, *e.peaks);
}

void WaveformService::onFailed(const WaveformFailedEvent &e) {
    if (e.mediaPath != currentSource_) // stale (e.g. a cancel from a since-superseded request)
        return;
    footerTask_.end();
    view_.ready = false; // currentSource_ stays set, so we don't re-run ffmpeg until the media changes
    if (!e.cancelled)    // a genuine failure earns a toast + bell entry; a user cancel is self-explanatory
        eq.push(NotifyEvent{.level = NotifyLevel::Warning, .message = std::string(Str::WaveformFailed.c_str())});
}

void WaveformService::onCancelTask(const CancelTaskEvent &e) {
    // The user hit the abort button on our task entry. Flip the flag the worker polls; it kills ffmpeg
    // and pushes WaveformFailedEvent{cancelled=true}, which clears the indicator via onFailed.
    if (footerTask_.matches(e.id) && cancel_)
        cancel_->store(true, std::memory_order_release);
}

void WaveformService::upload(uint32_t bucketCount, double durationSeconds, const std::vector<float> &peaks) {
    if constexpr (ofs::kHeadless)
        return;
    if (bucketCount == 0 || peaks.size() < static_cast<size_t>(bucketCount) * 2)
        return;

    const uint32_t texW = waveform::kTexWidth;
    const uint32_t texH = (bucketCount + texW - 1) / texW; // rows needed to hold every bucket

    // Pad the row-major buffer out to the full texture footprint (trailing texels stay zero — sampled
    // only past bucketCount, which the shader clamps away).
    std::vector<float> padded(static_cast<size_t>(texW) * texH * 2, 0.0f);
    std::memcpy(padded.data(), peaks.data(), static_cast<size_t>(bucketCount) * 2 * sizeof(float));

    if (texture_ == 0)
        glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, static_cast<GLsizei>(texW), static_cast<GLsizei>(texH), 0, GL_RG, GL_FLOAT,
                 padded.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    view_ = GpuView{.textureId = texture_,
                    .bucketCount = bucketCount,
                    .texWidth = texW,
                    .texHeight = texH,
                    .durationSeconds = durationSeconds,
                    .ready = true};
}

} // namespace ofs
