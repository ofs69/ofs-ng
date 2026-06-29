#pragma once

#include "Util/Log.h"

#include <string>

namespace ofs::mode_lifecycle {

// Shared activation/unload/load lifecycle for the three interaction-mode routers (edit mode / navigator
// / selection mode). The four handlers are structurally identical — they differ only in the registry,
// the two project id fields (effective `activeId` + stored/authored `storedId`), the native fallback id,
// and a log label — so the logic lives here once and each router threads its own fields through. The
// router stays the sole writer of its id fields (the helper mutates them only via the references it
// passes), preserving the per-router selection-id write ownership. `Entry` must expose `onEnter`/`onExit`
// (OfsIntentLifecycleFn, nullable), `userData`, `owningPlugin`, and `id`.

// Activate `newId` from a footer user-pick. No-op when it is already active or unknown (a stale UI option
// whose plugin unloaded mid-frame), so the active id can never dangle. Runs the outgoing entry's onExit
// then the incoming entry's onEnter (exit-old before enter-new); native entries carry no callbacks.
// Updates both the effective and the stored/authored id, so a save persists the new pick rather than a
// previously fallen-back id.
template <class Registry>
void setActive(Registry &registry, std::string &activeId, std::string &storedId, const std::string &newId) {
    if (newId == activeId || !registry.has(newId))
        return;
    if (const auto *prev = registry.find(activeId); prev && prev->onExit)
        prev->onExit(prev->userData);
    activeId = newId;
    storedId = newId;
    if (const auto *next = registry.find(newId); next && next->onEnter)
        next->onEnter(next->userData);
}

// A plugin is unloading (disabled / unloaded / hot-reloaded / crashed). If it owns the active entry, run
// that entry's onExit best-effort and fall the *effective* id back to `nativeId`; the stored/authored id
// is left intact so a re-save preserves it and a later re-register of that id restores it (see
// onEntryRegistered) — a plugin still has no setter to activate an id the user never authored. onExit is
// a safe no-op once the managed slot is released (the guard turns a post-teardown/crash callback into a
// fallback). Then drop every entry the plugin owns.
template <class Registry>
void unregisterPlugin(Registry &registry, std::string &activeId, const std::string &pluginName, const char *nativeId) {
    if (const auto *active = registry.find(activeId); active && active->owningPlugin == pluginName) {
        if (active->onExit)
            active->onExit(active->userData);
        activeId = nativeId; // stored id untouched — authored id preserved
    }
    registry.removeByPlugin(pluginName);
}

// Re-derive the effective id from the stored/authored id on project load, falling back to `nativeId`
// (with a log line naming `label`) when no loaded plugin registers it — a weak reference (uninstalled
// plugin / foreign file). The stored id keeps pointing at the authored id so a re-save preserves it and
// re-opening with the plugin present restores it.
template <class Registry>
void onProjectLoaded(Registry &registry, std::string &activeId, const std::string &storedId, const char *nativeId,
                     const char *label) {
    activeId = storedId;
    if (!registry.has(activeId)) {
        OFS_CORE_INFO("{} '{}' is not registered; falling back to '{}' (authored id preserved).", label, storedId,
                      nativeId);
        activeId = nativeId;
    }
}

// A plugin just registered `newId`. Restore it as the *effective* id iff it is the authored/stored id and
// the effective id has fallen back to native because that plugin was absent — the deferred completion of
// onProjectLoaded. At startup the project opens a frame before plugins load (LoadProjectEvent drains
// before loadPlugins()), so onProjectLoaded sees no registered entry and falls back; without this the
// authored mode would only restore on a same-session reopen, once the plugin is present. Also covers a
// mid-session hot-reload of the authoring plugin. Guarded to the stored id, so a plugin can never
// silently activate a mode the user did not choose (there is no plugin-callable setter); a user's
// in-session pick moves storedId, so a late-registering old plugin won't override it.
template <class Registry>
void onEntryRegistered(Registry &registry, std::string &activeId, const std::string &storedId,
                       const std::string &newId) {
    if (newId != storedId || activeId == storedId)
        return;
    activeId = storedId;
    if (const auto *next = registry.find(activeId); next && next->onEnter)
        next->onEnter(next->userData);
}

} // namespace ofs::mode_lifecycle
