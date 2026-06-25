#include "Services/WaveformService.h"
#include "Core/EventQueue.h"
#include "Core/Events.h" // MediaChangedEvent, LoadVideoEvent, CloseVideoEvent
#include "Core/ScriptProject.h"
#include "Core/WaveformEvents.h"
#include "Platform/Headless.h"
#include "Services/JobSystem.h"
#include "Util/FileFingerprint.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"
#include "Util/Subprocess.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <glad/gl.h>
#include <thread>
#include <vector>

namespace ofs {

namespace {

// Convert a freshly-read byte run into mono float samples and feed them to the builder; returns the
// number of samples consumed. `carry` holds the <4 trailing bytes of an incomplete float between reads.
// The common path (empty carry) parses the chunk in place; only a straddling sample needs the concat.
size_t feedSamples(waveform::PeakBuilder &builder, std::string &carry, const std::string &bytes) {
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
    const size_t full = len / 4;
    for (size_t i = 0; i < full; ++i) {
        float s = 0.0f;
        std::memcpy(&s, data + i * 4, sizeof(float)); // ffmpeg f32le == host little-endian on our targets
        builder.add(s);
    }
    carry.assign(data + full * 4, len - full * 4);
    return full;
}

std::vector<const char *> toArgv(const std::vector<std::string> &args) {
    std::vector<const char *> v;
    v.reserve(args.size() + 1);
    for (const auto &a : args)
        v.push_back(a.c_str());
    v.push_back(nullptr);
    return v;
}

// Best-effort source duration (seconds) via ffprobe, used only to drive the progress bar. Returns 0 if
// it can't be determined — the modal then shows a 0% bar that jumps to done, which is acceptable.
double probeDurationSeconds(const std::string &ffprobeBin, const std::string &path) {
    const std::vector<std::string> args = {
        ffprobeBin, "-v", "error", "-show_entries", "format=duration", "-of", "default=nw=1:nk=1", path};
    const auto argv = toArgv(args);
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

    // Cache miss: now the (visible) work begins. The Probing event flips waveform.active true and raises
    // the progress modal on the main thread.
    eq.push(WaveformProgressEvent{.mediaPath = source, .progress = 0.0, .phase = WaveformPhase::Probing});
    const double durationSec = probeDurationSeconds(ffprobeBin, source);
    const double expectedSamples = durationSec > 0.0 ? durationSec * waveform::kDecodeSampleRate : 0.0;
    if (cancel.load(std::memory_order_acquire)) {
        eq.push(WaveformFailedEvent{.mediaPath = source, .cancelled = true});
        return false;
    }

    const std::vector<std::string> args = {ffmpegBin, "-hide_banner", "-nostdin", "-i", source,  "-vn", "-ac",
                                           "1",       "-ar",          "16000",    "-f", "f32le", "-"};
    const auto argv = toArgv(args);
    ofs::util::Process proc = ofs::util::Process::spawn(argv.data());
    if (!proc.valid()) {
        eq.push(WaveformFailedEvent{.mediaPath = source});
        return false;
    }

    waveform::PeakBuilder builder;
    std::string carry;
    uint64_t samplesDecoded = 0;
    const auto startTime = steady_clock::now();
    auto lastPush = startTime;
    for (;;) {
        if (cancel.load(std::memory_order_acquire))
            proc.kill(); // exit is detected below; reported as cancelled

        std::string chunk;
        const size_t n = proc.readSome(chunk, 1u << 16);
        if (n > 0) {
            samplesDecoded += feedSamples(builder, carry, chunk);
            if (const auto now = steady_clock::now(); duration_cast<milliseconds>(now - lastPush).count() >= 150) {
                const double progress =
                    expectedSamples > 0.0 ? std::clamp(static_cast<double>(samplesDecoded) / expectedSamples, 0.0, 1.0)
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
        }

        int code = 0;
        if (proc.exited(&code)) {
            std::string tail;
            while (proc.readSome(tail) > 0) { // drain whatever is buffered after exit
            }
            feedSamples(builder, carry, tail);
            if (cancel.load(std::memory_order_acquire)) {
                eq.push(WaveformFailedEvent{.mediaPath = source, .cancelled = true});
                return false;
            }
            if (code != 0) { // ffmpeg failed — most commonly: the media has no audio stream
                eq.push(WaveformFailedEvent{.mediaPath = source});
                return false;
            }
            break;
        }
        if (n == 0)
            std::this_thread::sleep_for(20ms); // nothing ready yet — don't busy-spin
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
    eq.on<CancelWaveformEvent>([this](const CancelWaveformEvent &e) { onCancel(e); });
}

WaveformService::~WaveformService() {
    // Destroyed before JobSystem joins its workers: signal cancel so a long extraction aborts promptly.
    // The worker holds its own shared_ptr copy of the flag, so this is safe even mid-run.
    if (cancel_)
        cancel_->store(true, std::memory_order_release);
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
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    currentSource_ = sourceUtf8;
    view_.ready = false;
    project.waveform = WaveformState{}; // a fresh request starts idle; the worker raises Probing on cache miss

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
    currentSource_.clear();
    view_.ready = false;
    project.waveform = WaveformState{}; // idle: closes the progress modal if one is open
}

void WaveformService::onProgress(const WaveformProgressEvent &e) {
    if (e.mediaPath != currentSource_) // a newer request superseded this one
        return;
    project.waveform.active = true;
    project.waveform.phase = e.phase;
    project.waveform.progress = e.progress;
    project.waveform.etaSeconds = e.etaSeconds;
}

void WaveformService::onReady(const WaveformReadyEvent &e) {
    if (e.mediaPath != currentSource_ || !e.peaks) // a newer media superseded this result
        return;
    project.waveform.active = false;
    project.waveform.phase = WaveformPhase::Done;
    upload(e.bucketCount, e.durationSeconds, *e.peaks);
}

void WaveformService::onFailed(const WaveformFailedEvent &e) {
    if (e.mediaPath != currentSource_) // stale (e.g. a cancel from a since-superseded request)
        return;
    project.waveform.active = false;
    project.waveform.phase = e.cancelled ? WaveformPhase::Cancelled : WaveformPhase::Failed;
    view_.ready = false; // currentSource_ stays set, so we don't re-run ffmpeg until the media changes
}

void WaveformService::onCancel(const CancelWaveformEvent &) {
    // The worker polls this, kills ffmpeg, and pushes WaveformFailedEvent{cancelled=true}.
    if (cancel_)
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
