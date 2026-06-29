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

    // An available update raises a persistent bell banner that sticks until the user discards it (it is
    // not a transient toast). The bell's discard button clears it.
    IM_REGISTER_TEST(e, "updates", "available_sticks_in_bell")->TestFunc = [](ImGuiTestContext *ctx) {
        auto &notif = *getTestState().notifications;
        notif.clear();
        notif.update.reset();
        setUpdateCurrentVersionForTesting("v1.0.0");
        setUpdateFetchOverrideForTesting([] {
            return std::optional<std::string>(
                R"({"tag_name":"v2.0.0","html_url":"https://example.test/r/v2.0.0","body":"Notes"})");
        });

        loadFixture(ctx);
        getTestState().eventQueue->push(CheckForUpdatesEvent{.userInitiated = false});

        bool banner = false;
        for (int i = 0; i < 240 && !banner; ++i) {
            ctx->Yield();
            banner = notif.update.has_value();
        }
        IM_CHECK(banner);
        IM_CHECK_STR_EQ(notif.update->version.c_str(), "v2.0.0");

        // Still present after further frames — unlike a toast, it does not age out on its own.
        ctx->Yield(10);
        IM_CHECK(notif.update.has_value());

        // Open the bell and discard it; the banner clears.
        ctx->ItemClick("//##AppFooterBar/##bell");
        ctx->Yield(2);
        ctx->ItemClick("**/###bell_update_dismiss");
        ctx->Yield(2);
        IM_CHECK(!notif.update.has_value());

        ctx->KeyPress(ImGuiKey_Escape); // leave no popup open for the next suite
        ctx->Yield();
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
