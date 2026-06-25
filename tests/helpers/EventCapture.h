#pragma once

#include "Core/EventQueue.h"
#include <vector>

namespace ofs::test {

template <typename E> struct EventCapture {
    std::vector<E> received;

    void attach(EventQueue &eq) {
        eq.on<E>([this](const E &e) { received.push_back(e); });
    }
};

} // namespace ofs::test
