#include "Core/Events.h"
#include "Core/ProjectLifecycleEvents.h"
#include "Core/ScriptProject.h"
#include "helpers/TestState.h"
#include <imgui.h>
#include <imgui_internal.h> // ImGuiItemFlags_Disabled
#include <imgui_te_context.h>
#include <imgui_te_engine.h>

using namespace ofs;

namespace {
constexpr const char *kWelcome = "Welcome###welcome_screen";
constexpr const char *kEditorWin = "Timeline###timeline"; // a representative editor-only window
constexpr const char *kFooter = "//##AppFooterBar";
constexpr const char *kMenuBar = "//##MainMenuBar";
constexpr const char *kPrefs = "//Preferences###preferences";

bool windowVisible(ImGuiTestContext *ctx, const char *ref) {
    return ctx->WindowInfo(ref, ImGuiTestOpFlags_NoError).Window != nullptr;
}

bool anyPopupOpen() {
    return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

// Open the command palette, focus its input explicitly (the full-viewport welcome window makes
// auto-focus less reliable than in the editor), type the query, and press Enter to invoke the top hit.
void paletteEnter(ImGuiTestContext *ctx, const char *query) {
    getTestState().eventQueue->push(ofs::OpenCommandPaletteEvent{});
    ctx->Yield(3);
    IM_CHECK(anyPopupOpen());
    ctx->SetRef("//$FOCUSED"); // the palette popup
    ctx->ItemClick("**/##q");  // focus the search InputText
    ctx->KeyCharsReplace(query);
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->SetRef(""); // reset so later relative refs aren't scoped to the (closed) popup
}

// Mirror of ProjectManager::hasActiveProject() — the test can't reach the private method.
bool noProjectOpen(const ScriptProject &p) {
    if (!p.state.filePath.empty() || !p.state.mediaPath.empty())
        return false;
    for (const auto &a : p.axes)
        if (a.showInStrip)
            return false;
    return true;
}

// Close any open project and settle on a clean no-project state. clearDirtyFlags() first so the close
// runs synchronously (guardUnsaved sees nothing dirty) instead of raising the save prompt. Mirrors the
// file-local helper in suite_noproject.cpp — the two suites keep independent copies on purpose.
void closeToNoProject(ImGuiTestContext *ctx) {
    getTestState().project->clearDirtyFlags();
    getTestState().eventQueue->push(CloseProjectRequestEvent{});
    ctx->Yield(3);
}
} // namespace

// The welcome screen is the no-project body: it replaces the editor dockspace whenever
// hasActiveProject() is false. These tests pin the single top-level branch in OfsApp::onImGuiRender.
void RegisterWelcomeTests(ImGuiTestEngine *e) {
    // With no project, the welcome window is up and the editor windows are not submitted at all.
    IM_REGISTER_TEST(e, "welcome", "visible_without_project")->TestFunc = [](ImGuiTestContext *ctx) {
        closeToNoProject(ctx);
        ctx->Yield(3); // let the unsubmitted editor windows drop out of the test engine's view
        IM_CHECK(windowVisible(ctx, kWelcome));
        IM_CHECK(!windowVisible(ctx, kEditorWin));
    };

    // Opening a project flips the branch: welcome gone, editor up.
    IM_REGISTER_TEST(e, "welcome", "hidden_with_project")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        ctx->Yield(3);
        IM_CHECK(windowVisible(ctx, kEditorWin));
        IM_CHECK(!windowVisible(ctx, kWelcome));
    };

    // Round-trip: open shows the editor, close returns to the welcome screen.
    IM_REGISTER_TEST(e, "welcome", "close_returns_to_welcome")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        ctx->Yield(3);
        IM_CHECK(windowVisible(ctx, kEditorWin));

