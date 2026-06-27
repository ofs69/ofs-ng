#pragma once

#include "Core/Events.h"
#include "Core/ProjectLifecycleEvents.h"
#include "Platform/DotNetHost.h"
#include "Services/BindingSystem.h"
#include "Services/CommandRegistry.h"
#include "Services/EffectRegistry.h"
#include "Services/PluginApi.h"
#include "Services/PluginEvents.h"
#include "Util/Coro.h"
#include "Util/FaultThrottle.h"
#include <bitset>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ofs {

class EventQueue;
class PluginManager;
class VideoPlayer;
struct ScriptProject;

// Context block stored in PluginManager and passed as void* ctx to every HostApi callback.
// Eliminates the need for file-scope statics in PluginManager.cpp.
// project is ScriptProject* because the C ABI requires a raw pointer via void*.
struct PluginCtx {
    ScriptProject *project = nullptr;
    EventQueue *eventQueue = nullptr;
    VideoPlayer *player = nullptr;
    PluginManager *manager = nullptr;
    EffectRegistryState *effectReg = nullptr;
    CommandRegistry *commandRegistry = nullptr;
    BindingSystem *bindingSystem = nullptr;
    const char *currentPluginName = nullptr;
    // Per-render-pass widget id counter, reset to 0 at the top of every plugin render pass. Its sole job
    // is uniqueness — each beginField/pushRow/pushSection takes the next value as its PushID seed, so no
    // two widgets in a pass can collide (no shared ImGui state, no asserts). Identity is therefore
    // positional, not label-based: transient state (focus/edit/open) won't follow a widget across a
    // set/order change, which is inherent to immediate mode and unfixable host-side.
    int uiIdCounter = 0;
    int uiSectionDepth = 0;  // nesting level of uiPushSection; only the outermost owns the form table
    int uiDisabledDepth = 0; // open uiPushDisabled scopes; guards the pop so an unbalanced plugin can't
                             // unwind ImGui's disabled stack past what we pushed
    // Explanatory tooltip for the innermost open uiPushDisabledTooltip scope (empty = none). endField
    // surfaces it on hover of each greyed widget, so a disabled section needs no separate disclaimer line.
    // The stack saves/restores the enclosing scope's text so nested disabled-tooltip scopes stay correct.
    std::string disabledTooltip;
    std::vector<std::string> disabledTooltipStack;
    // uiPushRow/uiPopRow horizontal-row state. inRow widgets flow on one line; width-bearing ones split
    // rowAvailW evenly by rowItemCount. A row's widget count isn't known until its body has run, so the
    // divisor comes from the count stored in per-window ImGui state under the row's id last frame; frame
    // 1 of a new row falls back to count 1 and self-corrects on frame 2.
    bool inRow = false;
    int rowItemIndex = 0;
    int rowItemCount = 1;
    float rowAvailW = 0.f;
    std::thread::id mainThreadId;
    bool inOnLoad = false;
    // Plugin-node custom UI: set by the node-body render path around an onNodeUi call (phase 4e), so
    // the nodeUiGetState/nodeUiSetState host callbacks read/write the current node's TState JSON.
    // currentNodeState is null outside such a call; the renderer clears the pair afterwards.
    std::string *currentNodeState = nullptr; // the current node's nodeState buffer
    bool currentNodeStateChanged = false;    // set by nodeUiSetState when the wrapper reports a change
    // Stable identity of the current onNodeUi node, surfaced to managed via nodeUiCurrentKey so a
    // deferred Node<TState>.Update can target it. -1 outside the call.
    int currentNodeRegionId = -1;
    int currentNodeId = -1;
};

struct LoadedPlugin {
    std::string name;
    std::string displayName;
    std::string version; // assembly informational version reported by the plugin; "" if none
    std::filesystem::path path;
    PluginApi api;
    bool enabled = true;
    bool windowOpen = true;
    // True for a shipped plugin loaded from the base root by the first-party allowlist. Such plugins
    // are auto-trusted (baked-hash match) and cannot be uninstalled through the user install flow.
    bool firstParty = false;
    // SHA-256 (hex) of the entry DLL the user last consented to load. A load is gated on this
    // matching the DLL's current bytes, so a changed or swapped binary re-prompts. Empty = never
    // acknowledged. Persisted in plugin_states.json; survives enable/disable so re-enabling an
    // unchanged plugin does not re-prompt.
    std::string acknowledgedHash;

