#include "Util/Http.h"

#include <mutex>

namespace ofs::util {
namespace {

// The installed backend. Set once on the main thread at startup (setHttpImpl) and read from a JobSystem
// worker (httpGet), so guard publication with a mutex and hand the worker a copy of the std::function.
std::mutex gImplMutex;
HttpImpl gImpl;

HttpImpl currentImpl() {
    std::lock_guard<std::mutex> lock(gImplMutex);
    return gImpl;
}

} // namespace

void setHttpImpl(HttpImpl impl) {
    std::lock_guard<std::mutex> lock(gImplMutex);
    gImpl = std::move(impl);
}

std::optional<HttpResponse> httpGet(const std::string &url, const std::string &userAgent,
                                    const std::vector<std::string> &headers) {
    if (auto impl = currentImpl())
        return impl(url, userAgent, headers);
    return std::nullopt;
}

} // namespace ofs::util
