#pragma once

#include "Services/ModeRegistry.h"
#include "Services/PluginApi.h"

#include <string>

namespace ofs {

// The always-present native selection mode: it selects every candidate the gesture covers (the host's
// default resolve). It is the default selection, owns no plugin state, and is never registered by a
// plugin — so it can never dangle and is the fallback target when a stored or plugin selection-mode id
// is unknown. Keep in sync with the default of ScriptProject::activeSelectionMode.
inline constexpr const char *kNativeSelectionModeId = "native";

// One selectable selection mode. The native entry carries only its id; a plugin mode also carries its
// display name, the owning plugin (for unload cleanup), and the C-ABI resolver the SelectIntentRouter
// dispatches to (onSelect → Pass / Drop / Replace, onEnter/onExit on activation). `userData` is the
// opaque value handed back to the callbacks. The native entry leaves them all null (the router runs its
// built-in per-axis candidate resolution and has no per-activation state).
struct SelectionModeEntry {
    std::string id;
    std::string displayName;        // shown in the footer; empty for native (footer localizes its own label)
    std::string owningPlugin;       // plugin that registered it; empty for native — used by removeByPlugin
    OfsSelectFn onSelect = nullptr; // null → native resolution (always Pass)
    OfsIntentLifecycleFn onEnter = nullptr;
    OfsIntentLifecycleFn onExit = nullptr;
    OfsIntentUiFn onUi = nullptr; // null → no options UI; the footer shows no options affordance for it
    void *userData = nullptr;
};

// The set of selection modes the user can pick between (the footer's Select selector). Seeded with the
// native entry; plugin modes are appended as plugins load and removed as they unload. Read by
// SelectIntentRouter to validate the active selection and by the footer to list them. Mirrors
// NavigatorRegistry / EditModeRegistry. All behavior lives in ModeRegistry; this only fixes the entry
// type and the native seed id.
class SelectionModeRegistry : public ModeRegistry<SelectionModeEntry> {
  public:
    SelectionModeRegistry() : ModeRegistry(kNativeSelectionModeId) {}
};

} // namespace ofs
