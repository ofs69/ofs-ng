#pragma once

// Test-only seam into PluginManager: inject a fake native plugin (no CLR required) and read back the
// HostApi / PluginCtx the dispatch path uses. PluginManager grants this struct friendship; the bodies
// live in PluginManagerTestAccess.cpp, compiled only into the plugin/ui test targets. The usual entry
// is addTestPlugin, which bypasses doLoad() and the .NET loader entirely.

#include <string>

namespace ofs {
class PluginManager;
struct PluginApi;
struct HostApi;
struct PluginCtx;

struct PluginManagerTestAccess {
    static void addTestPlugin(PluginManager &pm, const std::string &name, const PluginApi &api, bool enabled = true);
    static const HostApi &hostApi(PluginManager &pm);
    static PluginCtx &pluginCtx(PluginManager &pm);
};
} // namespace ofs
