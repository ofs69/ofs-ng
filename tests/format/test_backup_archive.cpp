#include "Format/BackupArchive.h"
#include "Util/PathUtil.h"

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace ofs;

namespace {
void touch(const fs::path &p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "x";
}
} // namespace

TEST_CASE("dirForProject keys by full path, not bare stem") {
    // The data-loss fix: two projects that share a filename stem in different folders must NOT share a
    // backup directory — otherwise one project's pruning silently deletes the other's history.
    auto a = backup::dirForProject(fs::path("Q:/projects/alpha/video.ofp"));
    auto b = backup::dirForProject(fs::path("Q:/projects/beta/video.ofp"));
    CHECK(a != b);

    // Stable for the same path.
    CHECK(backup::dirForProject(fs::path("Q:/projects/alpha/video.ofp")) == a);

    // Distinct stems are distinct too, and the readable stem is preserved in the folder name.
    auto c = backup::dirForProject(fs::path("Q:/projects/alpha/other.ofp"));
    CHECK(c != a);
    CHECK(ofs::util::toUtf8(a.filename()).starts_with("video-"));
    CHECK(ofs::util::toUtf8(c.filename()).starts_with("other-"));

    // A never-saved project maps to the shared "_unnamed_" bucket.
    CHECK(backup::dirForProject(fs::path()).filename() == "_unnamed_");
}

TEST_CASE("fileName is sortable, well-formed, and monotonic with time") {
    std::string name = backup::fileName(1700000000);
    CHECK(name.starts_with("backup-"));
    CHECK(name.ends_with(".ofp"));
    CHECK(name.size() == std::string("backup-YYYY-MM-DD_HH-MM-SS.ofp").size());

    // Fixed-width local-time stamp ⇒ later time sorts lexicographically after earlier time.
    CHECK(backup::fileName(1700003600) > backup::fileName(1700000000));
}

TEST_CASE("list returns only backup-*.ofp, newest first") {
    auto dir = fs::temp_directory_path() / "ofs-backup-archive-test-list";
    fs::remove_all(dir);
    touch(dir / "backup-2024-01-01_00-00-01.ofp");
    touch(dir / "backup-2024-01-01_00-00-03.ofp");
    touch(dir / "backup-2024-01-01_00-00-02.ofp");
    touch(dir / "notes.txt");   // wrong extension
    touch(dir / "scratch.ofp"); // missing prefix

    auto entries = backup::list(dir);
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].display == "2024-01-01 00:00:03"); // newest first
    CHECK(entries[1].display == "2024-01-01 00:00:02");
    CHECK(entries[2].display == "2024-01-01 00:00:01");

    CHECK(backup::list(dir / "does-not-exist").empty());
    fs::remove_all(dir);
}

TEST_CASE("list falls back to the raw core for a hand-renamed backup") {
    auto dir = fs::temp_directory_path() / "ofs-backup-archive-test-fallback";
    fs::remove_all(dir);
    touch(dir / "backup-restored-copy.ofp"); // not a 19-char timestamp

    auto entries = backup::list(dir);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].display == "restored-copy");
    fs::remove_all(dir);
}

TEST_CASE("prune deletes the oldest beyond keepCount, keeping only our files") {
    auto dir = fs::temp_directory_path() / "ofs-backup-archive-test-prune";
    fs::remove_all(dir);
    for (int i = 1; i <= 5; ++i)
        touch(dir / ("backup-2024-01-01_00-00-0" + std::to_string(i) + ".ofp"));
    touch(dir / "keep.txt"); // foreign file: must survive pruning

    backup::prune(dir, 2);

    auto entries = backup::list(dir);
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].display == "2024-01-01 00:00:05"); // two newest survive
    CHECK(entries[1].display == "2024-01-01 00:00:04");
    CHECK(fs::exists(dir / "keep.txt"));

    // keepCount is clamped to >=1.
    backup::prune(dir, 0);
    CHECK(backup::list(dir).size() == 1);

    fs::remove_all(dir);
}
