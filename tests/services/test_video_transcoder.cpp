#include "Core/EventQueue.h"
#include "Core/ScriptProject.h"
#include "Core/TaskEvents.h"
#include "Core/TranscodeEvents.h"
#include "Services/JobSystem.h"
#include "Services/VideoTranscoder.h"
#include <algorithm>
#include <doctest/doctest.h>
#include <string>
#include <utility>
#include <vector>

// Guards the pure, process-free helpers of VideoTranscoder: the ffmpeg/ffprobe argv builders, the
// scale-filter expression, and the `-progress` / duration parsers. These are exposed in the
// ofs::transcode namespace specifically so they can be exercised without spawning a child process.

using namespace ofs;
using namespace ofs::transcode;

namespace {
bool contains(const std::vector<std::string> &v, const std::string &s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// True if `key` appears immediately followed by `val` (an ffmpeg flag/value pair).
bool hasPair(const std::vector<std::string> &v, const std::string &key, const std::string &val) {
    for (size_t i = 0; i + 1 < v.size(); ++i)
        if (v[i] == key && v[i + 1] == val)
            return true;
    return false;
}

TranscodeConfig baseConfig() {
    TranscodeConfig c;
    c.sourcePath = "C:/videos/clip.mp4";
    c.outputPath = "C:/out/clip.hash.iframe.mp4";
    c.crf = 21;
    c.preset = "fast";
    return c;
}
} // namespace

TEST_CASE("scaleFilter maps the factor to an even-dimension -vf expression") {
    CHECK(scaleFilter(ScaleFactor::Full).empty()); // 1x omits -vf entirely
    CHECK(scaleFilter(ScaleFactor::ThreeQuarter) == "scale=trunc(iw*3/4/2)*2:trunc(ih*3/4/2)*2");
    CHECK(scaleFilter(ScaleFactor::Half) == "scale=trunc(iw/2/2)*2:trunc(ih/2/2)*2");
    CHECK(scaleFilter(ScaleFactor::Quarter) == "scale=trunc(iw/4/2)*2:trunc(ih/4/2)*2");
}

TEST_CASE("buildFfmpegArgs encodes the all-intra contract") {
    const auto args = buildFfmpegArgs("ffmpeg", baseConfig());

    // args[0] is the binary; source and output are present.
    CHECK(args.front() == "ffmpeg");
    CHECK(contains(args, "C:/videos/clip.mp4"));
    CHECK(args.back() == "C:/out/clip.hash.iframe.mp4"); // output is the final positional arg

    // The whole point: every frame a keyframe.
    CHECK(hasPair(args, "-x264-params", "keyint=1:scenecut=0"));
    CHECK(hasPair(args, "-c:v", "libx264"));
    CHECK(hasPair(args, "-crf", "21"));
    CHECK(hasPair(args, "-preset", "fast"));
    // Machine-readable progress on stdout.
    CHECK(hasPair(args, "-progress", "pipe:1"));
    CHECK(contains(args, "-nostats"));
}

TEST_CASE("buildFfmpegArgs: scale factor toggles the -vf filter") {
    SUBCASE("Full omits -vf") {
        auto cfg = baseConfig();
        cfg.scale = ScaleFactor::Full;
        CHECK_FALSE(contains(buildFfmpegArgs("ffmpeg", cfg), "-vf"));
    }
    SUBCASE("Half adds the half-scale filter") {
        auto cfg = baseConfig();
        cfg.scale = ScaleFactor::Half;
        CHECK(hasPair(buildFfmpegArgs("ffmpeg", cfg), "-vf", "scale=trunc(iw/2/2)*2:trunc(ih/2/2)*2"));
    }
    SUBCASE("ThreeQuarter adds the 75% filter") {
        auto cfg = baseConfig();
        cfg.scale = ScaleFactor::ThreeQuarter;
        CHECK(hasPair(buildFfmpegArgs("ffmpeg", cfg), "-vf", "scale=trunc(iw*3/4/2)*2:trunc(ih*3/4/2)*2"));
    }
}

TEST_CASE("scaledDimensions matches the scale filter's even-pixel rounding") {
    // Full leaves the source untouched (no -vf).
    CHECK(scaledDimensions(1920, 1080, ScaleFactor::Full) == std::pair{1920, 1080});
    // 75% of 1920×1080 → 1440×810, both already even.
    CHECK(scaledDimensions(1920, 1080, ScaleFactor::ThreeQuarter) == std::pair{1440, 810});
    CHECK(scaledDimensions(1920, 1080, ScaleFactor::Half) == std::pair{960, 540});
    CHECK(scaledDimensions(1920, 1080, ScaleFactor::Quarter) == std::pair{480, 270});
    // Odd source dimensions are forced even (libx264/yuv420p requires it): trunc(719/2/2)*2 = 358.
    CHECK(scaledDimensions(1280, 719, ScaleFactor::Half) == std::pair{640, 358});
}

TEST_CASE("parseInfoOutput reads ffprobe's JSON output") {
    const char *json = R"({
        "streams": [ { "width": 1920, "height": 1080, "r_frame_rate": "30000/1001" } ],
        "format": { "duration": "123.456" }
    })";
    const auto info = parseInfoOutput(json);
    CHECK(info.width == 1920);
    CHECK(info.height == 1080);
    CHECK(info.durationSec == doctest::Approx(123.456));
    CHECK(info.fps == doctest::Approx(30000.0 / 1001.0));
    CHECK(info.valid());

    SUBCASE("numeric width/height and a bare frame rate are tolerated") {
        // ffprobe usually stringifies, but accept raw numbers and a non-ratio rate defensively.
        const char *j2 = R"({"streams":[{"width":640,"height":480,"r_frame_rate":"25"}],"format":{"duration":"N/A"}})";
        const auto i2 = parseInfoOutput(j2);
        CHECK(i2.width == 640);
        CHECK(i2.height == 480);
        CHECK(i2.fps == doctest::Approx(25.0));
        CHECK(i2.durationSec == 0.0); // "N/A" → 0
    }
    SUBCASE("empty / malformed / streamless output is invalid") {
        CHECK_FALSE(parseInfoOutput("").valid());
        CHECK_FALSE(parseInfoOutput("not json").valid());
        CHECK_FALSE(parseInfoOutput(R"({"streams":[]})").valid());
    }
    SUBCASE("a missing or wrong-typed field falls back to zero, not a parse error") {
        // jsonStr tolerates an absent key (no "width") and a non-string/non-number value (null height),
        // so a partial ffprobe payload yields a (0-filled) MediaInfo rather than throwing.
        const char *partial = R"({"streams":[{"height":null,"r_frame_rate":"24/1"}]})";
        const auto i = parseInfoOutput(partial);
        CHECK(i.width == 0);  // key absent
        CHECK(i.height == 0); // present but null (neither string nor number)
        CHECK(i.fps == doctest::Approx(24.0));
    }
}

