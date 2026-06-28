// API back-compat witness: load a plugin binary built against an OLDER Ofs.Api against the CURRENT host.
//
// The other load tests build their plugin against today's Ofs.Api, so they re-bind on every build and
// can never reveal an API break. This one loads Ofs.CompatPlugin — a binary the build reproduced from the
// most recent same-major release tag (tools/build_compat_plugin.py, staged under plugins-compat/) and did
// NOT recompile. The script guarantees the baseline is the same Ofs.Api MAJOR as the host, so the
// PluginBootstrapper back-compat guarantee means it MUST load and run. If a same-major change quietly
// dropped or re-signatured a public member this old binary uses, its OnLoad throws (MissingMethodException
// / TypeLoadException) deep in the bridge and the round-trip assertions below go red — exactly the
// regression the recompile otherwise masks.
//
// Skips cleanly (never fails) when no baseline is staged: pre-release (no matching tag), or the first
// release of a new MAJOR before it is tagged. See build_compat_plugin.py for the no-op contract.

#include <doctest/doctest.h>

#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Core/ScriptProject.h"
#include "Services/BindingSystem.h"
#include "Services/CommandRegistry.h"
#include "Services/EffectRegistry.h"
#include "Services/PluginApi.h"
#include "Services/PluginManager.h"
#include "Util/PathUtil.h"
#include "helpers/FakeVideoPlayer.h"
#include "helpers/TestProject.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

using namespace ofs;
using ofs::test::FakeVideoPlayer;
using ofs::test::TestProject;

namespace {
namespace fs = std::filesystem;

LoadedPlugin *findPlugin(std::vector<LoadedPlugin> &plugins, std::string_view name) {
    auto it = std::ranges::find_if(plugins, [&](const LoadedPlugin &p) { return p.name == name; });
    return it == plugins.end() ? nullptr : &*it;
}

#ifdef OFS_COMPAT_PLUGIN_DIR
// Copy the reproduced compat plugin from the build-staged plugins-compat/ into the test pref dir (the only
// root loadPlugins() scans for user plugins). Returns the staged entry DLL, or empty when no baseline was
// produced (so the case skips rather than fails).
fs::path stageCompatPlugin() {
    const fs::path src = fs::path(OFS_COMPAT_PLUGIN_DIR) / "Ofs.CompatPlugin";
    if (!fs::exists(src / "Ofs.CompatPlugin.dll"))
        return {};

    std::error_code ec;
    fs::remove(ofs::util::getPrefPath() / "plugin_states.json", ec); // load enabled, not from prior state

    const fs::path dst = ofs::util::getPrefPath() / "plugins" / "Ofs.CompatPlugin";
    fs::create_directories(dst.parent_path(), ec);
    fs::remove_all(dst, ec);
    fs::copy(src, dst, fs::copy_options::recursive, ec);
    return dst / "Ofs.CompatPlugin.dll";
}
#endif
} // namespace

// Its own doctest suite so ctest can run it as the dedicated `plugin-compat` entry (and exclude it from
// the main `plugins` entry) by suite name — robust to renaming the case below. See tests/CMakeLists.txt.
TEST_SUITE("plugin-compat") {

    TEST_CASE("host loads a plugin built against an older same-major Ofs.Api") {
#ifndef OFS_COMPAT_PLUGIN_DIR
        MESSAGE("back-compat fixture not configured (no Python/git at build) — skipping");
        return;
#else
        const fs::path dll = stageCompatPlugin();
        if (dll.empty()) {
            MESSAGE("no same-major release tag baseline staged — skipping API back-compat check");
            return;
        }

        TestProject tp;
        auto player = std::make_shared<FakeVideoPlayer>();
        auto dummy = std::make_shared<FakeVideoPlayer>();
        CommandRegistry cmdReg{tp.eq};
        RebindState rebind;
        BindingSystem binding{tp.eq, cmdReg, rebind};
        EffectRegistryState effectReg;
        PluginManager pm{tp.project, tp.eq, player, dummy, cmdReg, binding, effectReg};
        tp.eq.freeze();

        REQUIRE(pm.init());
        pm.loadPlugins();

        // The bootstrapper accepted the old binary (same Ofs.Api major, plugin <= host) instead of rejecting
        // it — proof the back-compat gate holds for a plugin we did NOT recompile.
        LoadedPlugin *lp = findPlugin(pm.getPlugins(), "Ofs.CompatPlugin");
        REQUIRE(lp != nullptr);
        CHECK(lp->enabled);
        REQUIRE(lp->api.getName != nullptr);
        CHECK(std::string(lp->api.getName()) == "Test Plugin");

        // OnLoad ran in the old binary and round-tripped through the host bridge: its Commands.Register call
        // landed in the native registry, namespaced by this plugin's load name. If a public API member it uses
        // had been dropped same-major, OnLoad would have thrown before reaching here.
        CHECK(cmdReg.find("Ofs.CompatPlugin.ping") != nullptr);

        // Event dispatch into the old binary's managed callbacks stays sound.
        REQUIRE(lp->api.onProjectChange != nullptr);
        tp.eq.push(LoadProjectEvent{});
        tp.eq.drain();

        // Unload so the DLL isn't left locked for any later case in this process.
        tp.eq.push(SetPluginEnabledEvent{.name = "Ofs.CompatPlugin", .enabled = false});
        tp.eq.drain();
#endif
    }

} // TEST_SUITE("plugin-compat")
