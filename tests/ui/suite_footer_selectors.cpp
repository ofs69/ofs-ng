#include "Core/ScriptProject.h"
#include "Services/EditModeRegistry.h"      // kNativeEditModeId
#include "Services/NavigatorRegistry.h"     // kFollowOverlayNavigatorId
#include "Services/SelectionModeRegistry.h" // kNativeSelectionModeId
#include "helpers/TestState.h"
#include <imgui.h>
#include <imgui_te_context.h>
#include <imgui_te_engine.h>

using namespace ofs;

// UI tests for the footer interaction extension-point selectors (Edit mode + Step navigator). They
// drive the real app: a project is loaded so the footer renders its full zone set, then the two
// dropdowns are exercised by their stable, language-independent ###ids. Phase 3 ships only the native
// entries, so these assert the dropdowns open, list the native option (keyed by its registry id, not
// its translated label — so they hold under ui-smoke-loc), and that picking it keeps the project field
// native. The plugin-driven switch path is covered once a second entry exists (later phase).
void RegisterFooterSelectorTests(ImGuiTestEngine *e) {
    IM_REGISTER_TEST(e, "footer", "edit_mode_dropdown_lists_native")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        ctx->Yield(2);

        ctx->ItemClick("//##AppFooterBar/###ft_editmode");
        ctx->Yield(2);
        IM_CHECK(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));

        // The native option is keyed by its stable registry id; re-selecting it is a no-op.
        ctx->ItemClick("**/###native");
        ctx->Yield(2);
        IM_CHECK_STR_EQ(getTestState().project->activeEditMode.c_str(), kNativeEditModeId);
    };

    IM_REGISTER_TEST(e, "footer", "step_dropdown_lists_follow_overlay")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        ctx->Yield(2);

        ctx->ItemClick("//##AppFooterBar/###ft_step");
        ctx->Yield(2);
        IM_CHECK(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));

        ctx->ItemClick("**/###follow-overlay");
        ctx->Yield(2);
        IM_CHECK_STR_EQ(getTestState().project->activeNavigator.c_str(), kFollowOverlayNavigatorId);
    };

    IM_REGISTER_TEST(e, "footer", "select_dropdown_lists_native")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        ctx->Yield(2);

        ctx->ItemClick("//##AppFooterBar/###ft_select");
        ctx->Yield(2);
        IM_CHECK(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));

        ctx->ItemClick("**/###native");
        ctx->Yield(2);
        IM_CHECK_STR_EQ(getTestState().project->activeSelectionMode.c_str(), kNativeSelectionModeId);
    };
}
