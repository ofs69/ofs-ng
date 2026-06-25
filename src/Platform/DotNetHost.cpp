#include "Platform/DotNetHost.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"
#include <SDL3/SDL_loadso.h>
#include <mutex>

namespace ofs {

// hostfxr status codes (corehost_error_codes.h). init_for_runtime_config returns one of the three
// "success" variants depending on whether a runtime was already up with matching properties.
namespace hostfxr_status {
constexpr int kSuccess = 0;
constexpr int kSuccessHostAlreadyInitialized = 1;
constexpr int kSuccessDifferentRuntimeProperties = 2;
} // namespace hostfxr_status

namespace {

// The CoreCLR runtime is a process singleton: hostfxr cannot start a second runtime, nor re-initialize
// after a context is closed. So we boot it exactly once into this file-scope state, shared by every
// DotNetHost (PluginManager's and ScriptSystem's alike), and never close it — the OS reclaims it at
// process exit. Closing the context in a destructor was the bug behind "only the first .NET test per
// process runs": a torn-down runtime can't be re-initialized, so every later DotNetHost::init() failed.
// The single `load_assembly` delegate is assembly-agnostic — each assembly resolves its own
// dependencies via its .deps.json regardless of which runtimeconfig booted the runtime — so one boot
// serves both Ofs.PluginHost.dll and Ofs.ScriptHost.dll.
struct ClrRuntime {
    hostfxr_initialize_for_runtime_config_fn initFptr = nullptr;
    hostfxr_get_runtime_delegate_fn getDelegateFptr = nullptr;
    hostfxr_close_fn closeFptr = nullptr;
    load_assembly_and_get_function_pointer_fn loadAssemblyFn = nullptr;
    hostfxr_handle context = nullptr;
    SDL_SharedObject *hostfxrLib = nullptr;
    bool fxrLoaded = false;
    bool runtimeUp = false;
};

ClrRuntime &clr() {
    static ClrRuntime rt;
    return rt;
}

// init()/loadAssembly()/getFunctionPointer() all run on the main thread at startup, but worker threads
// hold the resolved function pointers; guard the boot so the global state is published safely.
std::mutex &clrMutex() {
    static std::mutex m;
    return m;
}

bool loadHostFxrLocked(ClrRuntime &rt) {
    if (rt.fxrLoaded)
        return rt.initFptr && rt.getDelegateFptr && rt.closeFptr;
    rt.fxrLoaded = true;

    char_t buffer[32768];
    size_t bufferSize = sizeof(buffer) / sizeof(char_t);

    // libnethost is statically linked (NETHOST_USE_AS_STATIC), so call get_hostfxr_path() directly on
    // every platform. It runs .NET's standard hostfxr discovery (DOTNET_ROOT, the registered global
    // install, the default install location, PATH) — no absolute paths are baked into the binary and
    // it resolves against whatever dotnet the target machine provides. This avoids dlopen'ing a
    // nethost shared library, which on Linux lives in the dotnet host pack, off the loader path.
    int rc = get_hostfxr_path(buffer, &bufferSize, nullptr);
    if (rc != 0) {
        OFS_CORE_ERROR("Failed to get hostfxr path: {0}", rc);
        return false;
    }

    // char_t is wchar_t on Windows (DotNetHost.h), so this is a lossless wide-string path
    // construction; on other platforms char_t is char and buffer is OS-native (UTF-8).
    rt.hostfxrLib = SDL_LoadObject(ofs::util::toUtf8(std::filesystem::path(buffer)).c_str()); // utf8-ok
    if (!rt.hostfxrLib) {
        OFS_CORE_ERROR("Failed to load hostfxr: {}", SDL_GetError());
        return false;
    }
    rt.initFptr = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
        SDL_LoadFunction(rt.hostfxrLib, "hostfxr_initialize_for_runtime_config"));
    rt.getDelegateFptr = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
        SDL_LoadFunction(rt.hostfxrLib, "hostfxr_get_runtime_delegate"));
    rt.closeFptr = reinterpret_cast<hostfxr_close_fn>(SDL_LoadFunction(rt.hostfxrLib, "hostfxr_close"));

    return (rt.initFptr && rt.getDelegateFptr && rt.closeFptr);
}

} // namespace

bool DotNetHost::init() {
    std::lock_guard<std::mutex> lock(clrMutex());
    if (!loadHostFxrLocked(clr())) {
        OFS_CORE_ERROR("Failed to load hostfxr");
        return false;
    }
    return true;
}

bool DotNetHost::loadAssembly(const std::filesystem::path &assemblyPath) {
    auto configPath = assemblyPath;
    configPath.replace_extension(".runtimeconfig.json");

    if (!std::filesystem::exists(configPath)) {
        OFS_CORE_ERROR("Runtime config not found: {0}", ofs::util::toUtf8(std::filesystem::absolute(configPath)));
        return false;
    }

    std::lock_guard<std::mutex> lock(clrMutex());
    auto &rt = clr();
    if (!loadHostFxrLocked(rt))
        return false;

    // Boot the runtime once for the whole process. A later call gets Success_HostAlreadyInitialized
    // and a secondary context whose runtimeconfig is ignored, so there is nothing more to do — the
    // first boot's load delegate already loads any assembly by absolute path.
    if (!rt.runtimeUp) {
        int rc = rt.initFptr(configPath.c_str(), nullptr, &rt.context);
        using namespace hostfxr_status;
        if ((rc != kSuccess && rc != kSuccessHostAlreadyInitialized && rc != kSuccessDifferentRuntimeProperties) ||
            rt.context == nullptr) {
            OFS_CORE_ERROR("Failed to initialize hostfxr: {0}", rc);
            rt.context = nullptr;
            return false;
        }

        rc = rt.getDelegateFptr(rt.context, hdt_load_assembly_and_get_function_pointer, (void **)&rt.loadAssemblyFn);
        if (rc != 0 || rt.loadAssemblyFn == nullptr) {
            OFS_CORE_ERROR("Failed to get load_assembly_and_get_function_pointer: {0}", rc);
            rt.context = nullptr;
            return false;
        }
        rt.runtimeUp = true;
    }

    return true;
}

bool DotNetHost::getFunctionPointerRaw(const char_t *assemblyPath, const char_t *typeName, const char_t *methodName,
                                       void **outPtr) {
    std::lock_guard<std::mutex> lock(clrMutex());
    auto &rt = clr();
    if (!rt.loadAssemblyFn)
        return false;

    int rc = rt.loadAssemblyFn(assemblyPath, typeName, methodName, UNMANAGEDCALLERSONLY_METHOD, nullptr, outPtr);

    return rc == 0;
}

} // namespace ofs
