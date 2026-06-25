#include "CrashHandler.h"

#ifdef _WIN32

#include "Log.h"
#include "PathUtil.h"

#include <crtdbg.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on

namespace {

// True only in the real app (set by installCrashHandler). The headless test runner leaves it false so
// a crashing test can never spawn a modal message box or write a .dmp — it gets the trace only. Gates
// the dump+notify on every fatal path (SEH fault, assert, abort, std::terminate), not just the SEH one.
bool dumpOnFatal = false;

// Build a symbolized stack trace. When `contextRecord` is given (an SEH crash) the faulting thread is
// walked via StackWalk64; otherwise (an abort/assert) the live stack is captured directly. Best-effort
// and crash-path: any symbol we can't resolve becomes an address + offset, and it tolerates partial
// failure. The result is returned as one multiline string so callers can route it to stderr, the log,
// or both.
std::string buildStackTrace(CONTEXT *contextRecord) {
    HANDLE proc = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, nullptr, TRUE);

    alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256];
    auto *sym = reinterpret_cast<SYMBOL_INFO *>(symBuf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;

    std::string out = "\n--- stack trace ---\n";
    char lineBuf[1200];
    auto emit = [&](int i, DWORD64 addr) {
        // Always resolve module + RVA (no PDB needed). If the PDB is missing on the crashing
        // machine, "module+0xRVA" is still enough to symbolize offline against the archived PDB —
        // so users never need PDBs, only the developer keeping the matching build's pdb.
        char modName[MAX_PATH] = "?";
        const DWORD64 base = SymGetModuleBase64(proc, addr);
        const DWORD64 rva = base != 0 ? addr - base : 0;
        if (base != 0 && GetModuleFileNameA(reinterpret_cast<HMODULE>(base), modName, sizeof(modName)) != 0) {
            if (const char *slash = strrchr(modName, '\\'))
                memmove(modName, slash + 1, strlen(slash + 1) + 1);
        }

        DWORD64 disp = 0;
        if (SymFromAddr(proc, addr, &disp, sym)) {
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisp = 0;
            if (SymGetLineFromAddr64(proc, addr, &lineDisp, &line))
                snprintf(lineBuf, sizeof(lineBuf), "  #%-2d %s!%s  (%s:%lu)\n", i, modName, sym->Name, line.FileName,
                         line.LineNumber);
            else
                snprintf(lineBuf, sizeof(lineBuf), "  #%-2d %s!%s+0x%llx  [+0x%llx]\n", i, modName, sym->Name,
                         static_cast<unsigned long long>(disp), static_cast<unsigned long long>(rva));
        } else {
            snprintf(lineBuf, sizeof(lineBuf), "  #%-2d %s+0x%llx\n", i, modName, static_cast<unsigned long long>(rva));
        }
        out += lineBuf;
    };

    if (contextRecord != nullptr) {
        CONTEXT ctx = *contextRecord;
        STACKFRAME64 frame{};
        frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
        frame.AddrPC.Offset = ctx.Rip;
        frame.AddrFrame.Offset = ctx.Rbp;
        frame.AddrStack.Offset = ctx.Rsp;
        for (int i = 0; i < 62; ++i) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &frame, &ctx, nullptr, SymFunctionTableAccess64,
                             SymGetModuleBase64, nullptr) ||
                frame.AddrPC.Offset == 0)
                break;
            emit(i, frame.AddrPC.Offset);
        }
    } else {
        void *backtrace[62];
        const USHORT n = CaptureStackBackTrace(1, 62, backtrace, nullptr); // skip this frame
        for (USHORT i = 0; i < n; ++i)
            emit(i, reinterpret_cast<DWORD64>(backtrace[i]));
    }
    out += "-------------------\n";
    SymCleanup(proc);
    return out;
}

// Write a trace to the log (so it survives in ofs.log on release builds, where there's no console)
// and, in debug builds, to stderr for an immediate read.
void reportStackTrace(const char *headline, CONTEXT *contextRecord) {
    const std::string trace = buildStackTrace(contextRecord);
    if (ofs::Log::getCoreLogger() != nullptr) {
        OFS_CORE_CRITICAL("{}{}", headline, trace);
        ofs::Log::getCoreLogger()->flush();
    }
#ifndef NDEBUG
    fputs(headline, stderr);
    fputs(trace.c_str(), stderr);
    fflush(stderr);
#endif
}

