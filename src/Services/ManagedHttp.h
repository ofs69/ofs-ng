#pragma once

// Brings up the .NET-backed HTTP backend and registers it with ofs::util::httpGet. Loads the
// host-internal Ofs.HostServices managed assembly (verified against its baked SHA-256 first, like the
// other managed host assemblies) through its own DotNetHost, resolves the HttpGet entry point, and
// installs a util::HttpImpl that marshals to it. Independent of PluginManager/ScriptSystem: it shares the
// process-singleton CoreCLR runtime but needs neither plugins nor scripts enabled.
//
// Call once on the main thread at startup (after the window is up, alongside the other CoreCLR bring-up).
// Returns false — non-fatally — if dotnet is unavailable, the assembly is missing, or it fails trust; the
// update checker then simply reports no network. Safe to leave uncalled in headless/unit runs.

namespace ofs {

bool initManagedHttp();

} // namespace ofs
