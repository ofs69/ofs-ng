// Plugin install-from-zip tests. Drives the RequestInstallPluginEvent → installFromZip flow and the
// RequestUninstallPluginEvent path end to end, exercising the zip helpers (extractZip, locatePluginDir,
// moveDir, removeDirRetry) and every validation/error branch.
//
// Two tiers:
//   - Validation/error cases need no .NET runtime: the flow rejects a bad zip long before doLoad(), so
//     these run everywhere (a fake native PluginApi is never even reached).
//   - The success + replace + uninstall cases call the real CoreCLR loader (doLoad), so they REQUIRE the
//     .NET runtime and the built Ofs.TestPlugin to be staged next to the binary — exactly like
//     test_plugin_load.cpp. A missing runtime/plugin fails the test, it never skips.
//
// The install flow co_awaits Confirm modals (trust prompt, replace prompt). There is no ModalManager in
// this headless suite, so FakeModalHost stands in: it answers every modal-bearing ShowModalEvent by
// writing a configured button index (or canned dialog path) and resuming the suspended flow inside the
// same drain() — the same hand-off ModalManager::pump() performs after a click.

#include <doctest/doctest.h>

#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Services/BindingSystem.h"
#include "Services/CommandRegistry.h"
#include "Services/EffectRegistry.h"
#include "Services/ManagedAssemblyTrust.h"
#include "Services/PluginApi.h"
#include "Services/PluginManager.h"
#include "UI/Modals.h"
#include "Util/FileUtil.h"
#include "Util/PathUtil.h"
#include "helpers/FakeVideoPlayer.h"
#include "helpers/TestProject.h"

#include <miniz.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using namespace ofs;
using ofs::test::FakeVideoPlayer;
using ofs::test::TestProject;

namespace {
namespace fs = std::filesystem;

// Build a .zip in memory from {archive-path, bytes} entries and write it to `out` (UTF-8 path safe).
bool writeZip(const fs::path &out, const std::vector<std::pair<std::string, std::string>> &entries) {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_heap(&zip, 0, 0))
        return false;
    bool ok = true;
    for (const auto &[name, data] : entries)
        ok = ok && mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION);
    void *buf = nullptr;
    size_t sz = 0;
    ok = ok && mz_zip_writer_finalize_heap_archive(&zip, &buf, &sz);
    if (ok)
        ok = ofs::util::writeFile(out, buf, sz);
    mz_zip_writer_end(&zip);
    return ok;
}

// Zip a real plugin directory under a "<stem>/" prefix so the archive carries the loader's
// "<name>/<name>.dll" shape. Returns false if the source tree can't be read.
bool zipPluginDir(const fs::path &srcDir, const std::string &stem, const fs::path &out) {
    std::vector<std::pair<std::string, std::string>> entries;
    std::error_code ec;
    for (const auto &e : fs::recursive_directory_iterator(srcDir, ec)) {
        if (ec)
            return false;
        if (!e.is_regular_file())
            continue;
        const fs::path rel = fs::relative(e.path(), srcDir, ec);
        if (ec)
            return false;
        auto bytes = ofs::util::readFile(e.path());
        if (!bytes)
            return false;
        // Forward slashes; miniz stores names verbatim and extractZip rebuilds them with fromUtf8.
        std::string name =
            stem + "/" + ofs::util::toUtf8(rel.generic_string()); // utf8-ok: generic_string here is ASCII rel path
        entries.emplace_back(std::move(name), std::move(*bytes));
    }
    return !entries.empty() && writeZip(out, entries);
}

// A headless stand-in for ModalManager: records every modal raised and immediately answers any that
// carries a suspended flow, resuming it within the same drain() (mirrors ModalManager::pump()).
struct FakeModalHost {
    std::vector<ModalSpec> shown; // every modal raised (incl. fire-and-forget showError/showWarning)
    int answer = 0;               // button index handed to Confirm awaiters
    std::string dialogResult;     // path handed to FileDialog awaiters ("" = user cancelled)