TEST_CASE("buildFfprobeInfoArgs requests the video stream's geometry and duration as JSON") {
    const auto args = buildFfprobeInfoArgs("ffprobe", "C:/videos/clip.mp4");
    CHECK(args.front() == "ffprobe");
    CHECK(args.back() == "C:/videos/clip.mp4");
    CHECK(hasPair(args, "-select_streams", "v:0"));
    CHECK(hasPair(args, "-show_entries", "stream=width,height,r_frame_rate:format=duration"));
    CHECK(hasPair(args, "-of", "json"));
}

TEST_CASE("buildFfmpegArgs: timing chooses passthrough vs constant fps") {
    SUBCASE("KeepOriginal preserves PTS via fps_mode passthrough") {
        auto cfg = baseConfig();
        cfg.timing = TimingMode::KeepOriginal;
        const auto args = buildFfmpegArgs("ffmpeg", cfg);
        CHECK(hasPair(args, "-fps_mode", "passthrough"));
        CHECK_FALSE(contains(args, "-r"));
    }
    SUBCASE("ConstantFps emits -r and not passthrough") {
        auto cfg = baseConfig();
        cfg.timing = TimingMode::ConstantFps;
        cfg.cfrFps = 60.0;
        const auto args = buildFfmpegArgs("ffmpeg", cfg);
        CHECK(contains(args, "-r"));
        CHECK_FALSE(contains(args, "passthrough"));
    }
    SUBCASE("ConstantFps with a zero rate falls back to passthrough") {
        auto cfg = baseConfig();
        cfg.timing = TimingMode::ConstantFps;
        cfg.cfrFps = 0.0; // nonsensical rate — must not emit a bare -r
        CHECK(hasPair(buildFfmpegArgs("ffmpeg", cfg), "-fps_mode", "passthrough"));
    }
}

TEST_CASE("buildFfmpegArgs: fastDecode toggles x264 -tune fastdecode") {
    auto cfg = baseConfig();
    CHECK_FALSE(contains(buildFfmpegArgs("ffmpeg", cfg), "fastdecode")); // off by default
    cfg.fastDecode = true;
    CHECK(hasPair(buildFfmpegArgs("ffmpeg", cfg), "-tune", "fastdecode"));
}

