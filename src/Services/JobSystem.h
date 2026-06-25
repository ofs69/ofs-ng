#pragma once

#include "Core/ProcessingRegion.h"
#include "Core/ScriptAxisAction.h"
#include "Core/StandardAxis.h"
#include "Core/VectorSet.h"
#include "Services/ScriptRegistry.h"
#include <array>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ofs {

struct EvalJob {
    std::atomic<int> currentNodeId{-1};
    // Cooperative cancel flag. An int (not bool) so its address can be handed to plugin/script nodes as a
    // plain `const volatile int*` they poll directly — the managed side can't speak std::atomic. Monotonic:
    // set once, never cleared. The C++ side only ever touches it through cancel()/isCancelled().
    std::atomic<int> cancelled{0};
    // Names this eval's TState captures (set at snapshot build). -1 = nothing captured. Used on the
    // main thread to release the captures when the job completes or is superseded/cancelled.
    int captureGeneration = -1;

    void cancel() noexcept { cancelled.store(1, std::memory_order_release); }
    bool isCancelled() const noexcept { return cancelled.load(std::memory_order_acquire) != 0; }
};

struct AxisSnapshot {
    StandardAxis role;
    struct AxisData {
        VectorSet<ScriptAxisAction> actions;
    };
    std::array<AxisData, kStandardAxisCount> axes;
    std::vector<ProcessingRegion> regions;
    // Dynamic-node call info resolved on the main thread at snapshot-build time, for BOTH script and
    // plugin nodes: regionId -> (nodeId -> NodeCallRef). The worker reads this structure it solely
    // owns (the snapshot is a value copy), so neither a concurrent recompile (scripts) nor a plugin
    // unload (plugin nodes) can race it. The worker never reads effectReg.pluginNodes.
    std::unordered_map<int, NodeRefMap> nodeRefs;
};

class JobSystem {
  public:
    explicit JobSystem(std::size_t threadCount = 0); // 0 = hardware_concurrency; does NOT start the pool
    void start();                                    // start the thread pool; call after EventQueue::freeze()

    // Drop queued jobs, block until in-flight workers return, and join the pool. After this, submit()/
    // submitTask() are no-ops. MUST be called before any service a worker reaches into (ScriptSystem's
    // hostApi, a plugin's HostApi) is destroyed, so no node trampoline derefs a freed struct. Idempotent;
    // the destructor calls it as a backstop.
    void shutdown();

    ~JobSystem();

    // Submit axis evaluation work. fn must check job->isCancelled() at region boundaries.
    // fn MUST NOT touch ScriptProject — push results via EventQueue::push().
    void submit(const std::shared_ptr<EvalJob> &job, AxisSnapshot snap, std::function<void(AxisSnapshot)> fn);

    // Submit a general fire-and-forget task; returns a future for the result.
    std::future<bool> submitTask(std::function<bool()> fn);

    // Live view of the eval workers (the submit() path only — general submitTask work such as a multi-minute
    // video transcode is excluded so it can't masquerade as a stuck eval). `running` is how many eval jobs
    // are executing on a pool thread right now; `oldestAgeSeconds` is the wall-clock age of the longest-
    // running one (0 when none). A wedged worker never reaches its completion bookkeeping, so it keeps
    // counting and its age grows without bound — that is what surfaces a non-exiting worker in the footer.
    struct WorkerStats {
        std::size_t running = 0;
        double oldestAgeSeconds = 0.0;
    };
    WorkerStats evalWorkerStats() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ofs
