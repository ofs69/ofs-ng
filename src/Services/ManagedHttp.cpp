#include "Services/ManagedHttp.h"

#include "Platform/DotNetHost.h"
#include "Services/ManagedAssemblyTrust.h"
#include "Util/Http.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"

#include <coreclr_delegates.h>
#include <cstdint>

namespace ofs {
namespace {

// Native sink the managed HttpGet calls back with the response body. ctx is the HttpResponse to fill;
// the body bytes are valid only for the duration of this call (managed pins them), so copy them out here.
extern "C" void httpResponseSink(void *ctx, int status, const char *body, int len) {
    auto *resp = static_cast<util::HttpResponse *>(ctx);
    resp->status = status;
    if (body != nullptr && len > 0)
        resp->body.assign(body, static_cast<size_t>(len));
}

// Mirror of Ofs.HostServices.HttpService.HttpGet. headers is a '\n'-joined block of "Name: value" lines
// (or null); ctx/sink are forwarded to httpResponseSink. Returns 0 when the request completed (status
// valid, even non-2xx), -1 on a transport failure. The calling convention matches the plugin entry
// points (CORECLR_DELEGATE_CALLTYPE + a plain [UnmanagedCallersOnly] managed method).
typedef int(CORECLR_DELEGATE_CALLTYPE *http_get_native_fn)(const char *url, const char *userAgent, const char *headers,
                                                           void *ctx, void *sink);

http_get_native_fn gHttpGet = nullptr;

std::optional<util::HttpResponse> managedHttpGet(const std::string &url, const std::string &userAgent,
                                                 const std::vector<std::string> &headers) {
    if (gHttpGet == nullptr)
        return std::nullopt;

    // Flatten the header lines into one '\n'-separated block for the managed side to split.
    std::string headerBlock;
    for (const std::string &h : headers) {
        headerBlock += h;
        headerBlock += '\n';
    }

    util::HttpResponse resp;
    const int rc = gHttpGet(url.c_str(), userAgent.c_str(), headerBlock.empty() ? nullptr : headerBlock.c_str(), &resp,
                            reinterpret_cast<void *>(&httpResponseSink));
    if (rc != 0)
        return std::nullopt;
    return resp;
}

} // namespace

bool initManagedHttp() {
    // Ofs.HostServices runs in-process with full privileges; verify its bytes against the baked hash
    // before loading, exactly like the plugin/script hosts.
    if (!managedAssemblyTrusted(ofs::util::getManagedPath() / "Ofs.HostServices.dll", "Ofs.HostServices")) {
        OFS_CORE_WARN("Ofs.HostServices failed trust verification; update checks are disabled");
        return false;
    }

    DotNetHost host;
    if (!host.init()) {
        OFS_CORE_WARN("CoreCLR unavailable; update checks are disabled");
        return false;
    }

    const std::filesystem::path hostPath = ofs::util::getManagedPath() / "Ofs.HostServices.dll";
    if (!host.loadAssembly(hostPath)) {
        OFS_CORE_WARN("Failed to load Ofs.HostServices.dll; update checks are disabled");
        return false;
    }

    gHttpGet = host.getFunctionPointer<http_get_native_fn>(
        hostPath, STR("Ofs.HostServices.HttpService, Ofs.HostServices"), STR("HttpGet"));
    if (gHttpGet == nullptr) {
        OFS_CORE_WARN("Failed to resolve Ofs.HostServices.HttpService.HttpGet; update checks are disabled");
        return false;
    }

    util::setHttpImpl(&managedHttpGet);
    return true;
}

} // namespace ofs
