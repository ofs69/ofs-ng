#pragma once

#include <filesystem>
#include <span>
#include <string_view>

// Trust checks against the SHA-256 hashes baked into the binary at build time. The generated header
// (OfsBakedHashes.h) changes on every plugin/managed-assembly byte change, so the implementation is
// confined to ManagedAssemblyTrust.cpp — the one TU that includes it — and callers depend only on
// these declarations.

namespace ofs {

// Verify a managed host assembly on disk matches the SHA-256 baked into the binary at build time.
// Ofs.Api / Ofs.PluginHost / Ofs.ScriptHost run all plugin and script code with full privileges, and
// Ofs.HostServices performs network I/O in-process, so a mismatch (tampering or a corrupt/partial
// install) must block loading rather than prompt.
//
// Returns true when the on-disk bytes match the baked hash. Returns false on a real mismatch or an
// unreadable file — callers refuse to load. If no hash was baked for `name` (empty, e.g. a
// dotnet-less or pre-build baseline), there is nothing to verify against and it returns true so such
// builds are not bricked.
bool managedAssemblyTrusted(const std::filesystem::path &dll, std::string_view name);

// True if `name` is a shipped first-party plugin AND `hash` matches the DLL this build baked in
// (see cmake/GenBakedHashes.cmake). A first-party-named DLL whose bytes don't match the baked hash
// returns false, so it falls through to the normal trust prompt — name alone is never enough.
bool firstPartyHashMatches(std::string_view name, std::string_view hash);

// True if `name` is a shipped first-party plugin name. The base root loads only these (by name), and
// the user root reserves them so an installed plugin can never shadow or impersonate a shipped one.
bool isFirstPartyName(std::string_view name);

// The shipped first-party plugin names, in baked order. Backed by static storage valid for the
// program lifetime.
std::span<const std::string_view> firstPartyPluginNames();

} // namespace ofs
