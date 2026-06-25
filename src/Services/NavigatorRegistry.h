#pragma once

#include "Services/ModeRegistry.h"
#include "Services/PluginApi.h"

#include <string>

namespace ofs {

// The always-present native navigator: it defers stepping to the active overlay (Frame interval /
// Tempo grid). It is the default selection, owns no plugin state, and is never registered by a
// plugin — so it can never dangle and is the fallback target when a stored or plugin navigator id
// is unknown. Keep in sync with the default of ScriptProject::activeNavigator.
inline constexpr const char *kFollowOverlayNavigatorId = "follow-overlay";

// One selectable navigator. The native follow-overlay entry carries only its id; a plugin navigator
// also carries its display name, the owning plugin (for unload cleanup), and the C-ABI resolver the
// NavigatorRouter dispatches to (onStep → an optional Seek, onEnter/onExit on activation). `userData` is
// the opaque value handed back to the callbacks. The native entry leaves them all null (the router runs
// its built-in resolver and has no per-activation state).
struct NavigatorEntry {
    std::string id;
    std::string displayName;       // shown in the footer; empty for native (footer localizes its own label)
    std::string owningPlugin;      // plugin that registered it; empty for native — used by removeByPlugin
    OfsNavStepFn onStep = nullptr; // null → native follow-overlay resolution
    OfsIntentLifecycleFn onEnter = nullptr;
    OfsIntentLifecycleFn onExit = nullptr;
    OfsIntentUiFn onUi = nullptr; // null → no options UI; the footer shows no options affordance for it
    void *userData = nullptr;
};

// The set of navigators the user can pick between (the footer's Step selector). Seeded with the native
// follow-overlay entry; plugin navigators are appended as plugins load and removed as they unload.
// Read by NavigatorRouter to validate the active selection and dispatch to it, and by the footer.
// All behavior lives in ModeRegistry; this only fixes the entry type and the native seed id.
class NavigatorRegistry : public ModeRegistry<NavigatorEntry> {
  public:
    NavigatorRegistry() : ModeRegistry(kFollowOverlayNavigatorId) {}
};

} // namespace ofs
