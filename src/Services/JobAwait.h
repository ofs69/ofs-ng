#pragma once

#include "Core/EventQueue.h"
#include "Services/JobSystem.h"
#include "Util/Log.h"
#include <coroutine>
#include <functional>
#include <utility>

namespace ofs {

// Resumes a coroutine flow that suspended on a JobAwait. The worker pushes this the moment its task
// returns; the main-thread handler calls handle.resume(). The handle is a non-owning view into the
// suspended flow frame (alive until resumed) — like ShowModalEvent, this event must be drained
// exactly once and never duplicated in flight. A handler MUST be registered (ProjectManager does so
// in its constructor) or EventQueue::push drops the event and the flow never resumes.
struct ResumeFlowEvent {
    std::coroutine_handle<> handle{};
};

// co_await JobAwait<T>{jobSystem, eq, fn} — suspends the flow, runs fn() on a JobSystem worker (the
// frame loop keeps rendering), and resumes on the main thread with fn()'s return value. This is the
// coroutine-flow counterpart to JobSystem::submitTask: the bridge for moving a flow's blocking I/O
// or pure computation off the main thread without abandoning the linear co_await structure (used by
// ProjectManager for project load, sibling-funscript discovery, import parsing, and export writes).
//
// fn MUST obey the worker-thread rules: no ScriptProject access, no service calls — only file I/O
// and pure computation over data captured by value. Capture everything fn needs by value: the
// awaiter lives in the suspended frame, but fn runs on another thread. T must be default-
// constructible; model a "may fail" result as std::optional<T>.
template <typename T> struct JobAwait {
    JobSystem &jobs;
    EventQueue &eq;
    std::function<T()> fn;
    T result{};

    // User-declared so this stays a non-aggregate (keeps brace-init at call sites lint-clean).
    JobAwait(JobSystem &jobs, EventQueue &eq, std::function<T()> fn) : jobs(jobs), eq(eq), fn(std::move(fn)) {}

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        // Lifetime invariant the captured `this` rests on: the awaiter lives inside the suspended
        // coroutine frame, and the *only* thing that resumes (and so destroys) that frame is the
        // ResumeFlowEvent the worker pushes below. So `this` is guaranteed alive for the whole task —
        // the frame cannot be resumed from anywhere else while suspended on this await. Capturing
        // `this` by raw pointer is therefore safe here, but ONLY under that single-resumer invariant.
        // The worker writes `result` then pushes the resume; the queue's release/acquire makes that
        // write visible on the main thread before await_resume() reads it.
        // The push must happen even if fn throws, or the flow would suspend forever.
        jobs.submitTask([this, h]() {
            try {
                result = fn();
            } catch (const std::exception &e) {
                // A throw leaves `result` at its default (e.g. nullopt), which the flow handles as a
                // failure — but log it so a swallowed I/O/parse error isn't silent.
                OFS_CORE_ERROR("JobAwait task threw, resuming flow with default result: {}", e.what());
            } catch (...) {
                OFS_CORE_ERROR("JobAwait task threw a non-std exception, resuming flow with default result");
            }
            eq.push(ResumeFlowEvent{h});
            return true;
        });
    }
    T await_resume() { return std::move(result); }
};

} // namespace ofs