    // Transient developer aid (never persisted): when true, PluginManager polls this plugin's DLL and
    // reloads it when its bytes change (a rebuild). hotReloadMtime is the last-seen write time used to
    // detect the change; the default (epoch) means "re-seed on the next poll" (no spurious reload).
    bool hotReload = false;
    std::filesystem::file_time_type hotReloadMtime{};
};

// Native↔managed ABI gate applied after the bridge fills a plugin's PluginApi: the struct
// layout this build understands is version 1..OFS_ABI_VERSION. Anything else (a 0 from a
// failed fill, or a newer struct than this host knows) is rejected.
inline bool isPluginAbiVersionSupported(int version) {
    return version >= 1 && version <= OFS_ABI_VERSION;
}

typedef int(CORECLR_DELEGATE_CALLTYPE *load_plugin_native_fn)(const char *path, PluginApi *api, HostApi *hostApi);
// finalShutdown != 0 ⇒ the app is closing: the managed side skips forced collection and diagnostics
// (the DLL lock is moot at process exit). 0 ⇒ runtime disable/uninstall: force-collect so the folder
// can be replaced/deleted now.
typedef int(CORECLR_DELEGATE_CALLTYPE *unload_plugin_native_fn)(const char *dllStem, int finalShutdown);
// Current managed (GC) heap size in bytes; -1 on error. Cheap to call per frame (reads the GC total,
// does not collect). Null when the CLR never initialized — managedHeapBytes() then returns 0.
typedef long long(CORECLR_DELEGATE_CALLTYPE *get_managed_heap_bytes_fn)();

class PluginManager {
  public:
    PluginManager(ScriptProject &project, EventQueue &eq, std::shared_ptr<VideoPlayer> player,
                  std::shared_ptr<VideoPlayer> dummyPlayer, CommandRegistry &cmdRegistry, BindingSystem &bindingSystem,
                  EffectRegistryState &effectReg);
    ~PluginManager();

    // Initializes the .NET CLR host and plugin loader. Returns false if the runtime is unavailable.
    // Must be called after construction and before loadPlugins().
    bool init();
    void loadPlugins();
    void update(float delta);
    void renderUI();

    std::vector<LoadedPlugin> &getPlugins() { return loadedPlugins; }
    void firePluginCommand(const std::string &pluginName, const std::string &commandId);

    // Draw an active interaction intent's options (its OfsIntentUiFn) into the current ImGui scope,
    // wrapped in the same per-call plugin context renderUI() sets up around onBuildUI (currentPluginName
    // from `owningPlugin`, a fresh uiIdCounter, reset section/row state) so the ui* builder attributes
    // and ids correctly. The caller (the docked Tool Options panel) owns the surrounding window and
    // namespaces each section's ids; this only runs the callback. No-op if `onUi` is null. Main thread.
    void renderIntentUi(const std::string &owningPlugin, OfsIntentUiFn onUi, void *userData);
    void savePluginStates() const;

    // Managed (GC) heap bytes for the footer readout. 0 when the CLR is unavailable (no .NET runtime,
    // or init failed) — the footer then hides the zone. Calls into managed each frame; see the fn typedef.
    long long managedHeapBytes() const;

    // Push a throttled user-facing error toast for a swallowed plugin fault. Called from the hostNotify
    // ABI callback, which may run on a worker thread (node eval), so this is thread-safe: it coalesces
    // per plugin under a mutex and pushes a NotifyEvent (push is itself thread-safe). `ctx` is the entry
    // point that faulted (e.g. "OnUpdate", "node:gen").
    void notifyPluginFault(const std::string &who, const std::string &ctx);

    // Push a plugin-initiated user-facing toast at the given level, shown verbatim (plugin name
    // prefixed). Called from the hostNotify ABI callback (any thread; push is thread-safe). Unlike
    // notifyPluginFault this is deliberate and not throttled.
    void notifyPlugin(const std::string &who, NotifyLevel level, const std::string &msg);