TEST_CASE("buildFfmpegArgs: forceYuv420p pins the pixel format per codec") {
    auto cfg = baseConfig();
    CHECK_FALSE(contains(buildFfmpegArgs("ffmpeg", cfg), "-pix_fmt")); // unset by default
    cfg.forceYuv420p = true;
    CHECK(hasPair(buildFfmpegArgs("ffmpeg", cfg), "-pix_fmt", "yuv420p")); // libx264 → studio range
    cfg.codec = VideoCodec::Mjpeg;
    CHECK(hasPair(buildFfmpegArgs("ffmpeg", cfg), "-pix_fmt", "yuvj420p")); // mjpeg → full range
}

TEST_CASE("buildFfmpegArgs: MJPEG codec swaps the encoder and quality flag") {
    auto cfg = baseConfig();
    cfg.codec = VideoCodec::Mjpeg;
    cfg.mjpegQuality = 4;
    const auto args = buildFfmpegArgs("ffmpeg", cfg);
    CHECK(hasPair(args, "-c:v", "mjpeg"));
    CHECK(hasPair(args, "-q:v", "4"));
    // None of the libx264-specific flags should leak into an MJPEG encode.
    CHECK_FALSE(contains(args, "libx264"));
    CHECK_FALSE(contains(args, "-x264-params"));
    CHECK_FALSE(contains(args, "-crf"));
    CHECK_FALSE(contains(args, "fastdecode")); // fastDecode is x264-only even if the flag is set
    cfg.fastDecode = true;
    CHECK_FALSE(contains(buildFfmpegArgs("ffmpeg", cfg), "fastdecode"));
}

TEST_CASE("buildFfmpegArgs: audio mode") {
    auto cfg = baseConfig();
    cfg.audio = AudioMode::Copy;
    CHECK(hasPair(buildFfmpegArgs("ffmpeg", cfg), "-c:a", "copy"));
    cfg.audio = AudioMode::ReencodeAac;
    CHECK(hasPair(buildFfmpegArgs("ffmpeg", cfg), "-c:a", "aac"));
    cfg.audio = AudioMode::None;
    CHECK(contains(buildFfmpegArgs("ffmpeg", cfg), "-an"));
}

// ── Service-level handlers (event-driven state machine; no process spawned) ──────────────────────
// These exercise VideoTranscoder's worker→model event handlers directly, mirroring the worker's
// pushes into the TranscodeState the progress modal reads. onRequest is deliberately not driven here
// (it spawns an ffmpeg worker); the handlers below mutate only the model.
namespace {
struct TranscoderFixture {
    ScriptProject project;
    EventQueue eq;
    JobSystem jobSystem; // referenced only; never started (onRequest is not driven)
    VideoTranscoder transcoder{project, eq, jobSystem};
};
} // namespace

TEST_CASE("VideoTranscoder: progress events are mirrored only while a transcode is active") {
    TranscoderFixture f;
    f.eq.freeze();

    // No active transcode ⇒ a stray progress event must not touch the model (the active guard).
    f.eq.push(TranscodeProgressEvent{.progress = 0.5, .phase = TranscodePhase::Encoding});
    f.eq.drain();
    CHECK(f.project.transcode.progress == doctest::Approx(0.0));

    f.project.transcode.active = true;
    f.eq.push(
        TranscodeProgressEvent{.progress = 0.42, .etaSeconds = 12.0, .speed = 3.5, .phase = TranscodePhase::Encoding});
    f.eq.drain();
    CHECK(f.project.transcode.phase == TranscodePhase::Encoding);
    CHECK(f.project.transcode.progress == doctest::Approx(0.42));
    CHECK(f.project.transcode.etaSeconds == doctest::Approx(12.0));
    CHECK(f.project.transcode.speed == doctest::Approx(3.5));
}

TEST_CASE("VideoTranscoder: completion adopts the output as the intra source and ends the run") {
    TranscoderFixture f;
    f.eq.freeze();
    f.project.transcode.active = true;
    f.project.transcode.error = "stale"; // a prior error must be cleared on success

    f.eq.push(TranscodeCompleteEvent{"C:/out/clip.iframe.mp4"});
    f.eq.drain();

    CHECK(f.project.state.intraMediaPath == "C:/out/clip.iframe.mp4");
    CHECK_FALSE(f.project.transcode.active);
    CHECK(f.project.transcode.phase == TranscodePhase::Done);
    CHECK(f.project.transcode.progress == doctest::Approx(1.0));
    CHECK(f.project.transcode.error.empty());
}

