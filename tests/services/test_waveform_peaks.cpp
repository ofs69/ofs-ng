#include "Services/WaveformPeaks.h"
#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <system_error>

// Pure-helper tests for the audio waveform: the streaming min/max bucketing and the binary cache
// round-trip. No GL, no ffmpeg, no event queue — the reason these live in WaveformPeaks.{h,cpp}.

using namespace ofs::waveform;

TEST_CASE("PeakBuilder emits one min/max bucket per kSamplesPerBucket samples") {
    PeakBuilder b;
    // A full bucket: a dip to -0.8 near the start and a peak to +0.9 mid-bucket, the rest silence. The
    // sample indices are derived from kSamplesPerBucket so this survives a change to the bucket rate.
    const int dipAt = 1;
    const int peakAt = kSamplesPerBucket / 2;
    for (int i = 0; i < kSamplesPerBucket; ++i) {
        float s = 0.0f;
        if (i == dipAt)
            s = -0.8f;
        else if (i == peakAt)
            s = 0.9f;
        b.add(s);
    }
    CHECK(b.bucketCount() == 1); // exactly one full bucket so far

    // A short trailing run that finish() must flush as a partial bucket.
    for (int i = 0; i < 10; ++i)
        b.add(0.25f);

    const WaveformData d = b.finish();
    REQUIRE(d.bucketCount == 2);
    REQUIRE(d.peaks.size() == 4);
    CHECK(d.peaks[0] == doctest::Approx(-0.8f)); // bucket 0 min
    CHECK(d.peaks[1] == doctest::Approx(0.9f));  // bucket 0 max
    CHECK(d.peaks[2] == doctest::Approx(0.25f)); // bucket 1 min
    CHECK(d.peaks[3] == doctest::Approx(0.25f)); // bucket 1 max

    // Duration is total samples / decode rate, independent of bucket boundaries.
    CHECK(d.durationSeconds == doctest::Approx(static_cast<double>(kSamplesPerBucket + 10) / kDecodeSampleRate));
}

TEST_CASE("PeakBuilder on an empty stream produces nothing") {
    PeakBuilder b;
    const WaveformData d = b.finish();
    CHECK(d.bucketCount == 0);
    CHECK(d.peaks.empty());
    CHECK(d.durationSeconds == doctest::Approx(0.0));
}

TEST_CASE("writeCache/loadCache round-trips a waveform") {
    const auto file = std::filesystem::temp_directory_path() / "ofs_waveform_roundtrip.wfm";
    std::error_code ec;
    std::filesystem::remove(file, ec);

    WaveformData d;
    d.bucketCount = 3;
    d.durationSeconds = 12.5;
    // The cache stores peaks as int16 (each peak is min/max of s16 PCM, i.e. exactly k/32768), so values on
    // that grid round-trip bit-exactly; 0.2 is off-grid and comes back within one quantization step.
    d.peaks = {-1.0f, 0.75f, -0.5f, 0.5f, 0.0f, 0.2f};

    REQUIRE(writeCache(file, d));

    const auto loaded = loadCache(file);
    REQUIRE(loaded.has_value());
    CHECK(loaded->bucketCount == 3);
    CHECK(loaded->durationSeconds == doctest::Approx(12.5));
    REQUIRE(loaded->peaks.size() == d.peaks.size());
    for (size_t i = 0; i < d.peaks.size(); ++i)
        CHECK(loaded->peaks[i] == doctest::Approx(d.peaks[i]).epsilon(1.0 / 32768));

    std::filesystem::remove(file, ec);
}

TEST_CASE("loadCache rejects a missing or truncated file") {
    const auto missing = std::filesystem::temp_directory_path() / "ofs_waveform_does_not_exist.wfm";
    std::error_code ec;
    std::filesystem::remove(missing, ec);
    CHECK_FALSE(loadCache(missing).has_value());

    // A header claiming more buckets than the body holds must be rejected, not read out of bounds.
    const auto truncated = std::filesystem::temp_directory_path() / "ofs_waveform_truncated.wfm";
    {
        std::ofstream out(truncated, std::ios::binary | std::ios::trunc);
        const uint32_t magic = 0x4F574631, version = 2, bucketCount = 1000;
        const double duration = 1.0;
        out.write(reinterpret_cast<const char *>(&magic), sizeof(magic));
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));
        out.write(reinterpret_cast<const char *>(&bucketCount), sizeof(bucketCount));
        out.write(reinterpret_cast<const char *>(&duration), sizeof(duration));
        // ...no peak data at all
    }
    CHECK_FALSE(loadCache(truncated).has_value());
    std::filesystem::remove(truncated, ec);
}