    // Per-plugin app-scoped settings (global, not per-project): backed by an in-memory cache, one JSON
    // file per plugin at <pref>/plugin_settings/<name>.json. Reached from the getAppData/setAppData
    // HostApi callbacks via PluginCtx::manager (hence public). appSettingValue returns the cached JSON
    // value for `key` (lazily loading the file on first touch), or nullptr if unset. setAppSetting updates
    // the cache and marks it dirty (a null value erases the key); flushDirtyAppSettings writes every dirty
    // plugin's cache back to disk — called debounced once per frame from update() and again on shutdown.
    // Main thread only.
    const nlohmann::json *appSettingValue(const std::string &pluginName, const std::string &key);
    void setAppSetting(const std::string &pluginName, const std::string &key, nlohmann::json value);
    void flushDirtyAppSettings();

    // Tear every enabled plugin down (managed OnUnload + ALC unload) while hostApi/callCtx_ are still
    // alive. Idempotent. Does NOT change persisted enabled state or push events — see the .cpp. Called
    // from OfsApp shutdown and the destructor; PluginManager MUST outlive every plugin because the
    // managed host holds a raw HostApi*/PluginCtx* into this object's members.
    void shutdown();

    // Test seam: the only population path for loadedPlugins is doLoad(), which calls into the .NET
    // loader. A friend struct (bodies in tests/helpers/PluginManagerTestAccess.cpp, compiled only into
    // the test targets) lets tests exercise the dispatch + HostApi logic with a fake native PluginApi,
    // no CLR required. The declaration emits no code; shipping builds are unaffected.
    friend struct PluginManagerTestAccess;

  private:
    struct PluginSavedState {
        bool enabled = true;
        bool windowOpen = true;
        std::string acknowledgedHash;
        // Last-seen plugin version, persisted only so a disabled (unloaded) plugin can still show its
        // version in the UI. Not part of the trust gate — that stays the DLL hash (the version can't be
        // read without executing the untrusted assembly).
        std::string version;
    };
    std::map<std::string, PluginSavedState> loadPluginStates() const;

    // Ensure this plugin's app-settings object is in appSettingsCache_ (loading its file on first touch,
    // or an empty object when there is none) and return it. Backs appSettingValue / setAppSetting.
    nlohmann::json &loadedAppSettings(const std::string &pluginName);

    void setPluginEnabled(const std::string &name, bool enable);

    // Build a LoadedPlugin from a "<dir>/<dir-name>.dll" plugin folder, run the trust gate, and load
    // it (or push a disabled stub on decline). `firstParty` records whether this came from the base
    // (shipped) root. Shared by both roots in loadPlugins().
    void loadFromDir(const std::filesystem::path &pluginDir, const std::map<std::string, PluginSavedState> &savedStates,
                     bool firstParty);

    // Install flow (RequestInstallPluginEvent): stage the zip → validate structure → reject reserved
    // names / confirm replace → trust prompt → commit to <pref>/plugins → hot-load. Never touches the
    // user plugins dir until every check passes; the staging dir is always cleaned up.
    co::Fire installFromZip(std::filesystem::path zipPath);

    // notifyOnFailure: push a user-facing toast naming the failure (incompatible build vs. generic load
    // fault) when the load fails. Callers that surface their own failure UI (the install flow) pass false.
    bool doLoad(LoadedPlugin &lp, bool notifyOnFailure = true);

    // SHA-256 (hex) of lp's entry DLL, or nullopt if it can't be read.
    std::optional<std::string> hashPluginDll(const LoadedPlugin &lp) const;

    // Silent gate (no UI): true if lp is trusted to load right now — first-party (baked hash) or
    // its current bytes match lp.acknowledgedHash. Used at startup, which never prompts. Always
    // true under OFS_PLUGIN_TEST_HOOKS (headless tests). Consent for an untrusted plugin is
    // obtained interactively by enablePluginWithConsent / installFromZip, which then record the hash.
    bool isPluginTrusted(const LoadedPlugin &lp) const;

    // Shared body text for the "Load plugin?" trust modal (install + enable paths).
    static std::string trustPromptMessage(const std::string &name, const std::filesystem::path &dir);

    // User-initiated enable: if the plugin isn't already trusted, show the "Load plugin?" modal and,
    // on consent, record the hash; then enable (which loads it). Declining leaves it disabled.
    co::Fire enablePluginWithConsent(std::string name);

