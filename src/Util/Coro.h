#pragma once

#include <coroutine>

namespace ofs::co {

// Logs the in-flight exception (called from a coroutine's unhandled_exception). Defined in
// Coro.cpp so this header need not pull in the logging machinery. Swallows after logging — the
// frame then proceeds to final_suspend. Shared by Fire and Task.
void logCurrentFlowException(const char *flowKind) noexcept;

// Fire — a detached, eager-start, self-destroying coroutine for main-thread async flows.
//
// It is the generic primitive the modal system builds on, but it knows nothing about
// modals: any flow that drives a sequence of awaited results on the main thread can use it.
// A flow function returns `co::Fire`, starts running the moment it is called (eager
// `initial_suspend`), runs until its first `co_await`, then suspends. The frame is owned by
// whoever holds the awaiter's handle while suspended (the ModalManager, for modal awaiters)
// and self-destroys at `co_return` (`final_suspend` = `suspend_never`). The returned `Fire`
// object owns nothing — flows are fire-and-forget; the caller discards it.
//
// Two invariants flow authors MUST respect:
//   1. Code after a `co_await` may run in the SAME `drain()` (when the awaiter completed
//      without ever suspending) or in a LATER frame (after the user answered). Never assume
//      a frame boundary occurred across a `co_await`.
//   2. A flow body must not throw between an awaiter's `push` and its suspension. This holds
//      structurally — no user code runs between them — but keep it true when adding awaiters,
//      because a throw there would abandon an enqueued request pointing at a dying frame.
struct Fire {
    struct promise_type {
        Fire get_return_object() noexcept { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { logCurrentFlowException("co::Fire"); }
    };
};

} // namespace ofs::co
