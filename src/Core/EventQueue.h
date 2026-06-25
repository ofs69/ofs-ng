#pragma once

#include <cassert>
#include <concurrentqueue.h>
#include <functional>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace ofs {

class EventQueue {
  public:
    // Register a handler. Must be called before freeze().
    template <typename E> void on(std::function<void(const E &)> handler) {
        assert(!frozen_ && "EventQueue::on called after freeze()");
        handlers_[std::type_index(typeid(E))].push_back(
            [h = std::move(handler)](const void *e) { h(*static_cast<const E *>(e)); });
    }

    // Thread-safe. Safe to call from worker threads after freeze().
    template <typename E> void push(E event) {
        // Before freeze() the handler map is still being mutated by on() on the registration thread, so
        // a push() from any other thread here is a data race on handlers_ (a concurrent find() vs. a
        // rehashing insert) — undefined and otherwise invisible. After freeze() the map is immutable and
        // concurrent reads are safe, which is why cross-thread push() is only legal from that point on.
        // The classic trap is an mpv/preview wakeup callback (it runs on mpv's thread the instant the
        // callback is installed, well before freeze) pushing directly instead of just bumping an atomic.
        assert((frozen_ || std::this_thread::get_id() == ownerThread_) &&
               "EventQueue::push from a non-registration thread before freeze() — see EventQueue.h");
        const auto *hs = findHandlers(std::type_index(typeid(E)));
        if (hs == nullptr || hs->empty())
            return;
        queue_.enqueue([hs, ev = std::move(event)]() {
            for (const auto &h : *hs)
                h(&ev);
        });
    }

    void drain();

    // Lock handler registration. Call after all on() registrations and before starting worker threads.
    void freeze() { frozen_ = true; }

  private:
    using HandlerList = std::vector<std::function<void(const void *)>>;

    const HandlerList *findHandlers(std::type_index key) const {
        const auto it = handlers_.find(key);
        return it != handlers_.end() ? &it->second : nullptr;
    }

    std::unordered_map<std::type_index, HandlerList> handlers_;
    moodycamel::ConcurrentQueue<std::function<void()>> queue_;
    bool frozen_ = false;
    // The thread that constructs the queue (the main thread) is the only one allowed to register
    // handlers and to push() before freeze(); see the assert in push(). Captured at construction.
    std::thread::id ownerThread_ = std::this_thread::get_id();
};

} // namespace ofs