    // Hot-reload (dev): unload the live instance, accept its rebuilt bytes (the dev's standing consent
    // implied by leaving hot-reload on for this plugin), and reload from the same path. Only ever called
    // for an enabled, user-installed (non-first-party) plugin from pollHotReload.
    void reloadPlugin(LoadedPlugin &lp, const std::string &newHash);
    // Tear a live plugin down (onUnload + native unload + drop host registrations), leaving lp.api
    // zeroed. Shared by reloadPlugin and the language-switch reload in onSetLanguage.
    void tearDownPlugin(LoadedPlugin &lp);
    // Throttled poll (from update): for each plugin with hotReload on, reload it when its DLL's bytes
    // change (and can be fully read).
    void pollHotReload();

    // Throttled poll (from update): pick up a plugin folder that appeared in <pref>/plugins since launch
    // (a freshly built or just-installed plugin) without a restart — loads it if already trusted, else
    // adds a disabled stub the user can enable. Mirrors the user-root scan in loadPlugins().
    void discoverNewPlugins();

    void onSetPluginEnabled(const SetPluginEnabledEvent &e);
    void onSetPluginHotReload(const SetPluginHotReloadEvent &e);
    co::Fire onRequestInstallPlugin(RequestInstallPluginEvent e); // may co_await the zip file picker
    co::Fire onRequestUninstallPlugin(RequestUninstallPluginEvent e);
    void onSavePluginStates(const SavePluginStatesEvent &) const;
    void onRegisterPluginNode(const RegisterPluginNodeEvent &e);
    void onUnregisterPluginNodes(const UnregisterPluginNodesEvent &e);
    void onPlayStateChanged(const PlayStateChangedEvent &e);
    void onSpeedChanged(const SpeedChangedEvent &e);
    void onMediaChanged(const MediaChangedEvent &e);
    void onSetLanguage(const SetLanguageEvent &e);
    void onReloadPluginsForLanguage(const ReloadPluginsForLanguageEvent &e);
    void onProjectLoaded(const LoadProjectEvent &e);
    void onAxisModified(const AxisModifiedEvent &e);
    void flushAxisModified(); // notify plugins once per dirtied axis, called from update()
    void onAxisSelected(const AxisSelectedEvent &e);

    DotNetHost dotNetHost;
    std::vector<LoadedPlugin> loadedPlugins;
    load_plugin_native_fn loadPluginNative = nullptr;
    unload_plugin_native_fn unloadPluginNative = nullptr;
    get_managed_heap_bytes_fn getManagedHeapBytesNative = nullptr;

    HostApi hostApi{};
    PluginCtx callCtx_{};

    ScriptProject &project_;
    EventQueue &eventQueue_;
    EffectRegistryState &effectReg_;
    std::shared_ptr<VideoPlayer> player_;
    std::shared_ptr<VideoPlayer> dummyPlayer_;
    double lastReportedTime_ = -1.0;

    // A given axis can raise several AxisModifiedEvents in one drain (a mutate() plus a region edit
    // covering the same role, undo/redo fan-out, …). Plugins only need "axis X changed this frame",
    // so coalesce per-role here and forward exactly one onAxisModified callback per dirtied axis from
    // update(), instead of one managed-boundary crossing per event.
    std::bitset<kStandardAxisCount> axisModifiedDirty_;

    // Coalesces fault toasts (keyed by plugin name) so a node throwing every sample on a worker thread
    // can't flood the notification center.
    ofs::util::FaultThrottle faultThrottle_;

    // Per-plugin app-scoped settings: name → the plugin's whole settings object (the JSON written to
    // <pref>/plugin_settings/<name>.json). Loaded lazily on first access and kept across plugin
    // reloads/project switches. appSettingsDirty_ holds the names whose cache changed since the last disk
    // flush; flushDirtyAppSettings() writes and clears them (per frame from update(), and on shutdown()).
    std::map<std::string, nlohmann::json> appSettingsCache_;
    std::set<std::string> appSettingsDirty_;

    // Set by shutdown() so it (and the destructor that also calls it) runs its teardown exactly once.
    bool shutdownDone_ = false;

    // Frame-time accumulator that throttles the runtime plugin-folder poll (discoverNewPlugins +
    // pollHotReload), so neither hits the filesystem every frame.
    float pluginPollAccum_ = 0.f;

    // Names whose <pref>/plugins folder is awaiting deletion (a runtime uninstall whose DLL was still
    // locked; removed on next launch). Seeded in loadPlugins, updated on uninstall — discoverNewPlugins
    // consults it so it never re-surfaces a plugin the user just uninstalled.
    std::set<std::string> pendingUninstall_;
};

} // namespace ofs
