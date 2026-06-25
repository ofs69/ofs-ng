#include "Core/EventQueue.h"

namespace ofs {

void EventQueue::drain() {
    std::function<void()> fn;
    while (queue_.try_dequeue(fn))
        fn();
}

} // namespace ofs
