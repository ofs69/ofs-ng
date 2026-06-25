#pragma once

#include <cstddef>

#include <coreclr_delegates.h>
#include <filesystem>
#include <hostfxr.h>
#include <nethost.h>
#include <string>
#include <vector>

#include <SDL3/SDL_loadso.h>

#ifdef _WIN32
#define STR(s) L##s
typedef wchar_t char_t;
#else
#define STR(s) s
typedef char char_t;
#endif

namespace ofs {

// A thin, stateless handle onto the process-global CoreCLR runtime. The runtime (hostfxr lib, context,
// and load-assembly delegate) is a process singleton owned by a file-scope static in the .cpp — it is
// booted once and never closed, because CoreCLR cannot be unloaded or re-initialized. Every owner
// (PluginManager, ScriptSystem) holds its own DotNetHost, but they all attach to the same runtime.
class DotNetHost {
  public:
    DotNetHost() = default;

    bool init();
    bool loadAssembly(const std::filesystem::path &assemblyPath);

    template <typename T>
    T getFunctionPointer(const std::filesystem::path &assemblyPath, const char_t *typeName, const char_t *methodName) {
        void *ptr = nullptr;
        auto absolutePath = std::filesystem::absolute(assemblyPath);
        if (getFunctionPointerRaw(absolutePath.c_str(), typeName, methodName, &ptr)) {
            return reinterpret_cast<T>(ptr);
        }
        return nullptr;
    }

  private:
    static bool getFunctionPointerRaw(const char_t *assemblyPath, const char_t *typeName, const char_t *methodName,
                                      void **outPtr);
};

} // namespace ofs