        closeToNoProject(ctx);
        ctx->Yield(3);
        IM_CHECK(windowVisible(ctx, kWelcome));
        IM_CHECK(!windowVisible(ctx, kEditorWin));
    };

    // Always-on chrome: the footer and main menu bar render on both screens.
    IM_REGISTER_TEST(e, "welcome", "chrome_renders_on_both_screens")->TestFunc = [](ImGuiTestContext *ctx) {
        closeToNoProject(ctx);
        ctx->Yield(3);
        IM_CHECK(windowVisible(ctx, kFooter));
        IM_CHECK(windowVisible(ctx, kMenuBar));

        loadFixture(ctx);
        ctx->Yield(3);
        IM_CHECK(windowVisible(ctx, kFooter));
        IM_CHECK(windowVisible(ctx, kMenuBar));
    };

    // Project-scoped menu actions are gated by screen. File ▸ Close Project (a representative project
    // action in the always-enabled File menu) is disabled on welcome and enabled with a project. Open
    // the File menu so the item is submitted, then read its disabled flag.
    IM_REGISTER_TEST(e, "welcome", "menu_gates_project_sections")->TestFunc = [](ImGuiTestContext *ctx) {
        auto closeDisabled = [&] {
            ctx->ItemClick("//##MainMenuBar/##MenuBar/###menu_file"); // open the File menu
            ctx->Yield(2);
            // The opened first-level menu is the "##Menu_00" popup; the item carries a stable ###id.
            const bool disabled =
                (ctx->ItemInfo("//Menu_00/###menu_close_project").ItemFlags & ImGuiItemFlags_Disabled) != 0;
            ctx->KeyPress(ImGuiKey_Escape); // close the menu
            ctx->Yield(2);
            return disabled;
        };

        closeToNoProject(ctx);
        ctx->Yield(3);
        IM_CHECK(closeDisabled()); // no project → Close Project disabled

        loadFixture(ctx);
        ctx->Yield(3);
        IM_CHECK(!closeDisabled()); // project → Close Project enabled
    };

    // The few global commands still work on welcome: typing "preferences" and pressing Enter fires
    // "Open Preferences" and its window — now rendered on both screens — appears. (This also proves the
    // palette's type+Enter path is live on welcome, which the negative test below relies on.)
    IM_REGISTER_TEST(e, "welcome", "palette_global_command_works")->TestFunc = [](ImGuiTestContext *ctx) {
        closeToNoProject(ctx);
        ctx->Yield(3);
        if (windowVisible(ctx, kPrefs)) { // a prior test may have left it open
            ctx->WindowClose(kPrefs);
            ctx->Yield(2);
        }

        // Native command title is translated under ui-smoke-loc; resolve the active-language title so
        // the palette search matches in any language.
        paletteEnter(ctx, localizedCommandTitle("core.open-preferences").c_str());
        ctx->Yield(3);

        IM_CHECK(windowVisible(ctx, kPrefs));
        ctx->WindowClose(kPrefs); // restore for later suites
        ctx->Yield(2);
    };

    // On welcome the palette must not offer project commands. "Add Scratch Axis" is filtered out, so
    // typing it and pressing Enter matches nothing — no axis is created, no project appears. Meaningful
    // because palette_global_command_works proves type+Enter does fire commands on this screen.
    IM_REGISTER_TEST(e, "welcome", "palette_hides_project_commands")->TestFunc = [](ImGuiTestContext *ctx) {
        closeToNoProject(ctx);
        ctx->Yield(3);

        paletteEnter(ctx, "add scratch axis");
        ctx->Yield(3);

        IM_CHECK(noProjectOpen(*getTestState().project)); // command was filtered → nothing fired
        if (anyPopupOpen()) {
            ctx->KeyPress(ImGuiKey_Escape); // close a palette left open by the no-op Enter
            ctx->Yield(2);
        }
    };

    // The dedicated "Create Empty Project" button makes a media-less project straight away (no picker,
    // no confirm). The new project must never have a zero-length timeline — an empty project always gets
    // the default dummy duration so there is a span to place actions on.
    IM_REGISTER_TEST(e, "welcome", "create_empty_project_button")->TestFunc = [](ImGuiTestContext *ctx) {
        closeToNoProject(ctx);
        ctx->PopupCloseAll(); // a prior palette test may leave a results popup over the welcome buttons
        ctx->Yield(3);
        IM_CHECK(windowVisible(ctx, kWelcome));

        ctx->SetRef(kWelcome);
        ctx->ItemClick("###welcome_create_empty");
        ctx->SetRef("");
        ctx->Yield(5); // guardUnsaved (nothing dirty) → doClose → initNewProject settles over a few frames

        auto &proj = *getTestState().project;
        IM_CHECK(!noProjectOpen(proj));           // a project is now active
        IM_CHECK(windowVisible(ctx, kEditorWin)); // editor replaced the welcome screen
        IM_CHECK(!windowVisible(ctx, kWelcome));
        IM_CHECK_GT(proj.state.dummyDuration, 0.0); // never a zero-length timeline
        IM_CHECK_EQ(proj.state.dummyDuration, ofs::kDefaultDummyDuration);
    };

    // A file dropped onto the welcome screen routes through OpenDroppedFileEvent → ProjectManager's
    // extension dispatch (an .ofp opens that project). This drives the event the SDL drop handler pushes;
    // the SDL plumbing itself isn't simulable in the test engine.
    IM_REGISTER_TEST(e, "welcome", "dropped_file_opens_project")->TestFunc = [](ImGuiTestContext *ctx) {
        closeToNoProject(ctx);
        ctx->Yield(3);
        IM_CHECK(windowVisible(ctx, kWelcome));

        const std::filesystem::path dir(OFS_TESTS_DIR);
        getTestState().eventQueue->push(ofs::OpenDroppedFileEvent{(dir / "fixtures" / "basic.ofp").string()});
        // The .ofp now decodes on a worker; yield until the load lands rather than a fixed frame count,
        // then a few more frames so the UI swaps the welcome screen out for the editor.
        for (int i = 0; i < 240 && noProjectOpen(*getTestState().project); ++i)
            ctx->Yield();
        ctx->Yield(3);

        IM_CHECK(!noProjectOpen(*getTestState().project));
        IM_CHECK(windowVisible(ctx, kEditorWin));
        IM_CHECK(!windowVisible(ctx, kWelcome));
    };
}
