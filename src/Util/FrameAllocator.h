#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>

namespace ofs {

class FrameAllocator {
  public:
    static constexpr size_t kDefaultSize = 10 * 1024 * 1024; // 10MB

    explicit FrameAllocator(size_t capacity = kDefaultSize);

    void reset();
    void *alloc(size_t bytes, size_t align = alignof(std::max_align_t));

    template <typename T> T *allocArray(size_t count) {
        auto *p = static_cast<T *>(alloc(sizeof(T) * count, alignof(T)));
        if (!p) // arena exhausted: alloc() returns null rather than overrunning the buffer
            return nullptr;
        std::memset(p, 0, sizeof(T) * count);
        return p;
    }

    char *currentPos();
    size_t remaining() const;
    void advance(size_t n);

    static FrameAllocator &instance();

  private:
    void checkHighWaterMark();

    std::unique_ptr<std::byte[]> _buf;
    size_t _capacity;
    size_t _used = 0;
    size_t _peakUsed = 0;
};

} // namespace ofs

#include <spdlog/fmt/fmt.h>

template <typename... Args> const char *fmtScratch(fmt::format_string<Args...> fmtStr, Args &&...args) {
    auto &fa = ofs::FrameAllocator::instance();
    char *out = fa.currentPos();
    size_t rem = fa.remaining();
    if (rem == 0)
        return "";
    size_t maxWrite = rem - 1;
    auto result = fmt::format_to_n(out, maxWrite, fmtStr, std::forward<Args>(args)...);
    size_t written = std::min(result.size, maxWrite);
    out[written] = '\0';
    fa.advance(written + 1);
    return out;
}
