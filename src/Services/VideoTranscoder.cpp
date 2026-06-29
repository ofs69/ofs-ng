#include "Services/VideoTranscoder.h"
#include "Core/EventQueue.h"
#include "Core/Events.h" // ChangeMediaPathEvent, NotifyEvent
#include "Core/ScriptProject.h"
#include "Core/TaskEvents.h"
#include "Localization/Translator.h"
#include "Services/JobSystem.h"
#include "Util/FrameAllocator.h" // fmtScratch
#include "Util/PathUtil.h"
#include "Util/Subprocess.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>
#include <system_error>
#include <thread>
#include <utility>

namespace ofs {

namespace transcode {

std::string scaleFilter(ScaleFactor scale) {
    switch (scale) {
    case ScaleFactor::ThreeQuarter:
        return "scale=trunc(iw*3/4/2)*2:trunc(ih*3/4/2)*2";
    case ScaleFactor::Half:
        return "scale=trunc(iw/2/2)*2:trunc(ih/2/2)*2";
    case ScaleFactor::Quarter:
        return "scale=trunc(iw/4/2)*2:trunc(ih/4/2)*2";
    case ScaleFactor::Full:
        break;
    }
    return {};
}

std::vector<std::string> buildFfmpegArgs(const std::string &ffmpegBin, const TranscodeConfig &cfg) {
    std::vector<std::string> a = {ffmpegBin, "-hide_banner", "-nostdin", "-y", "-i", cfg.sourcePath};
    if (cfg.codec == VideoCodec::Mjpeg) {
        // MJPEG is intrinsically all-intra (each frame a standalone JPEG): the fastest decode/seek there
        // is, at the cost of very large files — the modal warns fast local storage is required.
        a.emplace_back("-c:v");
        a.emplace_back("mjpeg");
        a.emplace_back("-q:v");
        a.push_back(std::to_string(cfg.mjpegQuality));
    } else {
        a.emplace_back("-c:v");
        a.emplace_back("libx264");
        a.emplace_back("-x264-params");
        a.emplace_back("keyint=1:scenecut=0"); // every frame an I-frame — the whole point
        a.emplace_back("-crf");
        a.push_back(std::to_string(cfg.crf));
        a.emplace_back("-preset");
        a.push_back(cfg.preset);
        if (cfg.fastDecode) {
            // Cheaper-to-decode bitstream (CAVLC, no deblock) — faster stepping in mpv, larger file.
            a.emplace_back("-tune");
            a.emplace_back("fastdecode");
        }
    }
    if (std::string vf = scaleFilter(cfg.scale); !vf.empty()) {
        a.emplace_back("-vf");
        a.push_back(std::move(vf));
    }
    if (cfg.timing == TimingMode::ConstantFps && cfg.cfrFps > 0.0) {
        a.emplace_back("-r");
        a.push_back(fmt::format("{}", cfg.cfrFps));
    } else {
        // Passthrough preserves source PTS exactly — required for time-based funscript sync (incl. VFR).
        a.emplace_back("-fps_mode");
        a.emplace_back("passthrough");
    }
    switch (cfg.audio) {
    case AudioMode::Copy:
        a.emplace_back("-c:a");
        a.emplace_back("copy");
        break;
    case AudioMode::ReencodeAac:
        a.emplace_back("-c:a");
        a.emplace_back("aac");
        break;
    case AudioMode::None:
        a.emplace_back("-an");
        break;
    }
    if (cfg.forceYuv420p) {
        // mjpeg's planar full-range equivalent is yuvj420p; libx264 takes the studio-range yuv420p.
        a.emplace_back("-pix_fmt");
        a.emplace_back(cfg.codec == VideoCodec::Mjpeg ? "yuvj420p" : "yuv420p");
    }
    a.emplace_back("-movflags");
    a.emplace_back("+faststart");
    // Machine-readable progress on stdout; suppress the human stats line.
    a.emplace_back("-progress");
    a.emplace_back("pipe:1");
    a.emplace_back("-nostats");
    a.push_back(cfg.outputPath);
    return a;
}

std::vector<std::string> buildFfprobeInfoArgs(const std::string &ffprobeBin, const std::string &path) {
    return {ffprobeBin,
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=width,height,r_frame_rate:format=duration",
            "-of",
            "json",
            path};
}

namespace {
// ffprobe emits numbers as JSON strings ("1920", "30000/1001", "123.456"); pull one out whether it
// arrived as a string or a bare number, defaulting to "" when absent.
std::string jsonStr(const nlohmann::json &obj, const char *key) {
    auto it = obj.find(key);
    if (it == obj.end())
        return {};
    if (it->is_string())
        return it->get<std::string>();
    if (it->is_number())
        return it->dump();
    return {};
}
} // namespace

MediaInfo parseInfoOutput(std::string_view out) {
    MediaInfo info;
    const auto root = nlohmann::json::parse(out, nullptr, /*allow_exceptions=*/false);
    if (!root.is_object())
        return info;

    if (auto streams = root.find("streams"); streams != root.end() && streams->is_array() && !streams->empty()) {
        const auto &s = streams->front();
        info.width = static_cast<int>(std::strtol(jsonStr(s, "width").c_str(), nullptr, 10));
        info.height = static_cast<int>(std::strtol(jsonStr(s, "height").c_str(), nullptr, 10));
        // The rate is an exact "num/den" ratio (e.g. 30000/1001); a bare number is tolerated too.
        const std::string rate = jsonStr(s, "r_frame_rate");
        char *slash = nullptr;
        const double num = std::strtod(rate.c_str(), &slash);
        const double den = (slash && *slash == '/') ? std::strtod(slash + 1, nullptr) : 1.0;
        info.fps = den > 0.0 ? num / den : 0.0;
    }
    if (auto fmt = root.find("format"); fmt != root.end() && fmt->is_object())
        info.durationSec = std::strtod(jsonStr(*fmt, "duration").c_str(), nullptr); // "N/A" → 0
    return info;
}

std::pair<int, int> scaledDimensions(int w, int h, ScaleFactor scale) {
    // Mirror scaleFilter's trunc(dim*N/D/2)*2 in double arithmetic so the preview equals ffmpeg's output.
    auto even = [](int dim, double num, double den) { return static_cast<int>(std::trunc(dim * num / den / 2.0)) * 2; };
    switch (scale) {
    case ScaleFactor::ThreeQuarter:
        return {even(w, 3.0, 4.0), even(h, 3.0, 4.0)};
    case ScaleFactor::Half:
        return {even(w, 1.0, 2.0), even(h, 1.0, 2.0)};
    case ScaleFactor::Quarter:
        return {even(w, 1.0, 4.0), even(h, 1.0, 4.0)};
    case ScaleFactor::Full:
        break;
    }
    return {w, h};
}

void applyProgressLine(std::string_view line, ProgressAccum &acc) {
    const auto eq = line.find('=');
    if (eq == std::string_view::npos)
        return;
    std::string_view key = line.substr(0, eq);
    std::string_view val = line.substr(eq + 1);
    if (!val.empty() && val.back() == '\r') // ffmpeg emits CRLF on Windows
        val.remove_suffix(1);
    // strtoll/strtod want a null-terminated buffer; the worker is off the hot path, so a small copy is fine.
    const std::string v(val);
    if (key == "out_time_us") {
        acc.outTimeUs = std::strtoll(v.c_str(), nullptr, 10); // "N/A" → 0
    } else if (key == "speed") {
        acc.speed = std::strtod(v.c_str(), nullptr); // "2.4x" → 2.4, "N/A" → 0
    } else if (key == "progress") {
        acc.ended = (val == "end");
    }
}

} // namespace transcode

namespace {

MediaInfo probeMediaInfo(const std::string &ffprobeBin, const std::string &path) {
    const auto args = transcode::buildFfprobeInfoArgs(ffprobeBin, path);
    const auto argv = ofs::util::toArgv(args);
    std::string out;
    int code = 0;
    if (ofs::util::runCaptured(argv.data(), out, code) && code == 0)
        return transcode::parseInfoOutput(out);
    return {};
}

void pushProgress(EventQueue &eq, const transcode::ProgressAccum &acc, double durationSec) {
    const double outSec = static_cast<double>(acc.outTimeUs) / 1e6;
    const double progress = durationSec > 0.0 ? std::clamp(outSec / durationSec, 0.0, 1.0) : 0.0;
    const double eta = (acc.speed > 0.0 && durationSec > 0.0) ? std::max(0.0, (durationSec - outSec) / acc.speed) : 0.0;
    eq.push(TranscodeProgressEvent{
        .progress = progress, .etaSeconds = eta, .speed = acc.speed, .phase = TranscodePhase::Encoding});
}

// Drain whatever full `key=value` lines have accumulated in `buf`, folding each into `acc` and leaving
// any trailing partial line in place.
void consumeLines(std::string &buf, transcode::ProgressAccum &acc) {
    size_t nl = 0;
    while ((nl = buf.find('\n')) != std::string::npos) {
        transcode::applyProgressLine(std::string_view(buf.data(), nl), acc);
        buf.erase(0, nl + 1);
    }
}

// The whole transcode, run on a JobSystem worker. Touches NO ScriptProject/service — only reads its
// config copy, spawns processes, and pushes result events. Returns true on success.
bool runTranscode(EventQueue &eq, const std::string &ffmpegBin, const std::string &ffprobeBin, TranscodeConfig cfg,
                  const std::shared_ptr<std::atomic<bool>> &cancelPtr) {
    using namespace std::chrono;
    std::atomic<bool> &cancel = *cancelPtr;

    auto fail = [&](TranscodeFailReason reason, bool cancelled, int exitCode = 0, double outDur = 0.0,
                    double srcDurSec = 0.0) {
        std::error_code ec;
        if (!cfg.outputPath.empty())
            std::filesystem::remove(ofs::util::fromUtf8(cfg.outputPath), ec); // drop any partial output
        eq.push(TranscodeFailedEvent{.reason = reason,
                                     .cancelled = cancelled,
                                     .exitCode = exitCode,
                                     .outDurationSec = outDur,
                                     .srcDurationSec = srcDurSec});
        return false;
    };

    // Phase 1 — probing: resolve the duration that seeds the progress %.
    eq.push(TranscodeProgressEvent{.progress = 0.0, .etaSeconds = 0.0, .speed = 0.0, .phase = TranscodePhase::Probing});
    double srcDur =
        cfg.sourceDuration > 0.0 ? cfg.sourceDuration : probeMediaInfo(ffprobeBin, cfg.sourcePath).durationSec;
    if (cancel.load(std::memory_order_acquire))
        return fail(TranscodeFailReason::Cancelled, true);

    // Phase 2 — encoding. Skipped entirely when the user chose to adopt an already-existing output:
    // there is nothing to encode, only the duration check below to confirm the file is sound.
    std::error_code reuseEc;
    const bool reuse = cfg.reuseIfExists && std::filesystem::exists(ofs::util::fromUtf8(cfg.outputPath), reuseEc);
    if (!reuse) {
        const auto args = transcode::buildFfmpegArgs(ffmpegBin, cfg);
        const auto argv = ofs::util::toArgv(args);
        ofs::util::Process proc = ofs::util::Process::spawn(argv.data());
        if (!proc.valid())
            return fail(TranscodeFailReason::CouldNotStartFfmpeg, false);
        eq.push(TranscodeProgressEvent{
            .progress = 0.0, .etaSeconds = 0.0, .speed = 0.0, .phase = TranscodePhase::Encoding});

        std::string buf;
        transcode::ProgressAccum acc;
        auto lastPush = steady_clock::now();
        for (;;) {
            if (cancel.load(std::memory_order_acquire))
                proc.kill(); // exit is detected below; the failure is reported as cancelled

            const size_t n = proc.readSome(buf);
            if (n > 0) {
                consumeLines(buf, acc);
                if (const auto now = steady_clock::now(); duration_cast<milliseconds>(now - lastPush).count() >= 250) {
                    pushProgress(eq, acc, srcDur); // coalesce to ~4 updates/s, not per line
                    lastPush = now;
                }
            }

            int code = 0;
            if (proc.exited(&code)) {
                while (proc.readSome(buf) > 0) {
                } // drain the tail
                consumeLines(buf, acc);
                if (cancel.load(std::memory_order_acquire))
                    return fail(TranscodeFailReason::Cancelled, true);
                if (code != 0)
                    return fail(TranscodeFailReason::FfmpegExitCode, false, code);
                break;
            }
            if (n == 0)
                std::this_thread::sleep_for(milliseconds(40)); // nothing ready yet — don't busy-spin
        }
    }

    // Phase 3 — verifying: the output duration must match the source (catches a botched VFR conversion
    // before the user scripts against it). Skipped if the source duration is unknown.
    eq.push(
        TranscodeProgressEvent{.progress = 1.0, .etaSeconds = 0.0, .speed = 0.0, .phase = TranscodePhase::Verifying});
    if (srcDur > 0.0) {
        const double outDur = probeMediaInfo(ffprobeBin, cfg.outputPath).durationSec;
        if (outDur > 0.0 && std::abs(outDur - srcDur) > std::max(0.5, srcDur * 0.01))
            return fail(TranscodeFailReason::OutputDurationMismatch, false, 0, outDur, srcDur);
    }

    eq.push(TranscodeCompleteEvent{cfg.outputPath});
    return true;
}

} // namespace

VideoTranscoder::VideoTranscoder(ScriptProject &project, EventQueue &eq, JobSystem &jobSystem)
    : project(project), eq(eq), jobSystem(jobSystem) {
    eq.on<TranscodeRequestEvent>([this](const TranscodeRequestEvent &e) { onRequest(e); });
    eq.on<TranscodeProgressEvent>([this](const TranscodeProgressEvent &e) { onProgress(e); });
    eq.on<TranscodeCompleteEvent>([this](const TranscodeCompleteEvent &e) { onComplete(e); });
    eq.on<TranscodeFailedEvent>([this](const TranscodeFailedEvent &e) { onFailed(e); });
    eq.on<CancelTaskEvent>([this](const CancelTaskEvent &e) { onCancelTask(e); });
    eq.on<RequestMediaInfoEvent>([this](const RequestMediaInfoEvent &e) { onRequestMediaInfo(e); });
}

void VideoTranscoder::cancelInFlight() {
    // The worker holds its own shared_ptr copy of the flag, so this is safe even mid-run.
    if (cancel_)
        cancel_->store(true, std::memory_order_release);
}

VideoTranscoder::~VideoTranscoder() {
    // Backstop in case cancelInFlight() wasn't called before teardown.
    cancelInFlight();
}

void VideoTranscoder::onRequest(const TranscodeRequestEvent &ev) {
    if (project.transcode.active)
        return;                      // one transcode at a time; the command is gated, this is the model-side backstop
    TranscodeConfig cfg = ev.config; // non-const so the worker capture can be moved into runTranscode
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    switchAfter_ = cfg.switchAfter;

    project.transcode = TranscodeState{};
    project.transcode.active = true;
    project.transcode.phase = TranscodePhase::Probing;
    project.transcode.sourcePath = cfg.sourcePath;
    project.transcode.outputPath = cfg.outputPath;

    // Raise the footer task indicator for the run (replaces the old blocking progress modal).
    footerTask_.start(std::string(Str::TranscodeTaskLabel.c_str()), true);

    // Resolve the binaries on the main thread (getBasePath is cached here) and hand UTF-8 strings to the
    // worker, so the worker never reaches back into shared resolution state.
    const std::string ffmpegBin = ofs::util::resolveTool("ffmpeg");
    const std::string ffprobeBin = ofs::util::resolveTool("ffprobe");
    worker_ = jobSystem.submitTask([&q = eq, cancel = cancel_, ffmpegBin, ffprobeBin, cfg]() mutable -> bool {
        return runTranscode(q, ffmpegBin, ffprobeBin, std::move(cfg), cancel);
    });
}

void VideoTranscoder::onProgress(const TranscodeProgressEvent &ev) {
    if (!project.transcode.active)
        return;
    project.transcode.phase = ev.phase;
    project.transcode.progress = ev.progress;
    project.transcode.etaSeconds = ev.etaSeconds;
    project.transcode.speed = ev.speed;
    if (!footerTask_.active())
        return;

    // Probing/Verifying are short and have no meaningful fraction → indeterminate bar; Encoding shows the
    // real progress with a speed·ETA detail line (falling back to a plain label until ffmpeg reports speed).
    std::string detail;
    float progress = -1.0f;
    if (ev.phase == TranscodePhase::Probing) {
        detail = Str::TranscodePhaseProbing.c_str();
    } else if (ev.phase == TranscodePhase::Verifying) {
        detail = Str::TranscodePhaseVerifying.c_str();
    } else { // Encoding
        progress = static_cast<float>(ev.progress);
        if (ev.speed > 0.0) {
            const int etaS = static_cast<int>(std::llround(ev.etaSeconds));
            detail =
                Str::TranscodeStats.fmt(fmtScratch("{:.1f}", ev.speed), fmtScratch("{}:{:02d}", etaS / 60, etaS % 60));
        } else {
            detail = Str::TranscodePhaseEncoding.c_str();
        }
    }
    footerTask_.progress(std::move(detail), progress);
}

void VideoTranscoder::onComplete(const TranscodeCompleteEvent &ev) {
    project.state.intraMediaPath = ev.outputPath;
    project.transcode.active = false;
    project.transcode.phase = TranscodePhase::Done;
    project.transcode.progress = 1.0;
    project.transcode.error.clear();
    footerTask_.end();
    eq.push(NotifyEvent{.level = NotifyLevel::Success, .message = std::string(Str::TranscodeDone.c_str())});
    // Switch the player to the optimized copy. ProjectManager's ChangeMediaPath handler sees the path is
    // the (just-set) intra source, flips activeSource = Intra, and loads it — single source of truth.
    if (switchAfter_)
        eq.push(ChangeMediaPathEvent{ev.outputPath});
}

void VideoTranscoder::onFailed(const TranscodeFailedEvent &ev) {
    // The worker already removed any partial output before pushing this.
    project.transcode.active = false;
    project.transcode.phase = ev.cancelled ? TranscodePhase::Cancelled : TranscodePhase::Failed;
    project.transcode.progress = 0.0;
    footerTask_.end();
    if (ev.cancelled) { // a user cancel is self-explanatory: no error text, no toast/bell entry
        project.transcode.error.clear();
        return;
    }

    // Localize the reason on the main thread (the worker only knows the reason code + raw numbers).
    std::string reason;
    switch (ev.reason) {
    case TranscodeFailReason::CouldNotStartFfmpeg:
        reason = Str::TranscodeErrCantStart.c_str();
        break;
    case TranscodeFailReason::FfmpegExitCode:
        reason = Str::TranscodeErrExitCode.fmt(ev.exitCode);
        break;
    case TranscodeFailReason::OutputDurationMismatch:
        reason = Str::TranscodeErrDuration.fmt(fmtScratch("{:.2f}", ev.outDurationSec),
                                               fmtScratch("{:.2f}", ev.srcDurationSec));
        break;
    case TranscodeFailReason::Cancelled:
        break; // handled above
    }
    project.transcode.error = reason;
    std::string msg = reason.empty() ? std::string(Str::TranscodeFailed.c_str())
                                     : fmt::format("{}: {}", Str::TranscodeFailed.sv(), reason);
    eq.push(NotifyEvent{.level = NotifyLevel::Error, .message = std::move(msg)});
}

void VideoTranscoder::onCancelTask(const CancelTaskEvent &ev) {
    // The user hit the abort button on our task entry. Flip the flag the worker polls; it kills ffmpeg
    // and pushes TranscodeFailedEvent{cancelled=true}, which clears the indicator via onFailed.
    if (footerTask_.matches(ev.id) && cancel_)
        cancel_->store(true, std::memory_order_release);
}

void VideoTranscoder::onRequestMediaInfo(const RequestMediaInfoEvent &ev) {
    // ffprobe blocks, so run it on a worker and push the result back; the modal shows a placeholder
    // until then. Resolve the binary on the main thread (cached) so the worker stays self-contained.
    const std::string ffprobeBin = ofs::util::resolveTool("ffprobe");
    infoProbe_ = jobSystem.submitTask([&q = eq, ffprobeBin, path = ev.path]() -> bool {
        q.push(MediaInfoReadyEvent{.path = path, .info = probeMediaInfo(ffprobeBin, path)});
        return true;
    });
}

} // namespace ofs
