#pragma once

namespace ofs {

// Installs the app's process-wide crash handler. Every fatal path — an unhandled SEH exception
// (access violation), a CRT/STL/user assertion, a bare abort(), or an uncaught C++ exception
// (std::terminate) — writes a minidump (.dmp) plus a copy of the log to getCrashDir() and shows a
// message box, after logging a symbolized stack trace. On non-Windows platforms this is a no-op.
// Call once, as early as possible in main(). In debug builds this also installs the assert
// stack-trace handler below. (The headless variant deliberately suppresses the dump and box.)
void installCrashHandler();

// Debug builds only (no-op in release / non-Windows): when a CRT/STL/user assertion
// aborts the process — e.g. std::clamp's bounds check or a failed IM_ASSERT routed to
// abort() — print a symbolized stack trace to stderr before the process dies. On its own it pops
// no message box and writes no dump (only installCrashHandler() opts those in), so it is safe in
// headless test runners. Call once, early in main().
void installAssertStackTrace();

// Headless variant of installCrashHandler() for test runners: installs the SEH
// unhandled-exception filter (so an access violation — e.g. a null dereference in a test —
// prints a symbolized stack trace to stderr and the log) plus the assert handler, but
// writes no minidump and pops no message box that would hang a headless run.
//
// onCrash, if set, runs first inside the SEH filter — the test runner passes
// ImGuiTestEngine_CrashHandler so the engine can mark the in-flight test failed and
// export partial results before the symbolized trace is printed and the process dies.
void installCrashHandlerHeadless(void (*onCrash)() = nullptr);

} // namespace ofs
