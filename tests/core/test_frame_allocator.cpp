#include "Util/FrameAllocator.h"
#include <doctest/doctest.h>
#include <string>

TEST_CASE("fmtScratch returns a non-null pointer with correct content") {
    ofs::FrameAllocator fa;
    fa.reset();
    const char *s = fmtScratch("hello {}", 42);
    REQUIRE(s != nullptr);
    CHECK(std::string(s) == "hello 42");
}

TEST_CASE("fmtScratch pointer is valid until reset") {
    ofs::FrameAllocator fa;
    fa.reset();

    const char *s = fmtScratch("value={}", 7);
    REQUIRE(s != nullptr);
    CHECK(std::string(s) == "value=7");

    fa.reset();
    // After reset the pointer is no longer valid — we only verify reset doesn't crash.
}

TEST_CASE("Two fmtScratch calls in the same frame return different pointers with correct content") {
    ofs::FrameAllocator fa;
    fa.reset();

    const char *a = fmtScratch("first");
    const char *b = fmtScratch("second");

    CHECK(a != b);
    CHECK(std::string(a) == "first");
    CHECK(std::string(b) == "second");
}

TEST_CASE("allocArray returns zeroed memory of the right count") {
    ofs::FrameAllocator fa;
    fa.reset();

    int *arr = fa.allocArray<int>(8);
    REQUIRE(arr != nullptr);
    for (int i = 0; i < 8; ++i)
        CHECK(arr[i] == 0);
}

TEST_CASE("reset reclaims space so subsequent allocations fit") {
    ofs::FrameAllocator fa(512);
    fa.reset();

    fa.allocArray<char>(400);

    fa.reset();
    char *p = fa.allocArray<char>(500);
    CHECK(p != nullptr);
}

// alloc() asserts on overflow in a debug build (the high-water-mark canary), so the graceful
// null-return path is only reachable — and only testable — when NDEBUG strips that assert. This
// guards the release behavior: an over-capacity request returns null instead of memset'ing a null
// pointer and crashing.
#ifdef NDEBUG
TEST_CASE("allocArray returns null instead of crashing when the arena is exhausted") {
    ofs::FrameAllocator fa(256);
    fa.reset();

    int *p = fa.allocArray<int>(1024); // 4096 bytes requested into a 256-byte arena
    CHECK(p == nullptr);
}
#endif

TEST_CASE("fmtScratch uses the global singleton FrameAllocator instance") {
    auto &inst = ofs::FrameAllocator::instance();
    inst.reset();

    const char *s = fmtScratch("singleton test {}", 99);
    REQUIRE(s != nullptr);
    CHECK(std::string(s) == "singleton test 99");
}
