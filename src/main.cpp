#include "App/OfsApp.h"
#include "Core/EventQueue.h"
#include "Core/ScriptProject.h"
#include "Util/CrashHandler.h"
#include "Util/InstanceLock.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"
#include "Util/PrefVersionGuard.h"
#include <OfsBuildInfo.h> // generated: git commit (long/short) + tag baked into the binary
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_messagebox.h>
#include <algorithm>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char *argv[]) {
    ofs::installCrashHandler();

    // Single-instance guard: refuse to start if another copy is already running. This MUST precede
    // Log::init() — the file sink rotates (renames/reopens) ofs.log on open, which throws while a
    // first instance holds the log, so a second launch would crash in logging before ever reaching a
    // guard placed later. Held for the whole process lifetime; the OS releases the lock on exit
    // (including a crash), so it never self-strands.
    ofs::InstanceLock instanceLock(ofs::util::getPrefPath() / "instance.lock");
    if (!instanceLock.acquired()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "ofs-ng", "ofs-ng is already running.", nullptr);
        return 0;
    }

    ofs::Log::init();

    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc) - 1);
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    // Deliberately crash to verify the crash handler / minidump path. Kept available in
    // Release so the shipped build's dump pipeline can be tested.
    if (std::ranges::find(args, "--crash-test") != args.end()) {
        OFS_CORE_WARN("--crash-test: forcing an access violation");
        volatile int *p = nullptr;
        *p = 42;
    }

#if defined(_WIN32) && !defined(NDEBUG)
    if (std::ranges::find(args, "--wait-debugger") != args.end()) {
        OFS_CORE_INFO("Waiting for debugger to attach (PID: {})...", GetCurrentProcessId());
        while (!IsDebuggerPresent()) {
            Sleep(100);
        }
        DebugBreak();
    }
#endif

    OFS_CORE_INFO("Starting ofs-ng...");
#ifdef NDEBUG
    OFS_CORE_INFO("Build type: Release");
#else
    OFS_CORE_INFO("Build type: Debug");
#endif
    using std::string_view_literals::operator""sv;
    OFS_CORE_INFO("Revision: {} ({}), tag: {}",
                  ofs::generated::kGitCommitShort.empty() ? "unknown"sv : ofs::generated::kGitCommitShort,
                  ofs::generated::kGitCommitLong.empty() ? "unknown"sv : ofs::generated::kGitCommitLong,
                  ofs::generated::kGitTag.empty() ? "none"sv : ofs::generated::kGitTag);

    // Refuse to start if this pref dir was last written by a newer, incompatible build. Running an
    // older build would let it overwrite the newer preferences with its old format and lose data, so
    // we bail out before touching anything rather than clobber. (Must follow Log::init for diagnostics
    // and precede any pref write.)
    const std::string_view thisVersion = ofs::generated::kGitTag.empty() ? "this build"sv : ofs::generated::kGitTag;
    if (const ofs::PrefVersionStatus pref = ofs::checkPrefDirVersion(thisVersion); !pref.ok) {
        const std::string msg = fmt::format(
            "Your settings folder was last used by a newer version of ofs-ng ({}).\n\n"
            "Running this older version could overwrite and corrupt those settings, so it "
            "will not start.\n\nPlease update to the newer version, or move the settings "
            "folder aside to start fresh:\n{}",
            pref.writtenBy.empty() ? "unknown" : pref.writtenBy, ofs::util::toUtf8(ofs::util::getPrefPath()));
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "ofs-ng", msg.c_str(), nullptr);
        return 0;
    }

    ofs::ScriptProject project;
    ofs::EventQueue eventQueue;
    OfsApp app(project, eventQueue);
    if (app.init()) {
        return app.run();
    }
    return -1;
}
