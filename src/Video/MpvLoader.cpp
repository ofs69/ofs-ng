#include "MpvLoader.h"
#include "Util/Log.h"
#include <SDL3/SDL.h>
#include <string>

#ifdef _WIN32
#include "Util/PathUtil.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <filesystem>
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// Every entry point MpvLoader resolves. One list drives the definitions, the lookup and the reset, so a
// partially resolved library can never leave a stale pointer behind.
#define OFS_MPV_FUNCTIONS(X)                                                                                           \
    X(mpv_create)                                                                                                      \
    X(mpv_initialize)                                                                                                  \
    X(mpv_wait_event)                                                                                                  \
    X(mpv_observe_property)                                                                                            \
    X(mpv_render_context_update)                                                                                       \
    X(mpv_render_context_render)                                                                                       \
    X(mpv_set_option_string)                                                                                           \
    X(mpv_set_property_string)                                                                                         \
    X(mpv_set_property)                                                                                                \
    X(mpv_request_log_messages)                                                                                        \
    X(mpv_command_async)                                                                                               \
    X(mpv_render_context_create)                                                                                       \
    X(mpv_set_wakeup_callback)                                                                                         \
    X(mpv_render_context_set_update_callback)                                                                          \
    X(mpv_set_property_async)                                                                                          \
    X(mpv_render_context_free)                                                                                         \
    X(mpv_destroy)                                                                                                     \
    X(mpv_render_context_report_swap)                                                                                  \
    X(mpv_terminate_destroy)

namespace ofs {
#define OFS_MPV_DEFINE(name) name##_FUNC MpvLoader::name = nullptr;
OFS_MPV_FUNCTIONS(OFS_MPV_DEFINE)
#undef OFS_MPV_DEFINE

namespace {
enum class LoadState { NotAttempted, Loaded, Failed };

SDL_SharedObject *mpvHandle = nullptr;
LoadState loadState = LoadState::NotAttempted;

// Most-preferred first. On Linux .so.2 is the current ABI (and what the AppImage bundles); .so.1 is the
// pre-0.35 soname some distros still ship.
#if defined(_WIN32)
constexpr const char *kLibraryCandidates[] = {"libmpv-2.dll"};
#elif defined(__APPLE__)
constexpr const char *kLibraryCandidates[] = {"libmpv.dylib"};
#else
constexpr const char *kLibraryCandidates[] = {"libmpv.so.2", "libmpv.so.1"};
#endif

using mpv_client_api_version_FUNC = unsigned long (*)();

void resetFunctions() {
#define OFS_MPV_RESET(name) MpvLoader::name = nullptr;
    OFS_MPV_FUNCTIONS(OFS_MPV_RESET)
#undef OFS_MPV_RESET
}

// Returns the name of the first missing entry point, or nullptr when every one resolved.
const char *resolveFunctions() {
#define OFS_MPV_RESOLVE(name)                                                                                          \
    MpvLoader::name = reinterpret_cast<name##_FUNC>(SDL_LoadFunction(mpvHandle, #name));                               \
    if (!MpvLoader::name)                                                                                              \
        return #name;
    OFS_MPV_FUNCTIONS(OFS_MPV_RESOLVE)
#undef OFS_MPV_RESOLVE
    return nullptr;
}

// The file the dynamic loader actually bound, so a log shows whether a bundled or a host libmpv was used.
std::string loadedLibraryPath() {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(MpvLoader::mpv_create), &module))
        return {};
    std::wstring buffer(32768, L'\0');
    DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    buffer.resize(length);
    return util::toUtf8(std::filesystem::path(buffer)); // utf8-ok
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(MpvLoader::mpv_create), &info) == 0 || !info.dli_fname)
        return {};
    return info.dli_fname;
#endif
}

void logLoadedLibrary(const char *soname) {
    std::string path = loadedLibraryPath();
    if (path.empty())
        path = soname;
    auto apiVersion =
        reinterpret_cast<mpv_client_api_version_FUNC>(SDL_LoadFunction(mpvHandle, "mpv_client_api_version"));
    if (apiVersion) {
        unsigned long version = apiVersion();
        OFS_CORE_INFO("Loaded libmpv (client API {}.{}) from {}", version >> 16, version & 0xFFFF, path);
    } else {
        OFS_CORE_INFO("Loaded libmpv from {}", path);
    }
}
} // namespace

bool MpvLoader::load() noexcept {
    // A failed load is final: the loader environment does not change at runtime, and every caller
    // probing again would only repeat the same errors in the log.
    if (loadState != LoadState::NotAttempted)
        return loadState == LoadState::Loaded;
    loadState = LoadState::Failed;

    // Log every candidate's own error. SDL_GetError() only holds the last failure, so reporting it once
    // after the loop would show the fallback soname's "not found" and hide why the preferred one failed
    // (typically one of *its* dependencies missing or too old on this host).
    const char *loadedName = nullptr;
    for (const char *candidate : kLibraryCandidates) {
        mpvHandle = SDL_LoadObject(candidate);
        if (mpvHandle) {
            loadedName = candidate;
            break;
        }
        OFS_CORE_WARN("Could not load {}: {}", candidate, SDL_GetError());
    }

    if (!mpvHandle) {
        OFS_CORE_ERROR("libmpv is unavailable; video playback is disabled");
        return false;
    }

    if (const char *missing = resolveFunctions()) {
        OFS_CORE_ERROR("{} lacks the required function {}: {}; video playback is disabled", loadedName, missing,
                       SDL_GetError());
        resetFunctions();
        SDL_UnloadObject(mpvHandle);
        mpvHandle = nullptr;
        return false;
    }

    logLoadedLibrary(loadedName);
    loadState = LoadState::Loaded;
    return true;
}
} // namespace ofs
