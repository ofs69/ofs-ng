#include "PluginManagerTestAccess.h"

#include "Services/PluginManager.h"
#include "Util/PathUtil.h"

namespace ofs {

void PluginManagerTestAccess::addTestPlugin(PluginManager &pm, const std::string &name, const PluginApi &api,
                                            bool enabled) {
    LoadedPlugin lp;
    lp.name = name;
    lp.displayName = (api.getName && api.getName()) ? api.getName() : name;
    lp.path = ofs::util::fromUtf8(name);
    lp.api = api;
    lp.enabled = enabled;
    lp.windowOpen = true;
    pm.loadedPlugins.push_back(std::move(lp));
}

const HostApi &PluginManagerTestAccess::hostApi(PluginManager &pm) {
    return pm.hostApi;
}

PluginCtx &PluginManagerTestAccess::pluginCtx(PluginManager &pm) {
    return pm.callCtx_;
}

} // namespace ofs