    void install(EventQueue &eq) {
        eq.on<ShowModalEvent>([this](const ShowModalEvent &e) {
            shown.push_back(e.spec);
            if (!e.handle)
                return; // fire-and-forget message (showError/showInfo): nothing to resume
            if (e.dialog && e.resultStrSlot)
                *e.resultStrSlot = dialogResult;
            else if (e.resultSlot)
                *e.resultSlot = answer;
            e.handle.resume();
        });
    }

    // True if any raised modal's title contains `needle` (errors use title "Install plugin").
    [[nodiscard]] bool sawTitle(std::string_view needle) const {
        for (const auto &s : shown)
            if (s.title.find(needle) != std::string::npos)
                return true;
        return false;
    }
};

// PluginManager wired to a fresh project with a FakeModalHost and notification capture. Mirrors the
// construction in test_plugin_load.cpp.
struct InstallFixture {
    TestProject tp;
    std::shared_ptr<FakeVideoPlayer> player = std::make_shared<FakeVideoPlayer>();
    std::shared_ptr<FakeVideoPlayer> dummy = std::make_shared<FakeVideoPlayer>();
    CommandRegistry cmdReg{tp.eq};
    RebindState rebind;
    BindingSystem binding{tp.eq, cmdReg, rebind};
    EffectRegistryState effectReg;
    PluginManager pm{tp.project, tp.eq, player, dummy, cmdReg, binding, effectReg};
    FakeModalHost modals;
    std::vector<NotifyEvent> notes;

    InstallFixture() {
        modals.install(tp.eq);
        tp.eq.on<NotifyEvent>([this](const NotifyEvent &e) { notes.push_back(e); });
        tp.eq.freeze();
    }

    EventQueue &eq() { return tp.eq; }

    void requestInstall(const fs::path &zip) {
        tp.eq.push(RequestInstallPluginEvent{.path = ofs::util::toUtf8(zip)});
        tp.eq.drain();
    }

    bool installedNote() const {
        for (const auto &n : notes)
            if (n.level == NotifyLevel::Success && n.message.find("Installed") != std::string::npos)
                return true;
        return false;
    }

    LoadedPlugin *find(std::string_view name) {
        for (auto &p : pm.getPlugins())
            if (p.name == name)
                return &p;
        return nullptr;
    }
};

