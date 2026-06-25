#pragma once

#include "Core/TranscodeState.h" // TranscodePhase
#include <string>

namespace ofs {

// Output resolution relative to the source. ThreeQuarter/Half/Quarter scale each dimension by the
// factor (rounded to even pixels for the encoder); Full keeps the source resolution.
enum class ScaleFactor { Full, ThreeQuarter, Half, Quarter };

// How the optimized copy treats source timing. KeepOriginal passes frame timestamps through verbatim
// (the safe default — funscripts are time-based, so PTS/duration must be preserved); ConstantFps
// re-times to a fixed rate (cfrFps).
enum class TimingMode { KeepOriginal, ConstantFps };

// What to do with the source audio. Copy passes the stream through (default — keeps sync feel with no
// re-encode); ReencodeAac re-encodes when copy is container-incompatible; None drops audio.
enum class AudioMode { Copy, ReencodeAac, None };

// Output video codec, both intra-only so every frame is independently seekable. H264 (libx264, all
// keyframes) is the size/quality sweet spot; Mjpeg encodes each frame as a standalone JPEG — the
// fastest possible decode/seek but with very large files, so it demands fast local storage.
enum class VideoCodec { H264, Mjpeg };

// The full set of choices made in the options modal, handed to VideoTranscoder via TranscodeRequestEvent.
struct TranscodeConfig {
    std::string sourcePath; // UTF-8
    std::string outputPath; // UTF-8, resolved via ofs::util::intraOutputPath()
    ScaleFactor scale = ScaleFactor::Full;
    TimingMode timing = TimingMode::KeepOriginal;
    double cfrFps = 0.0; // target rate; used only when timing == ConstantFps
    VideoCodec codec = VideoCodec::H264;
    int crf = 21;                // libx264 quality (lower = better/larger); used when codec == H264
    int mjpegQuality = 3;        // mjpeg -q:v (2..31, lower = better/larger); used when codec == Mjpeg
    std::string preset = "fast"; // libx264 -preset: encode speed vs size
    // Drop CABAC + the deblocking filter (x264 -tune fastdecode) so each frame decodes cheaper — faster
    // stepping/seeking in mpv, at the cost of a larger file and more block artifacts. H264-only.
    bool fastDecode = false;
    // Pin 8-bit 4:2:0 output so mpv stays on the hardware-decode fast path (10-bit/4:2:2 sources else
    // fall back to slow software decode). Cost: HDR/10-bit sources are truncated without tone-mapping.
    bool forceYuv420p = false;
    AudioMode audio = AudioMode::Copy;
    bool switchAfter = true;     // load the optimized copy when the transcode finishes
    double sourceDuration = 0.0; // seeds the progress %; from mpv/ffprobe
    // The deterministic output name keys on source identity only, so re-optimizing with new settings
    // reuses the same path. When the file already exists the modal offers two paths: adopt it as-is
    // (reuseIfExists = true → worker skips encoding, only verifies) or overwrite it (false).
    bool reuseIfExists = false;
};

// Command → OfsApp: open the options modal (gating already checked by the command's isEnabled).
struct OpenTranscodeDialogEvent {};

// Options modal → VideoTranscoder: start a transcode with these settings.
struct TranscodeRequestEvent {
    TranscodeConfig config;
};

// Worker → VideoTranscoder (main thread): periodic progress, coalesced to a few per second.
struct TranscodeProgressEvent {
    double progress = 0.0; // 0..1
    double etaSeconds = 0.0;
    double speed = 0.0;
    TranscodePhase phase = TranscodePhase::Encoding;
};

// Worker → VideoTranscoder: the transcode finished successfully; outputPath is the written file.
struct TranscodeCompleteEvent {
    std::string outputPath; // UTF-8
};

// Worker → VideoTranscoder: the transcode ended without a usable output. `cancelled` distinguishes a
// user-requested abort from a genuine failure (different phase + no error toast).
struct TranscodeFailedEvent {
    std::string message;
    bool cancelled = false;
};

// UI → VideoTranscoder: abort the running transcode (kills the ffmpeg process).
struct CancelTranscodeEvent {};

// Source video properties read by ffprobe, used to preview the optimize dialog (resolution + the
// dimensions the chosen scale factor will produce) and to seed timing without touching the player.
struct MediaInfo {
    int width = 0;
    int height = 0;
    double durationSec = 0.0;
    double fps = 0.0;
    bool valid() const { return width > 0 && height > 0; }
};

// OfsApp → VideoTranscoder: probe `path` off the main thread (ffprobe blocks) to feed the modal preview.
struct RequestMediaInfoEvent {
    std::string path; // UTF-8
};

// Worker → OfsApp: the ffprobe result for `path`. `info.valid()` is false if the probe failed.
struct MediaInfoReadyEvent {
    std::string path; // UTF-8; matched against the source the modal is showing
    MediaInfo info;
};

// OfsApp → ProjectManager: the user dismissed the optimize prompt ("Not Now") for the current original.
// Records a per-project flag so reopening this project doesn't re-offer; cleared when a new original loads.
struct DeclineOptimizeEvent {};

} // namespace ofs
