#pragma once

#include "Services/ModeRegistry.h"
#include "Services/PluginApi.h"

#include <string>

namespace ofs {

// The always-present native edit mode: it applies every edit intent unchanged (the host's default
// resolve). It is the default selection, owns no plugin state, and is never registered by a plugin —
// so it can never dangle and is the fallback target when a stored or plugin edit-mode id is unknown.
// Keep in sync with the default of ScriptProject::activeEditMode.
inline constexpr const char *kNativeEditModeId = "native";

// One selectable edit mode. The native entry carries only its id; a plugin mode also carries its
// display name, the owning plugin (for unload cleanup), and the C-ABI callbacks the EditIntentRouter
// dispatches to (onEditIntent for Pass/Drop/Replace, onEnter/onExit on activation). `userData` is the
// opaque value handed back to each callback. A native mode leaves the callbacks null.
struct EditModeEntry {
    std::string id;
    std::string displayName;                // shown in the footer; empty for native (footer localizes its own label)
    std::string owningPlugin;               // plugin that registered it; empty for native — used by removeByPlugin
    OfsEditIntentFn onEditIntent = nullptr; // null → native behavior (always Pass)
    OfsIntentLifecycleFn onEnter = nullptr;
    OfsIntentLifecycleFn onExit = nullptr;
    OfsIntentUiFn onUi = nullptr; // null → no options UI; the footer shows no options affordance for it
    void *userData = nullptr;
};

// The set of edit modes the user can pick between (the footer's Edit-mode selector). Seeded with the
// native entry; plugin modes are appended as plugins load and removed as they unload. Read by
// EditIntentRouter to validate the active selection and dispatch to it, and by the footer to list them.
// All behavior lives in ModeRegistry; this only fixes the entry type and the native seed id.
class EditModeRegistry : public ModeRegistry<EditModeEntry> {
  public:
    EditModeRegistry() : ModeRegistry(kNativeEditModeId) {}
};

} // namespace ofs
