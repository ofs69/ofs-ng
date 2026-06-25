#pragma once

#include <string>
#include <string_view>

// Forward declarations so consumers don't pull in SDL just to hold a process handle. In C++ a struct
// tag declaration also introduces the type name, so these double as the SDL typedef names.
struct SDL_Process;
struct SDL_IOStream;

namespace ofs::util {

// Resolve a tool name ("ffmpeg", "ffprobe") to the command used to launch it. On Windows the tool
// ships next to the exe, so this returns the absolute "<base>/<tool>.exe"; elsewhere it returns the
// bare name for a PATH lookup. UTF-8.
std::string resolveTool(std::string_view tool);

// Cheap, cached availability probe for the command gate (called every frame from a command's
// isEnabled, so it must not spawn per call). Windows: checks the bundled exe exists. Linux/macOS:
// spawns "<tool> -version" once and caches the result for the session. Pass forceRefresh=true to
// re-probe (e.g. when Preferences opens). Main thread only (the cache is unsynchronized).
bool toolAvailable(std::string_view tool, bool forceRefresh = false);

// Run a process to completion, capturing its stdout. BLOCKING — call from a JobSystem worker, never
// the main thread. `args` is a NULL-terminated argv (args[0] is the executable, as from resolveTool).
// Returns false if the process could not be started; on success `out` holds stdout and `exitCode` the
// process exit code (the caller decides whether a non-zero code is a failure). UTF-8 throughout.
bool runCaptured(const char *const *args, std::string &out, int &exitCode);

// A running child process with its stdout piped to the app — used to stream ffmpeg's `-progress`
// output. Move-only, RAII: the destructor force-kills a still-running process and destroys the SDL
// handle. A single Process must be touched from one thread at a time.
class Process {
  public:
    Process() = default;
    ~Process();
    Process(Process &&other) noexcept;
    Process &operator=(Process &&other) noexcept;
    Process(const Process &) = delete;
    Process &operator=(const Process &) = delete;

    // Spawn `args` (NULL-terminated argv) with stdin/stdout piped to the app. The returned Process is
    // invalid (valid() == false) if the process could not be started.
    static Process spawn(const char *const *args);

    bool valid() const { return proc_ != nullptr; }

    // Append up to `max` bytes of available stdout to `buf`; returns the number of bytes read. A return
    // of 0 means "nothing ready yet" while running() and "end of output" once exited() — the caller
    // uses exited() to tell them apart. Non-blocking.
    size_t readSome(std::string &buf, size_t max = 4096);

    // Non-blocking exit check. Returns true once the process has exited; fills `exitCode` if non-null.
    bool exited(int *exitCode = nullptr);

    // Force-terminate the process. Safe to call on an already-exited process.
    void kill();

  private:
    void destroy(); // kill-if-running + destroy the SDL handle

    SDL_Process *proc_ = nullptr;
    SDL_IOStream *out_ = nullptr; // owned by proc_; never closed separately
};

} // namespace ofs::util
