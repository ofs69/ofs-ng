#pragma once

#include "Core/Events.h"
#include "Core/ProjectLifecycleEvents.h"
#include "Platform/DotNetHost.h"
#include "Services/PluginApi.h"
#include "Services/ScriptNodeEvents.h"
#include "Services/ScriptRegistry.h"
#include "Services/ScriptWatch.h"
#include "Util/FaultThrottle.h"
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace ofs {

class EventQueue;
class JobSystem;
struct ScriptProject;

// Native↔managed result of one compile. Layout must match ScriptCompileResult in
// plugins/Ofs.ScriptHost/ScriptCompiler.cs.
struct OfsScriptCompileResult {
    OfsGenericFn fnPtr = nullptr; // trampoline
    void *userData = nullptr;     // slot index
    int signal = 0;               // OfsSignalKind
};

// Pins the marshaled layout to the C# mirror (ScriptCompileResult); reordering/resizing a field fails
// the build here, forcing the matching edit on the managed side. (offsetof via <cstddef>, pulled in by
// PluginApi.h above.)
static_assert(sizeof(OfsScriptCompileResult) == 24, "OfsScriptCompileResult layout drift");
static_assert(offsetof(OfsScriptCompileResult, fnPtr) == 0 && offsetof(OfsScriptCompileResult, userData) == 8 &&
                  offsetof(OfsScriptCompileResult, signal) == 16,
              "OfsScriptCompileResult field drift");

// CompileScriptNative: returns 0 on success (fills *out), 1 on a compile error (fills errBuf),
// negative on an unexpected failure. InitScriptHostNative hands the script trampolines a HostApi
// whose discrete I/O accessors are valid even when no plugin has registered a discrete node.
//
// `inputNames`/`outputNames` are the header's named pins, one per line ("<name>\n…"), in declaration
// order; the line count is the pin count (empty/null inputNames ⇒ a generator). The managed wrapper
// injects each as a named local (an input read / an output write target) and shapes the Eval method
// to that (inputCount, outputCount). `paramSpec` carries the user-declared params (parsed once on the
// C++ side via parseScriptHeader) so the wrapper can inject a named local per param without re-parsing
// the header. Format: one line per param, "<name>\t<f|i>\t<default>\n", in declaration order (index =
// ctx.Param slot). Empty/null = no params (wrapper injects nothing).
typedef int(CORECLR_DELEGATE_CALLTYPE *compile_script_native_fn)(const char *source, int signal, const char *inputNames,
                                                                 const char *outputNames, const char *paramSpec,
                                                                 OfsScriptCompileResult *out, char *errBuf,
                                                                 int errBufSize);
typedef void(CORECLR_DELEGATE_CALLTYPE *init_script_host_native_fn)(HostApi *hostApi);
// Releases a compiled script (nulls its slot, unloads its collectible assembly) once the host stops
// referencing it. (signal, userData) are the NodeCallRef fields the matching compile returned.
typedef void(CORECLR_DELEGATE_CALLTYPE *release_script_native_fn)(int signal, void *userData);

// Compiles script-node .cs files (under <prefPath>/scripts) via the Roslyn-backed Ofs.ScriptHost
// and fills the main-thread ScriptRegistryState that ProcessingSystem reads at snapshot-build time.
// Compilation is content-hash cached and runs on a JobSystem worker; the result returns to the main
// thread as a ScriptCompiledEvent. Owns its own DotNetHost (the CLR is process-global, so this just
// resolves additional entry points from the already-initialized runtime).
class ScriptSystem {
  public:
    ScriptSystem(ScriptProject &project, ScriptRegistryState &scriptReg, EventQueue &eq, JobSystem &jobSystem);

    // Loads Ofs.ScriptHost and wires the discrete I/O accessors. Returns false if .NET is
    // unavailable; scripts then never compile and Script nodes stay pass-through.
    bool init();

    // Polls the on-disk mtime of every script file referenced by a watch-enabled Script node and
    // recompiles the ones that changed. Throttled internally; safe to call once per frame.
    void update(float dt);

    // Push a throttled user-facing error toast for a script-node fault. Called from the hostReportFault
    // ABI callback wired to this system's HostApi, which runs on a worker thread (node eval), so this
    // is thread-safe: it coalesces under a mutex and pushes a NotifyEvent (push is itself thread-safe).
    // `ctx` is the entry point that faulted (e.g. "node:gen"). Used when no plugin is loaded to install
    // the PluginGuard sinks — otherwise script faults would be caught but never surfaced.
    void notifyScriptFault(const std::string &ctx);

  private:
    void onCompileScript(const CompileScriptEvent &e);
    void onCompileEmbeddedScript(const CompileEmbeddedScriptEvent &e);
    void onScriptCompiled(const ScriptCompiledEvent &e);
    void onProjectLoaded(const LoadProjectEvent &e);
    // Compile every Script node referenced by the currently loaded project (deduped by file / source).
    // Runs both on LoadProjectEvent and at the end of init(): the host comes up after the first frame
    // (onStartupComplete), so a project reopened on startup loads while ready == false and its compile
    // requests are dropped — init() re-scans here once the host is up to catch that already-loaded graph.
    void compileProjectScripts();

    void compileFile(const std::string &fileName);
    void compileEmbedded(const std::string &source);
    // Header parse + hash dedupe + Roslyn submit for a unit of source. `fileName` is empty for an
    // embedded script (it has no disk file; it resolves purely by content hash).
    void compileSource(const std::string &source, const std::string &fileName);
    // Refresh the persisted header caches and reconcile params on every Script node that currently
    // resolves to `hash` (file nodes via fileToHash[scriptFile], embedded nodes via their source
    // hash), then re-evaluate the affected axes so the new (or now-removed) script ref takes effect.
    void refreshByHash(const std::string &hash, const CompiledScript &cs);

    static std::filesystem::path scriptsDir();
    // On-disk path of a user script under <prefPath>/scripts, iff it exists. The shipped library lives
    // in data.pak (ofs::res), not on disk, so this returns nullopt for a library-only name — a user
    // copy of the same name (a fork) shadows the library and is what makes a node editable/watchable.
    static std::optional<std::filesystem::path> userScriptPath(const std::string &fileName);

    ScriptProject &project;
    ScriptRegistryState &scriptReg;
    EventQueue &eq;
    JobSystem &jobSystem;

    DotNetHost dotNetHost;
    compile_script_native_fn compileScriptNative = nullptr;
    release_script_native_fn releaseScriptNative = nullptr;
    HostApi hostApi{}; // minimal: discrete I/O accessors + log/notify, kept alive for the trampolines
    bool ready = false;

    // Throttles script-fault toasts (keyed by faulting entry point) so a script throwing every sample on a
    // worker thread can't flood the bell.
    ofs::util::FaultThrottle faultThrottle;

    // Param defs parsed on the main thread in compileFile, keyed by content hash, awaiting the
    // worker's ScriptCompiledEvent so onScriptCompiled can attach them to the CompiledScript.
    // Main-thread-only; never read off the worker (the worker only sees the packed paramSpec).
    std::unordered_map<std::string, std::vector<ScriptParamDef>> pendingParams;

    // File-watch state: last-seen mtime per watched file. An entry only exists while at least one
    // watch-enabled node references the file; missing files are tracked with a zero mtime so the
    // first time they reappear counts as a change. Polled on a fixed cadence, not every frame.
    float watchPollAccum = 0.0f;
    WatchMtimeMap watchMtimes;
};

} // namespace ofs
