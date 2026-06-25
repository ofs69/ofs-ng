#include "FrameAllocator.h"
#include "Util/Log.h"

#include <algorithm>
#include <cassert>

namespace ofs {

FrameAllocator::FrameAllocator(size_t capacity) : _buf(std::make_unique<std::byte[]>(capacity)), _capacity(capacity) {}

void FrameAllocator::reset() {
    _used = 0;
}

void *FrameAllocator::alloc(size_t bytes, size_t align) {
    size_t aligned = (_used + align - 1) & ~(align - 1);
    assert(aligned + bytes <= _capacity && "FrameAllocator overflow");
    if (aligned + bytes > _capacity)
        return nullptr;
    _used = aligned + bytes;
    checkHighWaterMark();
    return _buf.get() + aligned;
}

char *FrameAllocator::currentPos() {
    return reinterpret_cast<char *>(_buf.get() + _used);
}
size_t FrameAllocator::remaining() const {
    return _capacity - _used;
}

void FrameAllocator::advance(size_t n) {
    assert(_used + n <= _capacity && "FrameAllocator overflow");
    _used = std::min(_used + n, _capacity);
    checkHighWaterMark();
}

void FrameAllocator::checkHighWaterMark() {
#ifndef NDEBUG
    if (_used > _peakUsed) {
        _peakUsed = _used;
        OFS_CORE_TRACE("FrameAllocator peak: {} bytes ({:.1f}%)", _peakUsed,
                       100.0 * static_cast<double>(_peakUsed) / static_cast<double>(_capacity));
    }
#endif
}

FrameAllocator &FrameAllocator::instance() {
    static FrameAllocator fa;
    return fa;
}

} // namespace ofs
