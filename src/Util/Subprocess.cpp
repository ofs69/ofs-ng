#include "Util/Subprocess.h"
#include "Util/PathUtil.h"
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_stdinc.h>
#include <filesystem>
#include <map>
#include <utility>

namespace ofs::util {

namespace {
// Spawn a child with stdin+stdout piped back to the app (same as SDL_CreateProcess(args, true)), but on
// Windows additionally mark it "background" so SDL passes CREATE_NO_WINDOW. Without that flag, launching
// a console-subsystem tool (ffmpeg/ffprobe) from our GUI process — which owns no console — makes Windows
// allocate a fresh one, flashing a stray empty command prompt. Background only nulls *inherited* stdio,
// so the explicit APP pipes survive, and on Windows the exit code is still readable (the "exit code is
// always 0" caveat is POSIX-only). We must therefore NOT set it off-Windows, where it would null stdout
// and zero the exit code that runCaptured/runTranscode rely on.
SDL_Process *createPipedProcess(const char *const *args) {
    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props)
        return nullptr;
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
                           const_cast<void *>(static_cast<const void *>(args)));
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
#ifdef _WIN32
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
#endif
    SDL_Process *p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    return p;
}
} // namespace

std::string resolveTool(std::string_view tool) {
#ifdef _WIN32
    // Bundled next to the exe (downloaded at build, staged by dist). Build the filename in UTF-8 and
    // cross the path boundary with fromUtf8 so a non-ASCII install dir survives.
    return toUtf8(getBasePath() / fromUtf8(std::string(tool) + ".exe"));
#else
    // Resolved from PATH by the system package manager's install.
    return std::string(tool);
#endif
}

namespace {
bool probeTool(std::string_view tool) {
#ifdef _WIN32
    return std::filesystem::exists(getBasePath() / fromUtf8(std::string(tool) + ".exe"));
#else
    // No reliable path to check, so confirm it actually runs. "<tool> -version" exits 0 quickly.
    const std::string bin(tool);
    const char *args[] = {bin.c_str(), "-version", nullptr};
    std::string out;
    int code = 0;
    return runCaptured(args, out, code) && code == 0;
#endif
}
} // namespace

bool toolAvailable(std::string_view tool, bool forceRefresh) {
    // Main-thread only, so an unsynchronized static cache is safe. On Windows the probe is a cheap
    // exists() check; on POSIX it spawns once, then every later isEnabled call reads the cached bool.
    static std::map<std::string, bool, std::less<>> cache;
    if (!forceRefresh) {
        if (auto it = cache.find(tool); it != cache.end())
            return it->second;
    }
    const bool available = probeTool(tool);
    cache[std::string(tool)] = available;
    return available;
}

bool runCaptured(const char *const *args, std::string &out, int &exitCode) {
    SDL_Process *p = createPipedProcess(args);
    if (!p)
        return false;
    size_t len = 0;
    int code = 0;
    // Blocks until the child exits, returning all of stdout (NULL-terminated) and the exit code.
    void *data = SDL_ReadProcess(p, &len, &code);
    if (data) {
        out.assign(static_cast<const char *>(data), len);
        SDL_free(data);
    }
    SDL_DestroyProcess(p);
    exitCode = code;
    return data != nullptr; // NULL => the read/spawn failed
}

void Process::destroy() {
    if (!proc_)
        return;
    int code = 0;
    if (!SDL_WaitProcess(proc_, false, &code)) { // still running
        SDL_KillProcess(proc_, true);
        SDL_WaitProcess(proc_, true, nullptr); // reap (bounded — it was force-killed)
    }
    SDL_DestroyProcess(proc_);
    proc_ = nullptr;
    out_ = nullptr; // owned by proc_, already gone
}

Process::~Process() {
    destroy();
}

Process::Process(Process &&other) noexcept : proc_(other.proc_), out_(other.out_) {
    other.proc_ = nullptr;
    other.out_ = nullptr;
}

Process &Process::operator=(Process &&other) noexcept {
    if (this != &other) {
        destroy();
        proc_ = std::exchange(other.proc_, nullptr);
        out_ = std::exchange(other.out_, nullptr);
    }
    return *this;
}

Process Process::spawn(const char *const *args) {
    Process p;
    p.proc_ = createPipedProcess(args);
    if (p.proc_)
        p.out_ = SDL_GetProcessOutput(p.proc_); // non-blocking stdout stream, owned by the process
    return p;
}

size_t Process::readSome(std::string &buf, size_t max) {
    if (!out_)
        return 0;
    const size_t base = buf.size();
    buf.resize(base + max);
    const size_t n = SDL_ReadIO(out_, buf.data() + base, max); // 0 => not-ready or EOF
    buf.resize(base + n);
    return n;
}

bool Process::exited(int *exitCode) {
    if (!proc_)
        return true;
    return SDL_WaitProcess(proc_, false, exitCode);
}

void Process::kill() {
    if (proc_)
        SDL_KillProcess(proc_, true);
}

} // namespace ofs::util
