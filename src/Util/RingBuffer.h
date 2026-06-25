#pragma once
#include <cstddef>
#include <utility>
#include <vector>

namespace ofs {

// Fixed-capacity circular buffer. Pushes past capacity overwrite the oldest
// element in O(1) — no element shifting. Only the back is popped/inspected,
// which is all the undo/redo stacks need.
template <typename T> class RingBuffer {
  public:
    explicit RingBuffer(size_t capacity) : storage(capacity) {}

    [[nodiscard]] bool empty() const { return count == 0; }
    [[nodiscard]] bool full() const { return count == storage.size(); }
    [[nodiscard]] size_t size() const { return count; }
    [[nodiscard]] size_t capacity() const { return storage.size(); }

    // Append at the back. When full, the oldest element is overwritten.
    void push_back(T value) {
        if (full()) {
            storage[head] = std::move(value);
            head = next(head);
        } else {
            storage[index(count)] = std::move(value);
            ++count;
        }
    }

    [[nodiscard]] T &back() { return storage[index(count - 1)]; }
    [[nodiscard]] const T &back() const { return storage[index(count - 1)]; }

    // Random access in logical order (0 = oldest live element … size()-1 = newest). Lets callers
    // iterate the live window without exposing the wrap-around storage layout.
    [[nodiscard]] T &operator[](size_t logical) { return storage[index(logical)]; }
    [[nodiscard]] const T &operator[](size_t logical) const { return storage[index(logical)]; }

    // Remove the most recent element, releasing its held memory.
    void pop_back() {
        storage[index(count - 1)] = T{};
        --count;
    }

    void clear() {
        for (auto &slot : storage)
            slot = T{};
        head = 0;
        count = 0;
    }

  private:
    [[nodiscard]] size_t next(size_t i) const { return (i + 1) % storage.size(); }
    [[nodiscard]] size_t index(size_t logical) const { return (head + logical) % storage.size(); }

    std::vector<T> storage;
    size_t head = 0;  // index of the oldest element
    size_t count = 0; // number of live elements
};

} // namespace ofs
