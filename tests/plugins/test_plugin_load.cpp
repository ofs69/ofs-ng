// Real CoreCLR load/unload tests — boot the .NET runtime and drive a genuine C# plugin
// (tests/plugins/Ofs.TestPlugin) through the full native<->managed bridge.
//
// Requires the .NET runtime plus the staged managed/ assemblies and the built test
// plugin next to the binary (see tests/CMakeLists.txt). .NET is a hard requirement: a
// failure to init the host or stage the test plugin fails the test — it never skips.

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
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

using namespace ofs;
using ofs::test::FakeVideoPlayer;
using ofs::test::TestProject;

namespace {
namespace fs = std::filesystem;

// The loader scans <pref>/plugins as the only user root (the base root next to the executable loads
// first-party plugins by name only). Stage the built test plugin into the test pref dir so
// loadPlugins() picks it up, and return its entry DLL path. Empty when the plugin wasn't built next
// to the binary — callers REQUIRE it to be non-empty (the plugin is mandatory).
fs::path stageTestPlugin() {
    std::error_code ec;
    // Start each case from clean persisted state: the pref dir is shared across cases and runs, so a
    // prior disable/uninstall would otherwise make the plugin load disabled (or not at all).
    fs::remove(ofs::util::getPrefPath() / "plugin_states.json", ec);
    fs::remove(ofs::util::getPrefPath() / "plugins_pending_uninstall.json", ec);

    const fs::path src = ofs::util::getBasePath() / "plugins" / "Ofs.TestPlugin";
    const fs::path dst = ofs::util::getPrefPath() / "plugins" / "Ofs.TestPlugin";
    if (!fs::exists(src))
        return {};
    fs::create_directories(dst.parent_path(), ec);
    fs::remove_all(dst, ec); // succeeds once any prior case unloaded the plugin (DLL no longer locked)
    fs::copy(src, dst, fs::copy_options::recursive, ec);
    return dst / "Ofs.TestPlugin.dll";
}

LoadedPlugin *findPlugin(std::vector<LoadedPlugin> &plugins, std::string_view name) {
    auto it = std::ranges::find_if(plugins, [&](const LoadedPlugin &p) { return p.name == name; });
    return it == plugins.end() ? nullptr : &*it;
}
} // namespace

TEST_CASE("PluginManager loads a real C# plugin through the CoreCLR bridge") {
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
    REQUIRE_FALSE(stageTestPlugin().empty());

    pm.loadPlugins();

    LoadedPlugin *lp = findPlugin(pm.getPlugins(), "Ofs.TestPlugin");
    REQUIRE(lp != nullptr);

    // The managed bridge filled the PluginApi struct and reported the plugin name.
    CHECK(lp->enabled);
    CHECK(lp->api.version == OFS_ABI_VERSION);
    REQUIRE(lp->api.getName != nullptr);
    CHECK(std::string(lp->api.getName()) == "Test Plugin");

    // Version flows reflection (AssemblyInformationalVersion) -> bridge -> ABI -> host. The test
    // plugin pins it to 2.3.4 in its .csproj.
    REQUIRE(lp->api.getVersion != nullptr);
    CHECK(std::string(lp->api.getVersion()) == "2.3.4");
    CHECK(lp->version == "2.3.4");

    // OnLoad ran in managed code and called back through HostApi.registerCommand,
    // mutating the native registry — a full native -> managed -> native round-trip.
    CHECK(cmdReg.find("Ofs.TestPlugin.ping") != nullptr);

    // Event dispatch reaches managed callbacks without crashing.
    REQUIRE(lp->api.onProjectChange != nullptr);
    tp.eq.push(LoadProjectEvent{});
    tp.eq.drain();

    // Unload before the case ends so the assembly's DLL isn't left locked for later cases in this
    // process (a loaded assembly keeps a file lock that would block them from re-staging the plugin).
    tp.eq.push(SetPluginEnabledEvent{.name = "Ofs.TestPlugin", .enabled = false});
    tp.eq.drain();
}

