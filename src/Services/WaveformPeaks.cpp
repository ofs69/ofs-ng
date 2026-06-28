#include "Services/WaveformPeaks.h"
#include <algorithm>
#include <cmath>
#include <fstream>

namespace ofs::waveform {

void PeakBuilder::add(float sample) {
    ++totalSamples_;
    if (peaks_.size() / 2 >= kMaxBuckets) // texture full: keep timing the stream, stop emitting buckets
        return;
    min_ = std::min(min_, sample);
    max_ = std::max(max_, sample);
    if (++countInBucket_ >= kSamplesPerBucket) {
        peaks_.push_back(min_);
        peaks_.push_back(max_);
        countInBucket_ = 0;
        min_ = std::numeric_limits<float>::max();
        max_ = std::numeric_limits<float>::lowest();
    }
}

WaveformData PeakBuilder::finish() const {
    WaveformData d;
    d.peaks = peaks_;
    if (countInBucket_ > 0) { // partial trailing bucket
        d.peaks.push_back(min_);
        d.peaks.push_back(max_);
    }
    d.bucketCount = static_cast<uint32_t>(d.peaks.size() / 2);
    d.durationSeconds = static_cast<double>(totalSamples_) / kDecodeSampleRate;
    return d;
}

namespace {
constexpr uint32_t kCacheMagic = 0x4F574631; // "OWF1"
// v2 stores peaks as int16 (was float) — half the bytes. The cache is regenerable, so an older v2-less
// file is simply rejected below and rebuilt; there is no on-disk migration to carry.
constexpr uint32_t kCacheVersion = 2;

// Peaks originate as min/max of s16le PCM, each exactly int16/32768 (see WaveformService::feedSamples), so
// int16 is their lossless native form. The mapping is symmetric for storage and load.
int16_t quantizePeak(float v) {
    return static_cast<int16_t>(std::clamp<long>(std::lround(v * 32768.0f), -32768, 32767));
}
float dequantizePeak(int16_t q) {
    return static_cast<float>(q) / 32768.0f;
}
} // namespace

std::optional<WaveformData> loadCache(const std::filesystem::path &file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::nullopt;
    uint32_t magic = 0, version = 0, bucketCount = 0;
    double duration = 0.0;
    in.read(reinterpret_cast<char *>(&magic), sizeof(magic));
    in.read(reinterpret_cast<char *>(&version), sizeof(version));
    in.read(reinterpret_cast<char *>(&bucketCount), sizeof(bucketCount));
    in.read(reinterpret_cast<char *>(&duration), sizeof(duration));
    if (!in || magic != kCacheMagic || version != kCacheVersion || bucketCount == 0 || bucketCount > kMaxBuckets)
        return std::nullopt;
    const size_t count = static_cast<size_t>(bucketCount) * 2;
    std::vector<int16_t> packed(count);
    in.read(reinterpret_cast<char *>(packed.data()), static_cast<std::streamsize>(count * sizeof(int16_t)));
    if (in.gcount() != static_cast<std::streamsize>(count * sizeof(int16_t)))
        return std::nullopt;
    WaveformData d;
    d.bucketCount = bucketCount;
    d.durationSeconds = duration;
    d.peaks.resize(count);
    std::ranges::transform(packed, d.peaks.begin(), dequantizePeak);
    return d;
}

bool writeCache(const std::filesystem::path &file, const WaveformData &data) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(reinterpret_cast<const char *>(&kCacheMagic), sizeof(kCacheMagic));
    out.write(reinterpret_cast<const char *>(&kCacheVersion), sizeof(kCacheVersion));
    out.write(reinterpret_cast<const char *>(&data.bucketCount), sizeof(data.bucketCount));
    out.write(reinterpret_cast<const char *>(&data.durationSeconds), sizeof(data.durationSeconds));
    std::vector<int16_t> packed(data.peaks.size());
    std::ranges::transform(data.peaks, packed.begin(), quantizePeak);
    out.write(reinterpret_cast<const char *>(packed.data()),
              static_cast<std::streamsize>(packed.size() * sizeof(int16_t)));
    return static_cast<bool>(out);
}

} // namespace ofs::waveform
