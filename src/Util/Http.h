#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

// Thin blocking HTTPS client — the app's only outbound network path (the update checker). Deliberately
// minimal: a single GET is all the checker needs. Safe to call from a JobSystem worker — it touches
// neither ScriptProject nor the frame allocator.
//
// The request is performed by the .NET runtime we already ship (HttpClient, via the Ofs.HostServices
// managed assembly), so no native TLS library is vendored and certificates are verified against the OS
// trust store. The runtime-backed implementation is installed once at startup by the host layer through
// setHttpImpl(); until then (and in headless/no-network test runs) httpGet returns nullopt. Keeping the
// backend behind this seam is what lets Util stay free of any Platform/.NET dependency.

namespace ofs::util {

struct HttpResponse {
    long status = 0; // HTTP status code (e.g. 200); 0 if the request never completed
    std::string body;
};

// Performs a blocking HTTPS GET. Returns nullopt on a transport-level failure (DNS, TLS, timeout, or no
// backend installed) — a completed request with a non-2xx status still returns the response so the caller
// decides. `userAgent` is sent verbatim (the GitHub API rejects requests without one). `headers` are raw
// "Name: value" lines.
std::optional<HttpResponse> httpGet(const std::string &url, const std::string &userAgent,
                                    const std::vector<std::string> &headers = {});

// Backend installed once at startup (main thread) by the host layer; thereafter httpGet forwards to it.
// Left unset in unit/headless runs, where httpGet then simply reports no network.
using HttpImpl = std::function<std::optional<HttpResponse>(const std::string &, const std::string &,
                                                           const std::vector<std::string> &)>;
void setHttpImpl(HttpImpl impl);

} // namespace ofs::util
