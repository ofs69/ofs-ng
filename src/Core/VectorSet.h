#pragma once

#include <algorithm>
#include <concepts>
#include <iterator>
#include <vector>

namespace ofs {
/**
 * @brief A sorted vector that maintains unique elements.
 *
 * VectorSet provides the interface of a set (logarithmic lookups, uniqueness)
 * with the performance characteristics of a vector (contiguous memory, cache friendliness).
 * It is ideal for data that is read/searched often but modified less frequently,
 * or where modifications are naturally sorted.
 */
template <typename T, typename Compare = std::less<T>> class VectorSet {
  public:
    using container_type = std::vector<T>;
    using value_type = typename container_type::value_type;
    using iterator = typename container_type::iterator;
    using const_iterator = typename container_type::const_iterator;
    using size_type = typename container_type::size_type;

    VectorSet() = default;

    explicit VectorSet(size_type capacity) { storage.reserve(capacity); }

    template <std::input_iterator InputIt, std::sentinel_for<InputIt> Sentinel>
    VectorSet(InputIt first, Sentinel last) {
        std::ranges::copy(first, last, std::back_inserter(storage));
        sort();
    }

    explicit VectorSet(const Compare &comparator) : comp(comparator) {}

    // Iterators
    iterator begin() { return storage.begin(); }
    [[nodiscard]] const_iterator begin() const { return storage.begin(); }
    iterator end() { return storage.end(); }
    [[nodiscard]] const_iterator end() const { return storage.end(); }
    [[nodiscard]] const_iterator cbegin() const { return storage.cbegin(); }
    [[nodiscard]] const_iterator cend() const { return storage.cend(); }

    // Capacity
    [[nodiscard]] bool empty() const noexcept { return storage.empty(); }
    [[nodiscard]] size_type size() const noexcept { return storage.size(); }
    void reserve(size_type newCap) { storage.reserve(newCap); }
    void clear() noexcept { storage.clear(); }

    // Modifiers

    /**
     * @brief Inserts an element if it doesn't already exist.
     * @return A pair containing an iterator to the element and a bool indicating if insertion took place.
     */
    std::pair<iterator, bool> insert(const T &value) {
        auto it = lowerBound(value);
        if (it != end() && !comp(value, *it)) {
            return {it, false};
        }
        return {storage.insert(it, value), true};
    }

    std::pair<iterator, bool> insert(T &&value) {
        auto it = lowerBound(value);
        if (it != end() && !comp(value, *it)) {
            return {it, false};
        }
        return {storage.insert(it, std::move(value)), true};
    }

    // Append an element expected to sort strictly after every existing one — the case when rebuilding from
    // already-sorted, unique data (e.g. deserializing a snapshot). O(1) amortized when the expectation
    // holds, skipping the binary search insert() would run. If the element is NOT strictly greater than the
    // current back (out of order, or a duplicate), it falls back to insert() so the sorted-unique invariant
    // holds for any input. Reserve first when the final count is known to avoid reallocation.
    std::pair<iterator, bool> appendSorted(T &&value) {
        if (storage.empty() || comp(storage.back(), value)) { // back < value ⇒ strictly greater ⇒ safe to append
            storage.push_back(std::move(value));              // push_back, not insert(end()): far cheaper under MSVC's
            return {std::prev(storage.end()), true};          // debug checked iterators, and identical otherwise
        }
        return insert(std::move(value));
    }

    template <std::input_iterator InputIt, std::sentinel_for<InputIt> Sentinel>
    void insertRange(InputIt first, Sentinel last) {
        std::ranges::copy(first, last, std::back_inserter(storage));
        sort();
    }

    // Replaces [rangeStart, rangeEnd) with [first, last), then re-sorts once.
    template <std::input_iterator InputIt, std::sentinel_for<InputIt> Sentinel>
    void replaceRange(const_iterator rangeStart, const_iterator rangeEnd, InputIt first, Sentinel last) {
        container_type next;
        next.reserve(static_cast<size_type>(rangeStart - begin()) + static_cast<size_type>(end() - rangeEnd));
        std::ranges::copy(begin(), rangeStart, std::back_inserter(next));
        std::ranges::copy(first, last, std::back_inserter(next));
        std::ranges::copy(rangeEnd, end(), std::back_inserter(next));
        storage = std::move(next);
        sort();
    }

    iterator erase(const_iterator pos) { return storage.erase(pos); }

    // Erase every element in the inclusive range [lo, hi] (by the comparator). lo/hi are bound
    // values, not necessarily present elements. A no-op when lo > hi or the range is empty.
    void eraseRange(const T &lo, const T &hi) {
        auto first = lowerBound(lo);
        auto last = upperBound(hi);
        if (first < last)
            storage.erase(first, last);
    }

    size_type erase(const T &value) {
        auto it = find(value);
        if (it != end()) {
            storage.erase(it);
            return 1;
        }
        return 0;
    }

    // Lookups

    [[nodiscard]] iterator find(const T &value) {
        auto it = lowerBound(value);
        if (it != end() && !comp(value, *it)) {
            return it;
        }
        return end();
    }

    [[nodiscard]] const_iterator find(const T &value) const {
        auto it = lowerBound(value);
        if (it != end() && !comp(value, *it)) {
            return it;
        }
        return end();
    }

    [[nodiscard]] bool contains(const T &value) const { return find(value) != end(); }

    [[nodiscard]] iterator lowerBound(const T &value) { return std::ranges::lower_bound(storage, value, comp); }

    [[nodiscard]] const_iterator lowerBound(const T &value) const {
        return std::ranges::lower_bound(storage, value, comp);
    }

    [[nodiscard]] iterator upperBound(const T &value) { return std::ranges::upper_bound(storage, value, comp); }

    [[nodiscard]] const_iterator upperBound(const T &value) const {
        return std::ranges::upper_bound(storage, value, comp);
    }

    // Element Access
    T &operator[](size_type pos) { return storage[pos]; }
    const T &operator[](size_type pos) const { return storage[pos]; }

    T &back() { return storage.back(); }
    [[nodiscard]] const T &back() const { return storage.back(); }

    T &front() { return storage.front(); }
    [[nodiscard]] const T &front() const { return storage.front(); }

    [[nodiscard]] const container_type &data() const noexcept { return storage; }

    [[nodiscard]] bool operator==(const VectorSet &other) const noexcept { return storage == other.storage; }

  private:
    /**
     * @brief Sorts the underlying vector and removes duplicates.
     */
    void sort() {
        std::ranges::sort(storage, comp);
        const auto dups =
            std::ranges::unique(storage, [this](const T &a, const T &b) { return !comp(a, b) && !comp(b, a); });
        storage.erase(dups.begin(), dups.end());
    }

    container_type storage;
    Compare comp;
};
} // namespace ofs
