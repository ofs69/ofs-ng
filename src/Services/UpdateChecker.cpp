#include "Services/UpdateChecker.h"

#include "Core/EventQueue.h"
#include "Services/JobSystem.h"
#include "Util/Http.h"
#include "Util/Log.h"
#include "Util/SemVer.h"

#include <OfsBuildInfo.h> // generated: ofs::generated::kGitTag — the running build's version anchor
#include <mutex>
#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

namespace ofs {
namespace {

// GitHub's "latest release" endpoint already excludes drafts and prereleases, so a stable-only check is
// just this one GET; the semver comparison below is the safety net.
constexpr const char *kReleasesUrl = "https://api.github.com/repos/ofs69/ofs-ng/releases/latest";
constexpr const char *kUserAgent = "ofs-ng-update-checker";

// Test override (see header). Read from a worker thread, set from the main thread before a check — guard
// the access with a mutex and hand back a copy so the worker never races a concurrent set.
std::mutex gFetchOverrideMutex;
std::function<std::optional<std::string>()> gFetchOverride;

std::function<std::optional<std::string>()> currentFetchOverride() {
    std::lock_guard<std::mutex> lock(gFetchOverrideMutex);
    return gFetchOverride;
}

// Test override of the running build's version tag (empty = use the baked git identity). Same cross-
// thread access pattern as the fetch override.
std::mutex gCurrentVersionMutex;
std::string gCurrentVersionOverride;

std::string currentVersionTag() {
    std::lock_guard<std::mutex> lock(gCurrentVersionMutex);
    return gCurrentVersionOverride.empty() ? std::string(generated::kGitTag) : gCurrentVersionOverride;
}

// Worker-thread body: obtain the release feed (real GET or the test override), parse it, compare against
// the running build, and push exactly one result event. Never touches the service or ScriptProject.
void runCheck(EventQueue &eq, bool userInitiated) {
    std::optional<std::string> body;
    if (auto fetchOverride = currentFetchOverride()) {
        body = fetchOverride();
        if (!body) {
            eq.push(UpdateCheckFailedEvent{.reason = "network unavailable", .userInitiated = userInitiated});
            return;
        }
    } else {
        auto resp = util::httpGet(kReleasesUrl, kUserAgent,
                                  {"Accept: application/vnd.github+json", "X-GitHub-Api-Version: 2022-11-28"});
        if (!resp) {
            eq.push(UpdateCheckFailedEvent{.reason = "network unavailable", .userInitiated = userInitiated});
            return;
        }
        if (resp->status != 200) {
            eq.push(
                UpdateCheckFailedEvent{.reason = fmt::format("HTTP {}", resp->status), .userInitiated = userInitiated});
            return;
        }
        body = std::move(resp->body);
    }

    std::string tag;
    std::string url;
    std::string notes;
    try {
        const auto json = nlohmann::json::parse(*body);
        tag = json.at("tag_name").get<std::string>();
        url = json.value("html_url", std::string{});
        notes = json.value("body", std::string{});
    } catch (const std::exception &ex) {
        eq.push(UpdateCheckFailedEvent{.reason = fmt::format("malformed release feed: {}", ex.what()),
                                       .userInitiated = userInitiated});
        return;
    }

    const auto latest = util::parseSemVer(tag);
    if (!latest) {
        eq.push(UpdateCheckFailedEvent{.reason = fmt::format("unrecognized release tag '{}'", tag),
                                       .userInitiated = userInitiated});
        return;
    }

    // The running build's own version. Empty/unparseable (a build made outside a git checkout) means we
    // cannot tell whether the release is newer — stay quiet rather than nag with a false positive.
    const std::string currentTag = currentVersionTag();
    const auto current = util::parseSemVer(currentTag);
    if (current && util::isNewer(*latest, *current))
        eq.push(
            UpdateAvailableEvent{.version = tag, .releaseUrl = url, .notes = notes, .userInitiated = userInitiated});
    else
        eq.push(UpdateUpToDateEvent{.userInitiated = userInitiated});
}

} // namespace

void setUpdateFetchOverrideForTesting(std::function<std::optional<std::string>()> fn) {
    std::lock_guard<std::mutex> lock(gFetchOverrideMutex);
    gFetchOverride = std::move(fn);
}

void setUpdateCurrentVersionForTesting(std::string version) {
    std::lock_guard<std::mutex> lock(gCurrentVersionMutex);
    gCurrentVersionOverride = std::move(version);
}

UpdateChecker::UpdateChecker(EventQueue &eq, JobSystem &jobs) : eq_(eq), jobs_(jobs) {
    eq_.on<CheckForUpdatesEvent>([this](const CheckForUpdatesEvent &e) { onCheckRequested(e); });

    // The result handlers run on the main thread (drained from the worker's push) and are the only writers
    // of status_/inFlight_. The service subscribes to its own results so the UI can read a coherent Status.
    eq_.on<UpdateAvailableEvent>([this](const UpdateAvailableEvent &e) {
        status_ = {.state = State::Available, .latestVersion = e.version, .releaseUrl = e.releaseUrl, .notes = e.notes};
        inFlight_ = false;
    });
    eq_.on<UpdateUpToDateEvent>([this](const UpdateUpToDateEvent &) {
        status_ = {.state = State::UpToDate};
        inFlight_ = false;
    });
    eq_.on<UpdateCheckFailedEvent>([this](const UpdateCheckFailedEvent &e) {
        status_ = {.state = State::Failed, .error = e.reason};
        inFlight_ = false;
        OFS_CORE_WARN("Update check failed: {}", e.reason);
    });
}

void UpdateChecker::onCheckRequested(const CheckForUpdatesEvent &e) {
    if (inFlight_)
        return;
    inFlight_ = true;
    status_.state = State::Checking;

    EventQueue *eq = &eq_;
    const bool userInitiated = e.userInitiated;
    jobs_.submitTask([eq, userInitiated]() -> bool {
        runCheck(*eq, userInitiated);
        return true;
    });
}

} // namespace ofs
