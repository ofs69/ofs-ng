#include "Util/FileFingerprint.h"
#include "Util/PathUtil.h"
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

// Guards the pure logic of src/Util/FileFingerprint.h: the fast partial-read SHA-256 and the
// deterministic intra-output naming. getPrefPath() resolves to a temp subdir for the test binary
// (OFS_TEST_PREF_SUBDIR), so these never touch real user data.

namespace {
namespace fs = std::filesystem;

fs::path tmpRoot() {
    return ofs::util::getPrefPath() / "fingerprint_test";
}

// Write `content` to a fresh file under the test temp dir; returns its path. std::ofstream(path)
// uses the wide overload on MSVC, so the path stays UTF-8-safe.
fs::path writeBinary(const fs::path &name, const std::string &content) {
    const fs::path root = tmpRoot();
    std::error_code ec;
    fs::create_directories(root, ec);
    const fs::path p = root / name;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return p;
}

std::string repeat(char c, size_t n) {
    return std::string(n, c);
}
} // namespace

TEST_CASE("fastFileFingerprint is deterministic and content-sensitive") {
    std::error_code ec;
    fs::remove_all(tmpRoot(), ec);

    SUBCASE("identical bytes -> identical 64-char hex hash") {
        const std::string content = repeat('A', 100 * 1024);
        const fs::path a = writeBinary("a.bin", content);
        const fs::path b = writeBinary("b.bin", content);
        const std::string ha = ofs::util::fastFileFingerprint(a);
        CHECK_FALSE(ha.empty());
        CHECK(ha.size() == 64); // SHA-256 hex digest
        CHECK(ha == ofs::util::fastFileFingerprint(b));
        CHECK(ha == ofs::util::fastFileFingerprint(a)); // stable across calls
    }

    SUBCASE("size-only difference -> different hash") {
        // Every 4 KiB sampling window is all 'A' in both files, so the only thing that differs is the
        // length — any change in the digest proves the file size is folded into the hash.
        const fs::path a = writeBinary("sz_a.bin", repeat('A', 64 * 1024));
        const fs::path b = writeBinary("sz_b.bin", repeat('A', 64 * 1024 + 1));
        CHECK(ofs::util::fastFileFingerprint(a) != ofs::util::fastFileFingerprint(b));
    }

    SUBCASE("content change inside the first 4 KiB -> different hash") {
        std::string c1 = repeat('A', 64 * 1024);
        std::string c2 = c1;
        c2[10] = 'B';
        const fs::path a = writeBinary("c_a.bin", c1);
        const fs::path b = writeBinary("c_b.bin", c2);
        CHECK(ofs::util::fastFileFingerprint(a) != ofs::util::fastFileFingerprint(b));
    }

    SUBCASE("small file (< 12 KiB) is hashed whole") {
        const fs::path a = writeBinary("small_a.bin", "hello world");
        const std::string ha = ofs::util::fastFileFingerprint(a);
        CHECK(ha.size() == 64);
        // A single-byte change anywhere is detected, since the whole file is hashed below the threshold.
        const fs::path b = writeBinary("small_b.bin", "hello worle");
        CHECK(ha != ofs::util::fastFileFingerprint(b));
    }

    SUBCASE("missing file -> empty string") {
        CHECK(ofs::util::fastFileFingerprint(tmpRoot() / "does_not_exist.bin").empty());
    }

    fs::remove_all(tmpRoot(), ec);
}

TEST_CASE("intraOutputPath derives <hash>.mp4 in the output dir") {
    std::error_code ec;
    fs::remove_all(tmpRoot(), ec);

    const fs::path src = writeBinary("clip.mp4", repeat('Z', 20 * 1024));
    const fs::path outDir = tmpRoot() / "out";

    const fs::path got = ofs::util::intraOutputPath(outDir, src);
    REQUIRE_FALSE(got.empty());
    CHECK(got.parent_path() == outDir);

    const std::string hash = ofs::util::fastFileFingerprint(src);
    CHECK(ofs::util::toUtf8(got.filename()) == hash + ".mp4");

    // Deterministic: the same source maps to the same output path (the reuse guarantee).
    CHECK(ofs::util::intraOutputPath(outDir, src) == got);

    // A source that can't be fingerprinted yields an empty path.
    CHECK(ofs::util::intraOutputPath(outDir, tmpRoot() / "nope.mp4").empty());

    fs::remove_all(tmpRoot(), ec);
}
