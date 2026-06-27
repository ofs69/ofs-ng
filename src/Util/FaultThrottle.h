#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace ofs::util {

// Coalesces repeated fault notifications keyed by source, so a node/script throwing every sample on a
// worker thread can't flood the notification center. Thread-safe: faults may be reported off the main
// thread. Used by both the plugin host and the script host, which differ only in message wording.
class FaultThrottle {
  public:
    explicit FaultThrottle(std::chrono::seconds window = std::chrono::seconds(3)) : window_(window) {}

    // Register a fault for `key`. Returns nullopt while inside the active window (the fault is suppressed
    // but counted); otherwise returns the number of faults suppressed since the previous emit (0 the first
    // time), and the caller should emit exactly one notification carrying that count.
    // `now` defaults to the current time; it is a parameter only so tests can drive the window
    // deterministically without sleeping.
    std::optional<int> onFault(const std::string &key,
                               std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto &st = state_[key];
        if (st.lastEmit.time_since_epoch().count() != 0 && now - st.lastEmit < window_) {
            ++st.suppressed;
            return std::nullopt;
        }
        const int suppressed = st.suppressed;
        st.suppressed = 0;
        st.lastEmit = now;
        return suppressed;
    }

  private:
    struct Entry {
        std::chrono::steady_clock::time_point lastEmit{};
        int suppressed = 0;
    };
    std::chrono::seconds window_;
    std::mutex mutex_;
    std::unordered_map<std::string, Entry> state_;
};

} // namespace ofs::util
