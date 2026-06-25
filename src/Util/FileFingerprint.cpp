#include "Util/FileFingerprint.h"
#include "Util/PathUtil.h"
#include <cstdint>
#include <fstream>
#include <picosha2.h>
#include <system_error>

namespace ofs::util {

namespace {
constexpr std::streamsize kChunk = 4096;
constexpr std::uintmax_t kSmallFileThreshold = 3 * 4096; // < 12 KiB → hash the whole file
} // namespace

std::string fastFileFingerprint(const std::filesystem::path &path) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec)
        return {};

    // ifstream takes the path's native (wide on Windows) representation, so it is UTF-8-safe without
    // a narrow conversion.
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};

    std::string buf;
    auto readChunk = [&](std::uintmax_t offset, std::streamsize n) -> bool {
        in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!in)
            return false;
        const size_t base = buf.size();
        buf.resize(base + static_cast<size_t>(n));
        in.read(buf.data() + base, n);
        // A short read (truncation/race) keeps only the bytes actually obtained — still deterministic.
        buf.resize(base + static_cast<size_t>(in.gcount()));
        in.clear(); // reading to EOF may set failbit/eofbit; clear so the next seek/read works
        return true;
    };

    if (size < kSmallFileThreshold) {
        if (!readChunk(0, static_cast<std::streamsize>(size)))
            return {};
    } else {
        const std::uintmax_t mid = size / 2 - static_cast<std::uintmax_t>(kChunk) / 2;
        const std::uintmax_t end = size - static_cast<std::uintmax_t>(kChunk);
        if (!readChunk(0, kChunk) || !readChunk(mid, kChunk) || !readChunk(end, kChunk))
            return {};
    }

    // Fold the exact size in (little-endian) so two sources sharing the sampled bytes but differing in
    // length never collide — e.g. one a prefix of the other, or one zero-padded.
    for (int i = 0; i < 8; ++i)
        buf.push_back(static_cast<char>((size >> (i * 8)) & 0xFFu));

    return picosha2::hash256_hex_string(buf);
}

std::filesystem::path intraOutputPath(const std::filesystem::path &outDir, const std::filesystem::path &source) {
    const std::string hash = fastFileFingerprint(source);
    if (hash.empty())
        return {};
    // The name is just the content hash + the (always-mp4) container extension — the source identity is
    // fully captured by the hash, so no stem is needed and one intra copy maps to each source.
    return outDir / ofs::util::fromUtf8(hash + ".mp4");
}

} // namespace ofs::util