// Write a minidump next to a copy of the log, then show the "crashed" message box. `ep` may be null on
// a fatal path that carries no SEH context (assert / abort / std::terminate): the dump then holds every
// thread's stack but no faulting-thread annotation. Callers are responsible for the trace; this only
// does the dump + notification, so it must run only when `dumpOnFatal` is set (the app, never tests).
void writeDumpAndNotify(EXCEPTION_POINTERS *ep) {
    const std::filesystem::path &dir = ofs::util::getCrashDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t baseName[64];
    swprintf(baseName, std::size(baseName), L"ofs-ng-%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond);
    const std::filesystem::path dumpPath = dir / (std::wstring(baseName) + L".dmp");

    HANDLE file =
        CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool wrote = false;
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{};
        MINIDUMP_EXCEPTION_INFORMATION *infoPtr = nullptr;
        if (ep != nullptr) {
            info.ThreadId = GetCurrentThreadId();
            info.ExceptionPointers = ep;
            info.ClientPointers = FALSE;
            infoPtr = &info;
        }

        // Normal stacks + thread info + memory referenced by stack pointers. This keeps the
        // dump small while still giving the developer locals and nearby heap to read.
        const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo |
                                                     MiniDumpWithIndirectlyReferencedMemory);

        wrote = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, infoPtr, nullptr, nullptr);
        CloseHandle(file);
    }

    // Copy the live log next to the dump so the user has both in one folder. The log truncates on
    // the next launch, so this snapshot preserves it for this crash. spdlog opens the log shared
    // (_SH_DENYNO), so it can be read while still open. Paired by name with the dump.
    const std::filesystem::path logCopyPath = dir / (std::wstring(baseName) + L".log");
    CopyFileW((ofs::util::getPrefPath() / L"ofs.log").c_str(), logCopyPath.c_str(), FALSE);

    wchar_t message[1024];
    if (wrote) {
        swprintf(message, std::size(message),
                 L"ofs-ng crashed.\n\nA crash dump was saved to:\n%ls\n\n"
                 L"This file, together with the matching .log file in the same folder, can help diagnose "
                 L"the issue. Sharing them is optional.\n\n"
                 L"The dump contains a portion of the application's memory and may include file paths, "
                 L"your user name, and data that was open at the time. If you choose to share it, please "
                 L"use a private channel.",
                 dumpPath.c_str());
    } else {
        swprintf(message, std::size(message), L"ofs-ng crashed, and writing the crash dump to\n%ls\nfailed.",
                 dumpPath.c_str());
    }
    MessageBoxW(nullptr, message, L"ofs-ng crashed", MB_OK | MB_ICONERROR);
}

// For the non-SEH fatal paths (assert / abort / std::terminate): in the app, capture the current
// context and write a dump + notification so these get the same artifacts as a hardware fault. The
// captured context makes the dump open on the crashing thread (a few handler frames above the real
// site, which the log's symbolized trace already pinpoints). No-op in the test runner.
void maybeWriteDumpForFatal() {
    if (!dumpOnFatal)
        return;
    // One dump + box per death: onTerminate() calls std::abort(), which re-enters onAbortSignal via
    // SIGABRT in debug. Clearing the gate up front collapses that cascade to a single artifact.
    dumpOnFatal = false;
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = 0xE0000001; // customer-defined (bit 29) synthetic code; no real SEH exception here
    EXCEPTION_POINTERS ep{.ExceptionRecord = &rec, .ContextRecord = &ctx};
    writeDumpAndNotify(&ep);
}

// Last-chance handler for an uncaught C++ exception (std::terminate). Reachable in every build, so it
// gets the trace into ofs.log on release too. The stack is the live one (no SEH context here).
[[noreturn]] void onTerminate() {
    const char *what = "unknown";
    if (std::exception_ptr ep = std::current_exception()) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception &e) {
            what = e.what();
        } catch (...) {
        }
    }
    char headline[512];
    snprintf(headline, sizeof(headline), "std::terminate — unhandled exception: %s", what);
    reportStackTrace(headline, nullptr);
    maybeWriteDumpForFatal();
    std::abort();
}