// A scratch directory under temp for a test's input zips, removed on scope exit.
struct TempDir {
    fs::path dir;
    explicit TempDir(const char *tag) {
        std::error_code ec;
        dir = fs::temp_directory_path(ec) / "ofs-install-test" / tag;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

// Delete a (possibly just-unloaded) plugin folder under <pref>/plugins, retrying briefly in case the
// CLR is still releasing the DLL. Keeps the shared pref dir clean between cases.
void purgeInstalled(const std::string &stem) {
    const fs::path dir = ofs::util::getPrefPath() / "plugins" / ofs::util::fromUtf8(stem);
    std::error_code ec;
    for (int i = 0; i < 50 && fs::exists(dir); ++i) {
        fs::remove_all(dir, ec);
        if (fs::exists(dir))
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    fs::remove(ofs::util::getPrefPath() / "plugin_states.json", ec);
}

// The three sibling files installFromZip requires for a structurally valid plugin folder.
std::vector<std::pair<std::string, std::string>> pluginTrio(const std::string &stem, const std::string &dllBytes) {
    return {{stem + "/" + stem + ".dll", dllBytes},
            {stem + "/" + stem + ".runtimeconfig.json", "{}"},
            {stem + "/" + stem + ".deps.json", "{}"}};
}
} // namespace

// ── Validation / error paths (no .NET runtime required) ──────────────────────────────────────────

TEST_CASE("install rejects a path that no longer exists") {
    InstallFixture f;
    f.requestInstall(ofs::util::getPrefPath() / "definitely-missing.zip");
    CHECK(f.modals.sawTitle("Install plugin")); // the error modal
    CHECK_FALSE(f.installedNote());
}

TEST_CASE("install rejects a file that is not a zip archive") {
    TempDir t{"notzip"};
    const fs::path zip = t.dir / "broken.zip";
    REQUIRE(ofs::util::writeFile(zip, "this is not a zip archive"));

    InstallFixture f;
    f.requestInstall(zip);
    CHECK(f.modals.sawTitle("Install plugin"));
    CHECK_FALSE(f.installedNote());
}

TEST_CASE("install rejects a zip with no plugin folder") {
    TempDir t{"noplugin"};
    const fs::path zip = t.dir / "docs.zip";
    REQUIRE(writeZip(zip, {{"readme.txt", "hello"}, {"docs/guide.md", "stuff"}}));

    InstallFixture f;
    f.requestInstall(zip);
    CHECK(f.modals.sawTitle("Install plugin"));
    CHECK_FALSE(f.installedNote());
}

TEST_CASE("install rejects a plugin folder missing the runtimeconfig/deps siblings") {
    TempDir t{"notrio"};
    const fs::path zip = t.dir / "Foo.zip";
    // name/name.dll is present (locatePluginDir accepts it) but the entry trio is incomplete.
    REQUIRE(writeZip(zip, {{"Foo/Foo.dll", "MZ\x00\x00"}}));

    InstallFixture f;
    f.requestInstall(zip);
    CHECK(f.modals.sawTitle("Install plugin"));
    CHECK_FALSE(f.installedNote());
    CHECK(f.find("Foo") == nullptr);
}

TEST_CASE("install rejects a name reserved for a built-in plugin") {
    REQUIRE(isFirstPartyName("Ofs.Core")); // guards the test against the reserved set changing
    TempDir t{"reserved"};
    const fs::path zip = t.dir / "Ofs.Core.zip";
    REQUIRE(writeZip(zip, pluginTrio("Ofs.Core", "MZ-bytes")));

    InstallFixture f;
    f.requestInstall(zip);
    CHECK(f.modals.sawTitle("Install plugin"));
    CHECK_FALSE(f.installedNote());
    CHECK(f.find("Ofs.Core") == nullptr);
}

// managedAssemblyTrusted / firstPartyHashMatches are pure hash-table lookups against the baked
// OfsBakedHashes.h. The baked hashes differ per build, so these cases verify only the
// build-independent branches: an unknown name, an unreadable file, and a wrong-bytes mismatch.
TEST_CASE("managedAssemblyTrusted accepts an unknown assembly name (nothing to verify against)") {
    // No hash is baked for a name that isn't one of the managed host assemblies, so it can't be
    // checked and must not block — returns true.
    CHECK(ofs::managedAssemblyTrusted("any/path/Whatever.dll", "NotAManagedAssembly"));
}

TEST_CASE("managedAssemblyTrusted rejects a known assembly whose file cannot be read") {
    // Ofs.Api has a baked hash, so the file must be read and hashed; a missing file is a hard fail.
    CHECK_FALSE(ofs::managedAssemblyTrusted("does_not_exist_zzz/Ofs.Api.dll", "Ofs.Api"));
}

TEST_CASE("managedAssemblyTrusted rejects a known assembly whose bytes do not match the baked hash") {
    TempDir t{"trust"};
    const fs::path dll = t.dir / "Ofs.Api.dll";
    REQUIRE(ofs::util::writeFile(dll, "definitely not the real assembly bytes"));
    CHECK_FALSE(ofs::managedAssemblyTrusted(dll, "Ofs.Api"));
}

TEST_CASE("firstPartyHashMatches needs both a shipped name and the exact baked hash") {
    REQUIRE(isFirstPartyName("Ofs.Core")); // guards against the reserved set changing
    // Right name, wrong hash → name alone is never enough, so it falls through (false).
    CHECK_FALSE(
        ofs::firstPartyHashMatches("Ofs.Core", "0000000000000000000000000000000000000000000000000000000000000000"));
    // Unknown name → never a first-party match regardless of hash.
    CHECK_FALSE(ofs::firstPartyHashMatches("Some.Other.Plugin", "deadbeef"));
}

TEST_CASE("install rejects a zip-slip entry escaping the staging dir") {
    TempDir t{"zipslip"};
    const fs::path zip = t.dir / "evil.zip";
    // A "../" entry must make extractZip bail before anything is written outside staging.
    REQUIRE(writeZip(zip, {{"Foo/Foo.dll", "x"}, {"../escape.txt", "pwned"}}));

    InstallFixture f;
    f.requestInstall(zip);
    CHECK(f.modals.sawTitle("Install plugin"));
    CHECK_FALSE(f.installedNote());
    CHECK_FALSE(fs::exists(t.dir.parent_path() / "escape.txt")); // nothing escaped
}

TEST_CASE("declining the trust prompt installs nothing") {
    const std::string stem = "DeclineMe";
    purgeInstalled(stem);
    TempDir t{"decline"};
    const fs::path zip = t.dir / "DeclineMe.zip";
    REQUIRE(writeZip(zip, pluginTrio(stem, "fake-dll-bytes")));

    InstallFixture f;
    f.modals.answer = 1; // "Cancel" on the "Load plugin?" prompt
    f.requestInstall(zip);

    CHECK(f.modals.sawTitle("Load plugin?")); // the trust prompt was reached (DLL hashed, then declined)
    CHECK_FALSE(f.installedNote());
    CHECK(f.find(stem) == nullptr);
    CHECK_FALSE(fs::exists(ofs::util::getPrefPath() / "plugins" / stem)); // nothing committed
}

TEST_CASE("onRequestInstallPlugin with no path opens the picker and honors cancel") {
    InstallFixture f;
    f.modals.dialogResult = ""; // user cancels the file picker
    f.eq().push(RequestInstallPluginEvent{.path = ""});
    f.eq().drain();
    CHECK_FALSE(f.installedNote());
    CHECK_FALSE(f.modals.sawTitle("Install plugin")); // cancelled before any install/error modal
}

// ── Success / replace / uninstall (real CoreCLR loader) ──────────────────────────────────────────

namespace {
// Locate the built Ofs.TestPlugin staged next to the binary (getBasePath()/plugins/Ofs.TestPlugin).
// Empty when it wasn't built; the CLR cases REQUIRE it to be non-empty.
fs::path testPluginSrc() {
    const fs::path src = ofs::util::getBasePath() / "plugins" / "Ofs.TestPlugin";
    return fs::exists(src) ? src : fs::path{};
}
} // namespace

TEST_CASE("install loads a real plugin from a zip, then uninstall removes it") {
    InstallFixture f;
    REQUIRE(f.pm.init());
    const fs::path src = testPluginSrc();
    REQUIRE_FALSE(src.empty());
    const std::string stem = "Ofs.TestPlugin";
    purgeInstalled(stem);

    TempDir t{"success"};
    const fs::path zip = t.dir / "plugin.zip";
    REQUIRE(zipPluginDir(src, stem, zip));

    // Install: trust prompt answered "Load" (index 0). Commit (moveDir) + hot-load (doLoad) run.
    f.requestInstall(zip);

    LoadedPlugin *lp = f.find(stem);
    REQUIRE(lp != nullptr);
    CHECK(lp->enabled);
    CHECK(f.installedNote());
    CHECK(fs::exists(ofs::util::getPrefPath() / "plugins" / stem / (stem + ".dll")));

    // Uninstall: confirm "Uninstall" (index 0) → unload + delete the folder (removeDirRetry).
    f.modals.answer = 0;
    f.eq().push(RequestUninstallPluginEvent{.name = stem});
    f.eq().drain();

    CHECK(f.find(stem) == nullptr);
    bool gone = false;
    for (int i = 0; i < 50 && !gone; ++i) {
        gone = !fs::exists(ofs::util::getPrefPath() / "plugins" / stem);
        if (!gone)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(gone);
}

TEST_CASE("updating a loaded plugin reinstalls and reloads it (commands survive)") {
    InstallFixture f;
    REQUIRE(f.pm.init());
    const fs::path src = testPluginSrc();
    REQUIRE_FALSE(src.empty());
    const std::string stem = "Ofs.TestPlugin";
    purgeInstalled(stem);

    TempDir t{"update"};
    const fs::path zip = t.dir / "plugin.zip";
    REQUIRE(zipPluginDir(src, stem, zip));

    // First install: the plugin loads and its OnLoad registers the "ping" command.
    f.requestInstall(zip);
    REQUIRE(f.find(stem) != nullptr);
    REQUIRE(f.find(stem)->enabled);
    REQUIRE(f.cmdReg.find("Ofs.TestPlugin.ping") != nullptr);

    // Update: the user installs a new build over the running plugin. The replace path unloads the live
    // instance, then reloads from the new bytes — the reload's OnLoad re-registers "ping". This must not
    // collide with the still-present registration nor be clobbered by the deferred unregister.
    f.notes.clear();
    f.modals.shown.clear();
    f.requestInstall(zip); // replace prompt (0) + trust prompt (0)
    f.eq().drain();        // let the deferred unregister/register events settle

    CHECK(f.modals.sawTitle("Install plugin")); // the "already installed — replace?" modal
    REQUIRE(f.find(stem) != nullptr);           // the reload succeeded
    CHECK(f.find(stem)->enabled);
    CHECK(f.installedNote());
    CHECK(f.cmdReg.find("Ofs.TestPlugin.ping") != nullptr); // command re-registered and still live

    f.eq().push(SetPluginEnabledEvent{.name = stem, .enabled = false});
    f.eq().drain();
    purgeInstalled(stem);
}

TEST_CASE("installing over an existing (unloaded) plugin takes the replace path") {
    InstallFixture f;
    REQUIRE(f.pm.init());
    const fs::path src = testPluginSrc();
    REQUIRE_FALSE(src.empty());
    const std::string stem = "Ofs.TestPlugin";
    purgeInstalled(stem);

    // Seed an existing (but never-loaded) install folder so installFromZip sees destDir already present
    // and runs the replace path: confirm "Replace", wipe the old folder, then move + load the new one.
    // The folder is not in loadedPlugins, so this stays clear of the deferred-command-unregister hazard
    // that a replace of a *loaded* command-registering plugin would hit (see test-suite notes).
    const fs::path destDir = ofs::util::getPrefPath() / "plugins" / stem;
    std::error_code ec;
    fs::create_directories(destDir, ec);
    REQUIRE(ofs::util::writeFile(destDir / "stale.txt", "old contents"));

    TempDir t{"replace"};
    const fs::path zip = t.dir / "plugin.zip";
    REQUIRE(zipPluginDir(src, stem, zip));

    // Replace prompt (0 = "Replace") then trust prompt (0 = "Load"); both answered by FakeModalHost.
    f.requestInstall(zip);

    CHECK(f.modals.sawTitle("Install plugin")); // the "already installed — replace?" modal
    CHECK(f.modals.sawTitle("Load plugin?"));
    REQUIRE(f.find(stem) != nullptr);
    CHECK(f.find(stem)->enabled);
    CHECK(f.installedNote());
    CHECK_FALSE(fs::exists(destDir / "stale.txt")); // the old folder was wiped, not merged

    // Unload so the DLL lock is released for later cases, then clean up.
    f.eq().push(SetPluginEnabledEvent{.name = stem, .enabled = false});
    f.eq().drain();
    purgeInstalled(stem);
}
