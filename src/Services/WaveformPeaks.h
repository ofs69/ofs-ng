#pragma once

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <vector>

// Pure, thread-agnostic audio-waveform helpers: the streaming min/max bucketing and the cache file
// round-trip. Kept free of GL, the event queue, and ffmpeg so they are unit-testable on their own and
// can compile into the no-window unit-test binary. WaveformService drives them from a JobSystem worker.
namespace ofs::waveform {

inline constexpr int kDecodeSampleRate = 16000; // ffmpeg resamples audio to this (mono)
inline constexpr int kBucketsPerSecond = 1000;  // peak buckets per second of audio (1 ms)
inline constexpr int kSamplesPerBucket = kDecodeSampleRate / kBucketsPerSecond; // 16
inline constexpr uint32_t kTexWidth = 2048;                                     // 2-D peak texture width (power of two)
// 8192 rows is within GL_MAX_TEXTURE_SIZE on any desktop GPU (the min guaranteed is 1024, real GPUs
// are >= 8192); at 1000 buckets/s that is ~4.7 h, beyond which the tail is dropped (PeakBuilder::add).
inline constexpr uint32_t kMaxBuckets = kTexWidth * 8192;

struct WaveformData {
    uint32_t bucketCount = 0;
    double durationSeconds = 0.0;
    std::vector<float> peaks; // interleaved [min,max] per bucket; size == 2 * bucketCount
};

// Streaming min/max accumulator: feed mono samples in order, then finish(). Keeps O(buckets) memory,
// never the whole PCM stream. Past kMaxBuckets it keeps counting time (for an accurate duration) but
// stops emitting buckets.
class PeakBuilder {
  public:
    void add(float sample);
    [[nodiscard]] uint32_t bucketCount() const { return static_cast<uint32_t>(peaks_.size() / 2); }
    [[nodiscard]] WaveformData finish() const; // flushes the final partial bucket

  private:
    int countInBucket_ = 0;
    float min_ = std::numeric_limits<float>::max();
    float max_ = std::numeric_limits<float>::lowest();
    uint64_t totalSamples_ = 0;
    std::vector<float> peaks_;
};

// Binary cache round-trip (keyed on the source file fingerprint by the caller). loadCache returns
// nullopt on a missing/corrupt/version-mismatched file; writeCache creates parent dirs as needed.
std::optional<WaveformData> loadCache(const std::filesystem::path &file);
bool writeCache(const std::filesystem::path &file, const WaveformData &data);

} // namespace ofs::waveform
