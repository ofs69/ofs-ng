#include "Services/WaveformPeaks.h"
#include <algorithm>
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
constexpr uint32_t kCacheVersion = 1;
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
    WaveformData d;
    d.bucketCount = bucketCount;
    d.durationSeconds = duration;
    d.peaks.resize(static_cast<size_t>(bucketCount) * 2);
    in.read(reinterpret_cast<char *>(d.peaks.data()), static_cast<std::streamsize>(d.peaks.size() * sizeof(float)));
    if (in.gcount() != static_cast<std::streamsize>(d.peaks.size() * sizeof(float)))
        return std::nullopt;
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
    out.write(reinterpret_cast<const char *>(data.peaks.data()),
              static_cast<std::streamsize>(data.peaks.size() * sizeof(float)));
    return static_cast<bool>(out);
}

} // namespace ofs::waveform
