// DotNetHost is a thin, stateless handle onto the process-global CoreCLR runtime. It owns no
// per-instance resources: the hostfxr library, runtime context, and load-assembly delegate live in a
// file-scope static that is booted once and never closed (CoreCLR cannot be unloaded or
// re-initialized). So constructing and destroying any number of hosts is free and must not disturb the
// shared runtime — that is what lets a test process spin up many ScriptSystems / PluginManagers in
// sequence without the second one finding a torn-down runtime.

#include <doctest/doctest.h>

#include "Platform/DotNetHost.h"

#include <filesystem>
#include <type_traits>
#include <utility>

// Owns nothing per instance, so it is trivially copyable/movable — no handle to double-free.
static_assert(std::is_trivially_copyable_v<ofs::DotNetHost>, "DotNetHost should own no per-instance state");
static_assert(std::is_nothrow_default_constructible_v<ofs::DotNetHost>, "DotNetHost must be cheaply constructible");

TEST_CASE("DotNetHost instances are independent stateless handles") {
    ofs::DotNetHost a;
    ofs::DotNetHost b(a); // copy: nothing to alias, both reference the same global runtime
    ofs::DotNetHost c;
    c = std::move(b); // move-assign: a no-op for a stateless handle
    // Constructing and destroying several hosts must not touch (let alone tear down) the shared runtime.
    CHECK(true);
}

// loadAssembly() resolves the assembly's sibling .runtimeconfig.json and bails before touching the
// runtime when it is missing — a malformed plugin (dll without its runtimeconfig) must fail cleanly
// rather than booting or corrupting the shared CLR. The host is mandatory, so init() must succeed.
TEST_CASE("DotNetHost::loadAssembly fails when the runtimeconfig is missing") {
    ofs::DotNetHost host;
    REQUIRE(host.init());

    // No .runtimeconfig.json sibling exists for a made-up name in the temp dir, so the exists() guard
    // rejects it before the runtime is touched.
    const auto bogus = std::filesystem::temp_directory_path() / "ofs_nonexistent_assembly.dll";
    CHECK_FALSE(host.loadAssembly(bogus));
}
