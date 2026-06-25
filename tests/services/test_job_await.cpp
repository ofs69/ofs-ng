// Tests for JobAwait — the awaitable that lets a co::Fire flow run blocking work on a JobSystem
// worker and resume on the main thread (the primitive behind ProjectManager's async load / sibling
// discovery / import / export). The flows themselves are exercised end-to-end in
// test_project_manager*.cpp; these pin the mechanism in isolation.

#include "Core/EventQueue.h"
#include "Services/JobAwait.h"
#include "Services/JobSystem.h"
#include "Util/Coro.h"
#include <chrono>
#include <doctest/doctest.h>
#include <functional>
#include <optional>
#include <stdexcept>
#include <thread>

using namespace ofs;

namespace {

// Pump drain() until `done` or timeout. The worker resumes the flow via ResumeFlowEvent, which is
// only applied by a drain() after the worker has pushed it — so a single drain never suffices.
bool drainUntil(EventQueue &eq, const std::function<bool()> &done, int timeoutMs = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do {
        eq.drain();
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

// Captures, by reference, where each stage of a JobAwait flow ran. Passed by reference as a coroutine
// PARAMETER (copied into the frame) rather than captured by a coroutine lambda — a capturing lambda
// coroutine would dangle once it suspends, since the closure temporary dies at the end of the call.
struct Probe {
    std::thread::id workerId;
    std::thread::id resumeId;
    int result = 0;
    bool done = false;
};

co::Fire runComputation(JobSystem &jobs, EventQueue &eq, Probe &p) {
    p.result = co_await JobAwait<int>{jobs, eq, [&p] {
                                          p.workerId = std::this_thread::get_id();
                                          return 6 * 7;
                                      }};
    p.resumeId = std::this_thread::get_id();
    p.done = true;
}

co::Fire runThrowing(JobSystem &jobs, EventQueue &eq, std::optional<int> &out, bool &done) {
    // fn throws — JobAwait must still resume the flow, leaving result default-constructed (nullopt).
    out = co_await JobAwait<std::optional<int>>{jobs, eq,
                                                []() -> std::optional<int> { throw std::runtime_error("boom"); }};
    done = true;
}

} // namespace

TEST_CASE("JobAwait runs fn on a worker thread and resumes the flow on the main thread") {
    EventQueue eq;
    eq.on<ResumeFlowEvent>([](const ResumeFlowEvent &e) {
        if (e.handle)
            e.handle.resume();
    });
    eq.freeze();
    JobSystem jobs;
    jobs.start();

    const std::thread::id mainId = std::this_thread::get_id();
    Probe p;
    runComputation(jobs, eq, p); // fire-and-forget: runs to the co_await, then suspends

    REQUIRE(drainUntil(eq, [&] { return p.done; }));
    CHECK(p.result == 42);       // the worker's return value reached the resumed flow
    CHECK(p.workerId != mainId); // fn genuinely ran off the main thread
    CHECK(p.resumeId == mainId); // the flow resumed on the drain (main) thread
}

TEST_CASE("JobAwait resumes the flow even when fn throws, with a default result") {
    EventQueue eq;
    eq.on<ResumeFlowEvent>([](const ResumeFlowEvent &e) {
        if (e.handle)
            e.handle.resume();
    });
    eq.freeze();
    JobSystem jobs;
    jobs.start();

    std::optional<int> out = 123; // sentinel: must be overwritten with the default (nullopt)
    bool done = false;
    runThrowing(jobs, eq, out, done);

    REQUIRE(drainUntil(eq, [&] { return done; })); // a throwing worker must not strand the flow
    CHECK_FALSE(out.has_value());                  // default-constructed result signals the failure
}
