#include "Core/UpdateEvents.h"
#include "Services/UpdateChecker.h"
#include "UI/Notifications.h"
#include "helpers/TestState.h"
#include <imgui_te_context.h>
#include <imgui_te_engine.h>
#include <optional>
#include <string>

using namespace ofs;

// Drives the update-checker surface end to end without touching the network: the fetch is replaced by an
// injected release feed (setUpdateFetchOverrideForTesting) and the running build's version is pinned
// (setUpdateCurrentVersionForTesting), so a click on the About window's "Check Now" flows through the
// real worker job, parse, semver comparison and result event into the About status / footer toast.

namespace {
constexpr const char *kAbout = "//About ofs-ng###about"; // visible prefix is ignored; the ###id matches
constexpr const char *kAboutMenu = "//##MainMenuBar/###menu_help/###menu_help_about";

void openAbout(ImGuiTestContext *ctx) {
    loadFixture(ctx);
    if (ctx->WindowInfo(kAbout, ImGuiTestOpFlags_NoError).Window == nullptr) {
        ctx->MenuClick(kAboutMenu);
        ctx->Yield(2);
    }
    ctx->SetRef(kAbout);
}

// Restore the harness default (an empty feed, real version) so no later test reaches the network or
// inherits a pinned version.
void resetOverrides() {
    setUpdateFetchOverrideForTesting([] { return std::optional<std::string>{}; });
    setUpdateCurrentVersionForTesting("");
}
} // namespace

void RegisterUpdatesTests(ImGuiTestEngine *e) {
    // A release newer than the running build surfaces the "available" row and its Open Release Page button.
    IM_REGISTER_TEST(e, "updates", "available_shows_in_about")->TestFunc = [](ImGuiTestContext *ctx) {
        setUpdateCurrentVersionForTesting("v1.0.0");
        setUpdateFetchOverrideForTesting([] {
            return std::optional<std::string>(
                R"({"tag_name":"v2.0.0","html_url":"https://example.test/r/v2.0.0","body":"Notes"})");
        });

        openAbout(ctx);
        ctx->ItemClick("**/###about_check_now");

        // The GET + parse run on a worker; poll until the result drains and the available state renders.
        bool found = false;
        for (int i = 0; i < 240 && !found; ++i) {
            ctx->Yield();
            found = ctx->ItemExists("**/###about_open_release");
        }
        IM_CHECK(found);

        ctx->WindowClose(kAbout);
        ctx->Yield(2);
        resetOverrides();
    };

    // A check that can't reach a feed reports a warning toast on a user-initiated run, and no release link.
    IM_REGISTER_TEST(e, "updates", "failed_check_notifies")->TestFunc = [](ImGuiTestContext *ctx) {
        auto &notif = *getTestState().notifications;
        setUpdateFetchOverrideForTesting([] { return std::optional<std::string>{}; });
        notif.clear();
        const int before = static_cast<int>(notif.log.size());

        getTestState().eventQueue->push(CheckForUpdatesEvent{.userInitiated = true});

        bool logged = false;
        for (int i = 0; i < 240 && !logged; ++i) {
            ctx->Yield();
            logged = static_cast<int>(notif.log.size()) > before; // a Warning persists in the bell log
        }
        IM_CHECK(logged);
        resetOverrides();
    };
}
