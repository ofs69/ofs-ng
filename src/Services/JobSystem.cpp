#include "Services/JobSystem.h"
#include <BS_thread_pool.hpp>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace ofs {

struct JobSystem::Impl {
    std::size_t threadCount;
    std::unique_ptr<BS::thread_pool<>> pool;
    bool stopped = false; // set by shutdown(); distinguishes "torn down" from "never started" for submit()

    // Start time of each eval job currently executing, keyed by a monotonic submit id. A worker inserts on
    // entry and erases on exit (RAII), so the map holds exactly the in-flight eval workers — including ones
    // whose EvalJob was already superseded, which axis.pendingEval no longer tracks. Touched from worker
    // threads and read from the main thread, hence the mutex.
    mutable std::mutex evalMtx;
    std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> evalStarts;
    std::atomic<std::uint64_t> nextEvalId{0};

    explicit Impl(std::size_t n) : threadCount(n == 0 ? std::thread::hardware_concurrency() : n) {}
};

JobSystem::JobSystem(std::size_t threadCount) : impl_(std::make_unique<Impl>(threadCount)) {}

void JobSystem::start() {
    assert(!impl_->pool && "JobSystem::start called more than once");
    impl_->pool = std::make_unique<BS::thread_pool<>>(impl_->threadCount);
}

void JobSystem::shutdown() {
    if (!impl_->pool)
        return;
    impl_->stopped = true; // reject any late submission (e.g. from a service dtor) as a no-op
    impl_->pool->purge();  // discard eval jobs not yet started
    impl_->pool->wait();   // block until every in-flight worker returns
    impl_->pool.reset();   // join the threads
}

JobSystem::~JobSystem() {
    shutdown();
}

void JobSystem::submit(const std::shared_ptr<EvalJob> &job, AxisSnapshot snap, std::function<void(AxisSnapshot)> fn) {
    if (impl_->stopped)
        return; // teardown in progress: drop the work rather than touch a torn-down pool
    assert(impl_->pool && "JobSystem::submit called before start()");
    Impl *impl = impl_.get(); // outlives the task: shutdown() joins the pool before ~JobSystem destroys Impl
    const std::uint64_t id = impl->nextEvalId.fetch_add(1, std::memory_order_relaxed);
    impl_->pool->detach_task([impl, id, job, snap = std::move(snap), fn = std::move(fn)]() mutable {
        {
            std::lock_guard lk(impl->evalMtx);
            impl->evalStarts.emplace(id, std::chrono::steady_clock::now());
        }
        // Erase on every exit path (normal return, early-cancel, exception) so the only entry that lingers is
        // a worker that never returns — precisely the stuck-thread case the footer warns about.
        struct Eraser {
            Impl *impl;
            std::uint64_t id;
            ~Eraser() {
                std::lock_guard lk(impl->evalMtx);
                impl->evalStarts.erase(id);
            }
        } eraser{.impl = impl, .id = id};
        if (!job->isCancelled())
            fn(std::move(snap));
    });
}

JobSystem::WorkerStats JobSystem::evalWorkerStats() const {
    std::lock_guard lk(impl_->evalMtx);
    const auto now = std::chrono::steady_clock::now();
    WorkerStats s{.running = impl_->evalStarts.size()};
    for (const auto &[id, start] : impl_->evalStarts)
        s.oldestAgeSeconds = std::max(s.oldestAgeSeconds, std::chrono::duration<double>(now - start).count());
    return s;
}

std::future<bool> JobSystem::submitTask(std::function<bool()> fn) {
    if (impl_->stopped) {
        std::promise<bool> p;
        p.set_value(false);
        return p.get_future();
    }
    assert(impl_->pool && "JobSystem::submitTask called before start()");
    return impl_->pool->submit_task(std::move(fn));
}

} // namespace ofs
