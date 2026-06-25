#include "Core/BookmarkChapterState.h"
#include "Core/Events.h"
#include "helpers/PmFixture.h"
#include <doctest/doctest.h>

using ofs::test::PmFixture;
using Bc = ofs::BookmarkChapterState;

// The mutator only appends/edits; the ModifyBookmarkChapterEvent handler re-asserts the
// sorted-by-time invariant. These cases pin that handler behavior.

TEST_CASE("Adding bookmarks appends and the handler keeps them sorted by time") {
    PmFixture f;
    f.send(ofs::ModifyBookmarkChapterEvent{.apply = [](Bc &s) { s.bookmarks.push_back({.time = 3.0, .name = "c"}); }});
    f.send(ofs::ModifyBookmarkChapterEvent{.apply = [](Bc &s) { s.bookmarks.push_back({.time = 1.0, .name = "a"}); }});
    f.send(ofs::ModifyBookmarkChapterEvent{.apply = [](Bc &s) { s.bookmarks.push_back({.time = 2.0, .name = "b"}); }});
    auto &bm = f.project().bookmarks.bookmarks;
    REQUIRE(bm.size() == 3);
    CHECK(bm[0].time == doctest::Approx(1.0));
    CHECK(bm[2].time == doctest::Approx(3.0));
}

TEST_CASE("Removing a bookmark erases by index; out-of-range (caller-guarded) is a no-op") {
    PmFixture f;
    f.send(ofs::ModifyBookmarkChapterEvent{.apply = [](Bc &s) { s.bookmarks.push_back({.time = 1.0, .name = "a"}); }});
    f.send(ofs::ModifyBookmarkChapterEvent{.apply = [](Bc &s) { s.bookmarks.push_back({.time = 2.0, .name = "b"}); }});
    f.send(ofs::ModifyBookmarkChapterEvent{.apply = [](Bc &s) {
        if (99 < static_cast<int>(s.bookmarks.size()))
            s.bookmarks.erase(s.bookmarks.begin() + 99);
    }});
    CHECK(f.project().bookmarks.bookmarks.size() == 2);
    f.send(ofs::ModifyBookmarkChapterEvent{.apply = [](Bc &s) { s.bookmarks.erase(s.bookmarks.begin()); }});
    REQUIRE(f.project().bookmarks.bookmarks.size() == 1);
    CHECK(f.project().bookmarks.bookmarks[0].name == "b");
}

TEST_CASE("Editing a bookmark's time re-sorts via the handler") {
    PmFixture f;
    f.send(ofs::ModifyBookmarkChapterEvent{.apply = [](Bc &s) { s.bookmarks.push_back({.time = 1.0, .name = "a"}); }});
    f.send(ofs::ModifyBookmarkChapterEvent{.apply = [](Bc &s) { s.bookmarks.push_back({.time = 2.0, .name = "b"}); }});
    f.send(ofs::ModifyBookmarkChapterEvent{.apply = [](Bc &s) {
        s.bookmarks[0].time = 5.0;
        s.bookmarks[0].name = "a2";
    }});
    auto &bm = f.project().bookmarks.bookmarks;
    CHECK(bm[0].name == "b");  // 2.0
    CHECK(bm[1].name == "a2"); // 5.0
}

TEST_CASE("Chapter add / modify / remove") {
    PmFixture f;
    f.send(ofs::ModifyBookmarkChapterEvent{.apply = [](Bc &s) {
        s.chapters.push_back({.startTime = 0.0, .endTime = 5.0, .name = "intro", .color = 0xFFFFFFFF});
    }});
    REQUIRE(f.project().bookmarks.chapters.size() == 1);
    f.send(ofs::ModifyBookmarkChapterEvent{.apply = [](Bc &s) {
        s.chapters[0].startTime = 1.0;
        s.chapters[0].endTime = 6.0;
        s.chapters[0].name = "intro2";
        s.chapters[0].color = 0xFF00FF00;
    }});
    CHECK(f.project().bookmarks.chapters[0].name == "intro2");
    CHECK(f.project().bookmarks.chapters[0].startTime == doctest::Approx(1.0));
    f.send(ofs::ModifyBookmarkChapterEvent{.apply = [](Bc &s) { s.chapters.erase(s.chapters.begin()); }});
    CHECK(f.project().bookmarks.chapters.empty());
}
