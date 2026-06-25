// Tests for JobSystem::evalWorkerStats — the footer's live view of in-flight eval workers (count + age
// of the longest-running one) that drives the stuck-worker warning. A blocking worker lets us observe a
// job while it runs and confirm it is no longer counted once it returns.
#include "Services/JobSystem.h"
#include <atomic>
#include <chrono>
#include <doctest/doctest.h>
#include <functional>
#include <memory>
#include <thread>

using namespace ofs;

namespace {

bool waitUntil(const std::function<bool()> &pred, int timeoutMs = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

} // namespace

TEST_CASE("evalWorkerStats counts a running eval worker and clears it when the worker returns") {
    JobSystem jobs;
    jobs.start();

    CHECK(jobs.evalWorkerStats().running == 0);

    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    auto job = std::make_shared<EvalJob>();
    jobs.submit(job, AxisSnapshot{}, [&](AxisSnapshot) {
        entered.store(true);
        while (!release.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });

    REQUIRE(waitUntil([&] { return entered.load(); }));
    const auto s = jobs.evalWorkerStats();
    CHECK(s.running == 1);
    CHECK(s.oldestAgeSeconds >= 0.0); // a just-started worker has a small, non-negative age

    release.store(true);
    CHECK(waitUntil([&] { return jobs.evalWorkerStats().running == 0; }));
}

TEST_CASE("evalWorkerStats does not count a job cancelled before it starts") {
    JobSystem jobs;
    jobs.start();

    auto job = std::make_shared<EvalJob>();
    job->cancel(); // superseded before the pool picks it up: fn must be skipped and nothing should linger
    std::atomic<bool> ran{false};
    jobs.submit(job, AxisSnapshot{}, [&](AxisSnapshot) { ran.store(true); });

    // The task body still briefly inserts/erases its bookkeeping entry, so the only stable post-condition
    // is that it settles back to zero and fn never ran.
    CHECK(waitUntil([&] { return jobs.evalWorkerStats().running == 0; }));
    CHECK_FALSE(ran.load());
}
