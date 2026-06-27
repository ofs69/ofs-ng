#include "ScriptSystem.h"
#include "Core/EventQueue.h"
#include "Core/ScriptProject.h"
#include "Services/JobSystem.h"
#include "Services/ManagedAssemblyTrust.h"
#include "Services/PluginNodeIO.h"
#include "Services/ScriptHeader.h"
#include "Services/ScriptWatch.h"
#include "Util/FileUtil.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"
#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_set>

namespace ofs {

namespace {

// hostLog/hostReportFault for the script host's HostApi, so a throwing script node is logged + toasted
// even when no plugin is loaded (the plugin path installs the managed PluginGuard sinks; a script-only
// project has none). ctx is the ScriptSystem (set as hostApi.ctx in init).
void scriptHostLog(void * /*ctx*/, int level, const char *msg) {
    if (!msg)
        return;
    switch (level) {
    case 0:
    case 1:
        OFS_CORE_TRACE("[script] {}", msg);
        break;
    case 3:
        OFS_CORE_WARN("[script] {}", msg);
        break;
    case 4:
        OFS_CORE_ERROR("[script] {}", msg);
        break;
    default:
        OFS_CORE_INFO("[script] {}", msg);
        break;
    }
}

void scriptHostReportFault(void *ctx, const char *faultCtx) {
    if (!ctx || !faultCtx)
        return;
    static_cast<ScriptSystem *>(ctx)->notifyScriptFault(faultCtx);
}

} // namespace

ScriptSystem::ScriptSystem(ScriptProject &project, ScriptRegistryState &scriptReg, EventQueue &eq, JobSystem &jobSystem)
    : project(project), scriptReg(scriptReg), eq(eq), jobSystem(jobSystem) {
    eq.on<CompileScriptEvent>([this](const CompileScriptEvent &e) { onCompileScript(e); });
    eq.on<CompileEmbeddedScriptEvent>([this](const CompileEmbeddedScriptEvent &e) { onCompileEmbeddedScript(e); });
    eq.on<ScriptCompiledEvent>([this](const ScriptCompiledEvent &e) { onScriptCompiled(e); });
    eq.on<LoadProjectEvent>([this](const LoadProjectEvent &e) { onProjectLoaded(e); });
}

bool ScriptSystem::init() {
    if (!dotNetHost.init()) {
        OFS_CORE_WARN("ScriptSystem: .NET runtime unavailable; script nodes disabled.");
        return false;
    }
    std::filesystem::path hostPath = "managed/Ofs.ScriptHost.dll";
    // The script host compiles and runs arbitrary C# with full privileges. A hash mismatch vs this
    // build means tampering or a corrupt install — refuse to load rather than run it.
    if (!managedAssemblyTrusted(hostPath, "Ofs.ScriptHost")) {
        OFS_CORE_ERROR("ScriptSystem: Ofs.ScriptHost integrity check failed; script nodes disabled.");
        return false;
    }
    if (!dotNetHost.loadAssembly(hostPath)) {
        OFS_CORE_WARN("ScriptSystem: failed to load Ofs.ScriptHost.dll; script nodes disabled.");
        return false;
    }

    compileScriptNative = dotNetHost.getFunctionPointer<compile_script_native_fn>(
        hostPath, STR("Ofs.ScriptHost.ScriptCompiler, Ofs.ScriptHost"), STR("CompileScriptNative"));
    auto initFn = dotNetHost.getFunctionPointer<init_script_host_native_fn>(
        hostPath, STR("Ofs.ScriptHost.ScriptCompiler, Ofs.ScriptHost"), STR("InitScriptHostNative"));
    releaseScriptNative = dotNetHost.getFunctionPointer<release_script_native_fn>(
        hostPath, STR("Ofs.ScriptHost.ScriptCompiler, Ofs.ScriptHost"), STR("ReleaseScriptNative"));
    if (!compileScriptNative || !initFn) {
        OFS_CORE_ERROR("ScriptSystem: failed to resolve Ofs.ScriptHost entry points.");
        return false;
    }

    // Minimal HostApi so the discrete script trampolines can read inputs / write output, plus
    // log/report-fault so script-node faults are surfaced without a plugin loaded. ctx is this system
    // (only hostLog/hostReportFault use it; the node I/O accessors take their handle directly and ignore
    // ctx). hostNotify is left null — script authors have no plugin-initiated toast surface.
    hostApi.version = OFS_ABI_VERSION;
    hostApi.ctx = this;
    hostApi.nodeInputCount = &nodeInputCount;
    hostApi.nodeInputTime = &nodeInputTime;
    hostApi.nodeInputPosition = &nodeInputPosition;
    hostApi.nodeAddAction = &nodeAddAction;
    hostApi.hostLog = &scriptHostLog;
    hostApi.hostReportFault = &scriptHostReportFault;
    initFn(&hostApi);

    ready = true;
    OFS_CORE_INFO("ScriptSystem initialized (Roslyn script host).");

    // Compile any Script nodes from a project that was reopened on startup: that load runs before this
    // deferred init (onStartupComplete), so its LoadProjectEvent and compile requests were dropped while
    // ready == false. Now that the host is up, scan the loaded graph so those nodes resolve without the
    // user re-adding them.
    compileProjectScripts();
    return true;
}

void ScriptSystem::notifyScriptFault(const std::string &ctx) {
    const auto suppressed = faultThrottle.onFault(ctx);
    if (!suppressed) // inside the coalescing window — counted, not emitted
        return;
    std::string message = *suppressed > 0
                              ? fmt::format("Script node error in {} (+{} more) — see log", ctx, *suppressed)
                              : fmt::format("Script node error in {} — see log", ctx);
    eq.push(NotifyEvent{.level = NotifyLevel::Error, .message = std::move(message)});
}

std::filesystem::path ScriptSystem::scriptsDir() {
    auto dir = ofs::util::getPrefPath() / "scripts";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::optional<std::filesystem::path> ScriptSystem::userScriptPath(const std::string &fileName) {
    if (fileName.empty())
        return std::nullopt;
    auto p = scriptsDir() / ofs::util::fromUtf8(fileName);
    std::error_code ec;
    if (std::filesystem::exists(p, ec))
        return p;
    return std::nullopt;
}

void ScriptSystem::update(float dt) {
    if (!ready)
        return;

    // Poll a few times a second — file edits are human-paced, and stat-ing every frame is wasteful.
    constexpr float kPollInterval = 0.4f;
    watchPollAccum += dt;
    if (watchPollAccum < kPollInterval)
        return;
    watchPollAccum = 0.0f;

    // Stat every file referenced by a watch-enabled Script node this tick (a missing file maps to a
    // zero mtime, so its reappearance later counts as a change). The pure diff decides what changed.
    WatchMtimeMap current;
    for (const auto &region : project.regions)
        for (const auto &node : region.nodeGraph.nodes)
            if (node.type == GraphNodeType::Script && node.scriptWatch && !node.scriptFile.empty() &&
                node.scriptEmbeddedSource.empty()) {
                // Only user files under <pref>/scripts are watchable; shipped library scripts live in
                // data.pak (no path, immutable) and embedded sources have no file at all.
                if (auto path = userScriptPath(node.scriptFile)) {
                    std::error_code ec;
                    auto mtime = std::filesystem::last_write_time(*path, ec);
                    current[node.scriptFile] = ec ? std::filesystem::file_time_type{} : mtime;
                }
            }

    for (const auto &fileName : diffWatchedMtimes(current, watchMtimes))
        compileFile(fileName);
}

void ScriptSystem::onProjectLoaded(const LoadProjectEvent &) {
    compileProjectScripts();
}

void ScriptSystem::compileProjectScripts() {
    if (!ready)
        return;
    std::unordered_set<std::string> files;
    std::unordered_set<std::string> embedded;
    for (const auto &region : project.regions)
        for (const auto &node : region.nodeGraph.nodes) {
            if (node.type != GraphNodeType::Script)
                continue;
            if (!node.scriptEmbeddedSource.empty())
                embedded.insert(node.scriptEmbeddedSource);
            else if (!node.scriptFile.empty())
                files.insert(node.scriptFile);
        }
    for (const auto &f : files)
        compileFile(f);
    for (const auto &s : embedded)
        compileEmbedded(s);
}

void ScriptSystem::onCompileScript(const CompileScriptEvent &e) {
    compileFile(e.fileName);
}

void ScriptSystem::onCompileEmbeddedScript(const CompileEmbeddedScriptEvent &e) {
    compileEmbedded(e.source);
}

void ScriptSystem::compileFile(const std::string &fileName) {
    if (!ready || fileName.empty())
        return;

    // Source resolution: a file node references a user file under <pref>/scripts — nothing else. The
    // shipped library is reached only through embedded nodes (which bake in their source); a file node
    // never falls back to a like-named packed library, so deleting the user file (or opening the graph
    // on a machine that lacks it) makes the node go stale ("script not found") rather than silently
    // resolving to a different script. Embedded nodes never come through here (they carry their source).
    std::optional<std::string> source;
    if (auto path = userScriptPath(fileName))
        source = ofs::util::readFile(*path);

    if (!source) {
        // Missing file: record an errored artifact (not an erase) so find() returns it and the node
        // shows "script not found" rather than an indefinite "compiling…" — find() == nullptr is
        // reserved for the genuinely uncompiled / in-flight state. The node still evaluates as
        // pass-through (ref.valid() == false), matching the prior behavior. The sentinel hash is
        // keyed per file so it never collides with a 16-hex content hash.
        CompiledScript cs;
        cs.error = fmt::format("script not found: {}", fileName);
        const std::string hash = fmt::format("missing:{}", fileName);
        scriptReg.byHash[hash] = cs;
        scriptReg.fileToHash[fileName] = hash;
        refreshByHash(hash, cs);
        return;
    }

    compileSource(*source, fileName);
}

void ScriptSystem::compileEmbedded(const std::string &source) {
    if (!ready || source.empty())
        return;
    compileSource(source, ""); // no disk file; resolves purely by content hash
}

void ScriptSystem::compileSource(const std::string &source, const std::string &fileName) {
    const ScriptHeader header = parseScriptHeader(source);

    // Author feedback: a recognized "// !ofs:" directive dropped for a bad value would otherwise vanish
    // silently. Surface each in the log keyed by the script so the author can see why a knob/pin is gone.
    for (const std::string &w : header.warnings)
        OFS_CORE_WARN("script header [{}]: {}", fileName.empty() ? "embedded" : fileName, w);

    const int signal = static_cast<int>(header.signal);
    const int inputCount = header.inputCount();
    const int outputCount = header.outputCount();
    const std::vector<std::string> inputNames = header.inputNames;
    const std::vector<std::string> outputNames = header.outputNames;
    const std::string hash = scriptContentHash(source);

    // Newline-packed pin names for the managed wrapper's named-local injection (see
    // compile_script_native_fn). The line count is the pin count.
    auto packNames = [](const std::vector<std::string> &names) {
        std::string packed;
        for (const auto &n : names)
            packed += fmt::format("{}\n", n);
        return packed;
    };
    const std::string inputNamesSpec = packNames(inputNames);
    const std::string outputNamesSpec = packNames(outputNames);

    // Content-hash dedupe: identical bytes share one compiled artifact; never recompile. The cached
    // entry already carries its param defs (identical bytes ⇒ identical params), so no re-parse.
    auto existing = scriptReg.byHash.find(hash);
    if (existing != scriptReg.byHash.end()) {
        if (!fileName.empty())
            scriptReg.fileToHash[fileName] = hash;
        refreshByHash(hash, existing->second);
        return;
    }

    // Stash the parsed defs so onScriptCompiled can attach them to the artifact (the worker result
    // does not carry them — they're main-thread data). Keyed by hash and grow-only, mirroring
    // byHash: identical content always yields identical defs, so a duplicate in-flight compile of
    // the same hash reads the same entry. Never read off the worker thread.
    pendingParams[hash] = header.params;

    // Packed param spec for the managed wrapper's named-local injection (see compile_script_native_fn).
    std::string paramSpec;
    for (const auto &p : header.params) {
        // Kind char drives the injected local's type (ScriptCompiler.cs): f=float, i=int, b=bool,
        // e=enum (an int index). fmt is locale-independent ⇒ '.' decimal regardless of host locale.
        char kind = 'f';
        switch (p.type) {
        case OfsParamInt:
            kind = 'i';
            break;
        case OfsParamBool:
            kind = 'b';
            break;
        case OfsParamEnum:
            kind = 'e';
            break;
        default:
            kind = 'f';
            break;
        }
        paramSpec += fmt::format("{}\t{}\t{}\n", p.name, kind, p.defaultValue);
    }

    // Heavy Roslyn work goes to a worker; the slot append happens inside (serialized in Ofs.Api).
    jobSystem.submitTask([fn = compileScriptNative, eqp = &eq, source, fileName, hash, signal, inputCount, outputCount,
                          inputNames, outputNames, inputNamesSpec, outputNamesSpec, paramSpec]() -> bool {
        OfsScriptCompileResult res{};
        char err[4096] = {};
        int rc = fn(source.c_str(), signal, inputNamesSpec.c_str(), outputNamesSpec.c_str(), paramSpec.c_str(), &res,
                    err, static_cast<int>(sizeof(err)));
        ScriptCompiledEvent ev;
        ev.fileName = fileName;
        ev.hash = hash;
        ev.inputCount = inputCount;
        ev.outputCount = outputCount;
        ev.inputNames = inputNames;
        ev.outputNames = outputNames;
        if (rc == 0) {
            ev.ok = true;
            ev.ref = NodeCallRef{.fnPtr = res.fnPtr,
                                 .userData = res.userData,
                                 .signal = static_cast<OfsSignalKind>(res.signal),
                                 .inputCount = inputCount,
                                 .outputCount = outputCount};
        } else {
            ev.ok = false;
            ev.error = err;
        }
        eqp->push(std::move(ev));
        return rc == 0;
    });
}

void ScriptSystem::onScriptCompiled(const ScriptCompiledEvent &e) {
    CompiledScript cs;
    cs.ref = e.ok ? e.ref : NodeCallRef{};
    cs.inputCount = e.inputCount;
    cs.outputCount = e.outputCount;
    cs.inputNames = e.inputNames;
    cs.outputNames = e.outputNames;
    cs.error = e.error;
    // Attach the param defs parsed on the main thread at submit time. Not erased: a duplicate
    // in-flight compile of the same hash must still find them (the entry is grow-only like byHash).
    if (auto pit = pendingParams.find(e.hash); pit != pendingParams.end())
        cs.params = pit->second;
    scriptReg.byHash[e.hash] = cs;
    if (!e.fileName.empty()) // embedded scripts have no file name; they resolve by hash
        scriptReg.fileToHash[e.fileName] = e.hash;
    if (!e.ok)
        OFS_CORE_WARN("ScriptSystem: compile failed for {}:\n{}", e.fileName.empty() ? "<embedded>" : e.fileName,
                      e.error);
    refreshByHash(e.hash, scriptReg.byHash[e.hash]);
}

void ScriptSystem::refreshByHash(const std::string &hash, const CompiledScript &cs) {
    AxisRoles affected;
    // Content hashes still reachable through an embedded node's baked-in source. File references are
    // covered separately by fileToHash's values (below). Together these are the artifacts the leak sweep
    // must keep; everything else with a valid compile is an orphan to release.
    std::unordered_set<std::string> embeddedHashes;
    for (auto &region : project.regions) {
        bool touched = false;
        for (auto &node : region.nodeGraph.nodes) {
            if (node.type != GraphNodeType::Script)
                continue;
            // A node resolves to `hash` if it is the embedded node whose source hashes to it, or a
            // file node whose file currently maps to it.
            std::string nodeHash;
            if (!node.scriptEmbeddedSource.empty()) {
                nodeHash = scriptContentHash(node.scriptEmbeddedSource);
                embeddedHashes.insert(nodeHash);
            } else if (auto it = scriptReg.fileToHash.find(node.scriptFile); it != scriptReg.fileToHash.end()) {
                nodeHash = it->second;
            }
            if (nodeHash != hash)
                continue;
            // Refresh the persisted header caches so the node draws correct pins even when the file
            // later goes missing. A successful compile is authoritative; an errored-but-parseable file
            // still carries header-declared arity. A genuine missing-file placeholder (invalid ref +
            // no defs) carries only defaults (inputCount == 1) — keep the node's persisted header so a
            // combiner stays a combiner and its B-input link is not left dangling against a hidden pin.
            if (cs.ref.valid() || !cs.params.empty()) {
                node.scriptInputCount = static_cast<uint8_t>(cs.inputCount);
                node.scriptOutputCount = static_cast<uint8_t>(cs.outputCount);
            }
            if (cs.ref.valid()) {
                node.scriptSignal = static_cast<uint8_t>(cs.ref.signal);
                // A successful compile is authoritative on pin arity: drop any link feeding an input pin
                // (toPin) or drawing from an output pin (fromPin) the script no longer has (e.g. a 1→0 or
                // 2→1 input change, or a dropped output), which would otherwise dangle against a pin that
                // is never rendered. Gated on a valid compile so a transiently missing or errored file
                // (which falls back to the default arity) never deletes the user's links.
                auto &links = region.nodeGraph.links;
                std::erase_if(links, [&node](const auto &l) {
                    return (l.toNode == node.id && l.toPin >= node.scriptInputCount) ||
                           (l.fromNode == node.id && l.fromPin >= node.scriptOutputCount);
                });
            }
            // Size/seed/clamp the node's param values to the artifact's current defs (by index).
            // Runs on compile and on project load, so a loaded node's values are seeded here.
            // Skip a missing-file placeholder (invalid ref + no defs): that would wipe values we
            // can't re-derive, destroying serialized params when a project opens without its .cs.
            // An errored-but-parseable file still has its header-declared defs, so it reconciles.
            if (cs.ref.valid() || !cs.params.empty())
                reconcileScriptParams(node.effect.params, cs.params);
            touched = true;
        }
        if (touched)
            affected |= region.axisRoles;
    }

    // Reclaim compiled scripts nothing references anymore. Each successful Roslyn compile roots a
    // collectible ALC + assembly + slot delegate for the process lifetime unless explicitly released, so
    // the file-watch / edit loop (a fresh content hash per revision) would otherwise leak one per edit. An
    // artifact is live while a file still maps to it (fileToHash value — this also preserves the compile-
    // then-place contract, since fileToHash[file] survives until that file's content changes) or an
    // embedded node still bakes in matching source; everything else with a valid compile is an orphan.
    // Errored / missing-file placeholders hold no managed resource (invalid ref), so they are left alone.
    // Note: a file the user re-points a node away from keeps its stale fileToHash entry, so that (bounded,
    // per-distinct-file) artifact is intentionally not reclaimed here — only superseded content is.
    if (releaseScriptNative) {
        std::unordered_set<std::string> fileHashes;
        for (const auto &[file, h] : scriptReg.fileToHash)
            fileHashes.insert(h);
        for (auto it = scriptReg.byHash.begin(); it != scriptReg.byHash.end();) {
            const NodeCallRef &r = it->second.ref;
            if (r.valid() && !fileHashes.contains(it->first) && !embeddedHashes.contains(it->first)) {
                releaseScriptNative(static_cast<int>(r.signal), r.userData);
                // The parsed defs are dead weight once the artifact is gone: pendingParams is grow-only
                // (one entry per distinct revision), so without this the edit/watch loop leaks one entry
                // per save even though the heavy ALC + byHash artifact are reclaimed here. An orphan is by
                // definition unreferenced, so no in-flight compile of this hash still needs the defs.
                pendingParams.erase(it->first);
                it = scriptReg.byHash.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (size_t i = 0; i < kStandardAxisCount; ++i)
        if (affected.test(i))
            eq.push(AxisModifiedEvent{static_cast<StandardAxis>(i)});
}

} // namespace ofs
