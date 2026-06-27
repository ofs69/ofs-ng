#pragma once

#include "Services/EditModeRegistry.h"
#include "Services/EffectRegistry.h"
#include "Services/NavigatorRegistry.h"
#include "Services/SelectionModeRegistry.h"

#include <string>

namespace ofs {

struct SetPluginEnabledEvent {
    std::string name;
    bool enabled;
};

struct SavePluginStatesEvent {};

// Transient per-plugin developer toggle: when on, PluginManager polls this plugin's DLL and
// hot-reloads it when the bytes change (e.g. after a rebuild). Session-only — never persisted.
struct SetPluginHotReloadEvent {
    std::string name;
    bool enabled;
};

// Install a plugin into the user plugins dir (<pref>/plugins) from a .zip archive (the unit the
// plugin index releases). `path` is a UTF-8 zip path; empty means PluginManager should open a
// picker. Handled entirely in the service — the UI only pushes the request (no dialog, no file work
// in the UI layer).
struct RequestInstallPluginEvent {
    std::string path;
};

// Uninstall a user-installed plugin: unload it and delete <pref>/plugins/<name>. Shipped first-party
// plugins (base root) are not removable through this flow.
struct RequestUninstallPluginEvent {
    std::string name;
};

struct RegisterPluginNodeEvent {
    PluginNodeEntry entry;
};

struct UnregisterPluginNodesEvent {
    std::string pluginName;
};

// Plugin-registered edit mode / navigator publication (mirrors RegisterPluginNodeEvent). The router for
// each extension point is the sole subscriber: it adds the entry to its registry, after which the footer
// lists it. Unregister removes every entry a plugin owns when it unloads.
struct RegisterEditModeEvent {
    EditModeEntry entry;
};
struct UnregisterEditModesEvent {
    std::string pluginName;
};
struct RegisterNavigatorEvent {
    NavigatorEntry entry;
};
struct UnregisterNavigatorsEvent {
    std::string pluginName;
};
struct RegisterSelectionModeEvent {
    SelectionModeEntry entry;
};
struct UnregisterSelectionModesEvent {
    std::string pluginName;
};

// Second phase of a UI-language switch: reload every plugin that onSetLanguage just tore down. Pushed
// AFTER the per-plugin UnregisterPlugin* events so those drain first (clearing the old command/node ids)
// before doLoad re-registers in the new language — command registration is synchronous, so reloading in
// the same handler as the teardown would collide with the still-present old ids.
struct ReloadPluginsForLanguageEvent {};

} // namespace ofs