// The plugin registers nodes (TestPlugin.cs), so its delegates land in Ofs.Api's static node-slot
// tables. Those tables used to keep the delegates — and thus the whole plugin assembly — alive for
// the entire process, so disabling a plugin never actually unloaded it and its DLL stayed locked.
// This drives the full lifecycle and proves the assembly is released: the DLL becomes deletable.
TEST_CASE("PluginManager unloads a plugin completely and can reload it") {
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
    const fs::path dll = stageTestPlugin();
    REQUIRE_FALSE(dll.empty());

    pm.loadPlugins();
    tp.eq.drain(); // flush the RegisterPluginNodeEvent(s) the plugin pushed during OnLoad

    REQUIRE(findPlugin(pm.getPlugins(), "Ofs.TestPlugin") != nullptr);
    REQUIRE(findPlugin(pm.getPlugins(), "Ofs.TestPlugin")->enabled);
    REQUIRE(fs::exists(dll));
    // The plugin's nodes are live in the registry.
    CHECK(effectReg.pluginNodes.count("Ofs.TestPlugin.fgen") == 1);
    CHECK(effectReg.pluginNodes.count("Ofs.TestPlugin.dgen") == 1);

    // ── Disable → unload. Releases the node slots, forces GC, unloads the AssemblyLoadContext. ──
    tp.eq.push(SetPluginEnabledEvent{.name = "Ofs.TestPlugin", .enabled = false});
    tp.eq.drain();

    LoadedPlugin *disabled = findPlugin(pm.getPlugins(), "Ofs.TestPlugin");
    REQUIRE(disabled != nullptr);
    CHECK_FALSE(disabled->enabled);
    CHECK(disabled->api.getName == nullptr);                        // PluginApi struct cleared on unload
    CHECK(effectReg.pluginNodes.count("Ofs.TestPlugin.fgen") == 0); // nodes unregistered

    // ── Re-enable → reload from the same DLL. Proves a clean unload left it loadable again. ──
    tp.eq.push(SetPluginEnabledEvent{.name = "Ofs.TestPlugin", .enabled = true});
    tp.eq.drain();

    LoadedPlugin *reloaded = findPlugin(pm.getPlugins(), "Ofs.TestPlugin");
    REQUIRE(reloaded != nullptr);
    CHECK(reloaded->enabled);
    REQUIRE(reloaded->api.getName != nullptr);
    CHECK(std::string(reloaded->api.getName()) == "Test Plugin");
    CHECK(effectReg.pluginNodes.count("Ofs.TestPlugin.fgen") == 1); // nodes back

    // ── Disable again, then prove the DLL is fully released. ──
    tp.eq.push(SetPluginEnabledEvent{.name = "Ofs.TestPlugin", .enabled = false});
    tp.eq.drain();

    // On Windows a loaded assembly's DLL cannot be deleted; success here is concrete proof the plugin
    // fully unloaded (the bug: it never did for the whole session). A few brief retries cover any lag
    // in the OS closing the image handle after the managed unload.
    std::error_code ec;
    bool deleted = false;
    for (int i = 0; i < 50 && !deleted; ++i) {
        fs::remove(dll, ec);
        deleted = !fs::exists(dll);
        if (!deleted)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(deleted);
}

// LoadMainAssembly loads the plugin from bytes (leaving the DLL unlocked for hot-reload) and loads a
// sibling .pdb alongside only when one is present. A Debug build ships a .pdb, so the normal load
// exercises the with-pdb branch; removing it before load drives the no-pdb branch — the plugin must
// still load and run.
TEST_CASE("PluginManager loads a plugin whose .pdb is absent") {
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
    const fs::path dll = stageTestPlugin();
    REQUIRE_FALSE(dll.empty());
    std::error_code ec;
    fs::remove(fs::path(dll).replace_extension(".pdb"), ec); // force the no-pdb load path

    pm.loadPlugins();

    LoadedPlugin *lp = findPlugin(pm.getPlugins(), "Ofs.TestPlugin");
    REQUIRE(lp != nullptr);
    CHECK(lp->enabled);
    REQUIRE(lp->api.getName != nullptr);
    CHECK(std::string(lp->api.getName()) == "Test Plugin"); // OnLoad ran → bridge wired without a pdb

    // Unload so the DLL isn't left locked for later cases in this process.
    tp.eq.push(SetPluginEnabledEvent{.name = "Ofs.TestPlugin", .enabled = false});
    tp.eq.drain();
}