#ifndef NDEBUG
// MSVC routes STL/CRT assertions — std::clamp's bounds check, a failed assert(), any _STL_VERIFY —
// through _CrtDbgReport and *then* terminates via __fastfail(), which no signal or SEH handler can
// intercept. The report hook is the one point where we can still run code, so we emit the trace here.
// Returning TRUE suppresses the default report (and any message box that would hang a headless run);
// the STL fast-fails immediately after, so the process still dies as expected. (These STL assertions
// only exist in debug — _ITERATOR_DEBUG_LEVEL == 2 — so this hook is debug-only.)
// Signature is fixed by _CRT_REPORT_HOOK (char*, not const), so the parameter can't be const-qualified.
// NOLINTNEXTLINE(readability-non-const-parameter)
int __cdecl crtReportHook(int reportType, char *message, int *returnValue) {
    if (returnValue != nullptr)
        *returnValue = 0; // do not request a debugger break
    if (reportType == _CRT_ASSERT || reportType == _CRT_ERROR) {
        reportStackTrace(message != nullptr ? message : "assertion failure", nullptr);
        maybeWriteDumpForFatal();
    }
    return TRUE;
}

// Fallback for a plain abort() that raises SIGABRT (the STL fast-fail path bypasses this). Emit a
// trace, then restore the default handler and re-raise so the process still terminates.
void onAbortSignal(int sig) {
    reportStackTrace("*** abort() ***", nullptr);
    maybeWriteDumpForFatal();
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

LONG WINAPI writeMiniDump(EXCEPTION_POINTERS *exceptionPointers) {
    // Symbolized trace into ofs.log first (release builds have no console, and the minidump needs
    // external tooling to read) — then copied next to the dump below, and shown on stderr in debug.
    reportStackTrace("Unhandled exception. Stack trace:", exceptionPointers->ContextRecord);
    writeDumpAndNotify(exceptionPointers);
    // Let the default handler run so the process terminates (and any debugger still attaches).
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

namespace ofs {

void installAssertStackTrace() {
#ifndef NDEBUG
    // Send assertion reports to stderr (never a modal dialog that would hang a headless run) and
    // route them through our hook, which prints a symbolized trace before the STL fast-fails.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, crtReportHook);

    // Cover a direct abort() too (raises SIGABRT, which fast-fail does not). Clear _CALL_REPORTFAULT
    // so abort() reaches our handler rather than Windows Error Reporting.
    _set_abort_behavior(_WRITE_ABORT_MSG, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    signal(SIGABRT, onAbortSignal);
#endif
}

void installCrashHandler() {
    dumpOnFatal = true; // app build: assert/abort/terminate also write a dump + show the message box
    SetUnhandledExceptionFilter(writeMiniDump);
    std::set_terminate(onTerminate); // uncaught C++ exceptions → trace in the log + dump (all builds)
    installAssertStackTrace();
}

namespace {
// Set by installCrashHandlerHeadless. Kept at file scope because the SEH filter is a captureless
// lambda (must convert to a plain function pointer) and so cannot capture the hook.
void (*headlessCrashHook)() = nullptr;
} // namespace

void installCrashHandlerHeadless(void (*onCrash)()) {
    headlessCrashHook = onCrash;
    // Same SEH coverage as installCrashHandler() — so a hardware fault (access violation) is caught
    // and symbolized — but routed through reportAndContinue, which only writes the trace (no dump,
    // no MessageBox) so it can't block a headless test run.
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS *ep) -> LONG {
        // Let the test engine record/export the crashed test before we symbolize and die.
        if (headlessCrashHook)
            headlessCrashHook();
        reportStackTrace("Unhandled exception (test runner). Stack trace:", ep->ContextRecord);
        return EXCEPTION_EXECUTE_HANDLER; // terminate the process; no dialog, no dump
    });
    std::set_terminate(onTerminate);
    installAssertStackTrace();
}

} // namespace ofs

#else // !_WIN32

namespace ofs {

void installCrashHandler() {}
void installCrashHandlerHeadless(void (*)()) {}
void installAssertStackTrace() {}

} // namespace ofs

#endif