TEST_CASE("VideoTranscoder: a failure and a cancel land in distinct phases") {
    TranscoderFixture f;
    f.eq.freeze();

    SUBCASE("genuine failure → Failed with the message") {
        f.project.transcode.active = true;
        f.eq.push(
            TranscodeFailedEvent{.reason = TranscodeFailReason::FfmpegExitCode, .cancelled = false, .exitCode = 1});
        f.eq.drain();
        CHECK_FALSE(f.project.transcode.active);
        CHECK(f.project.transcode.phase == TranscodePhase::Failed);
        CHECK(f.project.transcode.error == "ffmpeg failed (exit code 1)");
        CHECK(f.project.transcode.progress == doctest::Approx(0.0));
    }
    SUBCASE("ffmpeg could not be launched → localized detail") {
        f.project.transcode.active = true;
        f.eq.push(TranscodeFailedEvent{.reason = TranscodeFailReason::CouldNotStartFfmpeg, .cancelled = false});
        f.eq.drain();
        CHECK(f.project.transcode.phase == TranscodePhase::Failed);
        CHECK(f.project.transcode.error == "Could not start ffmpeg");
    }
    SUBCASE("output duration mismatch → detail formats both durations") {
        f.project.transcode.active = true;
        f.eq.push(TranscodeFailedEvent{.reason = TranscodeFailReason::OutputDurationMismatch,
                                       .cancelled = false,
                                       .outDurationSec = 12.0,
                                       .srcDurationSec = 30.5});
        f.eq.drain();
        CHECK(f.project.transcode.phase == TranscodePhase::Failed);
        CHECK(f.project.transcode.error == "Output duration 12.00s differs from source 30.50s");
    }
    SUBCASE("user cancel → Cancelled, no error toast") {
        f.project.transcode.active = true;
        f.project.transcode.error = "stale"; // a cancel must clear any prior error text
        f.eq.push(TranscodeFailedEvent{.reason = TranscodeFailReason::Cancelled, .cancelled = true});
        f.eq.drain();
        CHECK(f.project.transcode.phase == TranscodePhase::Cancelled);
        CHECK(f.project.transcode.error.empty());
    }
}

TEST_CASE("VideoTranscoder: a request while one is already active is ignored (no second worker)") {
    TranscoderFixture f;
    f.eq.freeze();
    // The model-side backstop to the command gate: one transcode at a time. With active already set, the
    // handler returns before spawning, leaving the in-flight run's state untouched.
    f.project.transcode.active = true;
    f.project.transcode.sourcePath = "C:/in/original.mp4";
    f.project.transcode.phase = TranscodePhase::Encoding;

    TranscodeRequestEvent req;
    req.config.sourcePath = "C:/in/other.mp4";
    req.config.outputPath = "C:/out/other.iframe.mp4";
    f.eq.push(req);
    f.eq.drain();

    CHECK(f.project.transcode.sourcePath == "C:/in/original.mp4"); // unchanged — the request was dropped
    CHECK(f.project.transcode.phase == TranscodePhase::Encoding);
}

TEST_CASE("VideoTranscoder: a cancel with no running transcode is a harmless no-op") {
    TranscoderFixture f;
    f.eq.freeze();
    // cancel_ is null until a request arms it; the handler must null-check rather than deref.
    f.eq.push(CancelTaskEvent{});
    f.eq.drain();
    CHECK_FALSE(f.project.transcode.active);
}

TEST_CASE("applyProgressLine folds ffmpeg -progress key=value lines") {
    ProgressAccum acc;

    applyProgressLine("out_time_us=27033333", acc);
    CHECK(acc.outTimeUs == 27033333);

    applyProgressLine("speed=2.4x", acc); // trailing 'x' tolerated
    CHECK(acc.speed == doctest::Approx(2.4));

    CHECK_FALSE(acc.ended);
    applyProgressLine("progress=continue", acc);
    CHECK_FALSE(acc.ended);
    applyProgressLine("progress=end", acc);
    CHECK(acc.ended);

    SUBCASE("CRLF line endings (Windows ffmpeg) are stripped") {
        ProgressAccum win;
        applyProgressLine("out_time_us=1000000\r", win);
        CHECK(win.outTimeUs == 1000000);
        applyProgressLine("progress=end\r", win);
        CHECK(win.ended);
    }

    SUBCASE("N/A and unknown keys are ignored") {
        ProgressAccum na;
        applyProgressLine("out_time_us=N/A", na);
        CHECK(na.outTimeUs == 0);
        applyProgressLine("frame=812", na); // unrecognized key — no effect, no crash
        applyProgressLine("no-equals-sign", na);
        CHECK(na.speed == 0.0);
    }
}
