#include "Core/Events.h"
#include "helpers/TestState.h"
#include <imgui.h>
#include <imgui_te_context.h>
#include <imgui_te_engine.h>

// ImGui names popup windows "##Popup_XXXXXXXX" (hash of owner + string id), so
// ctx->WindowInfo("##cmdpalette") will never find them. IsPopupOpen with AnyPopupId |
// AnyPopupLevel checks OpenPopupStack.Size > 0 without touching CurrentWindow and is safe
// to call between frames.
static bool anyPopupOpen() {
    return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

// Phase 2 UI tests for the command palette.
// open (shortcut + click) → type → Enter fires the right event.
void RegisterCommandPaletteTests(ImGuiTestEngine *e) {
    // Clicking the search box in the title bar opens the palette popup.
    IM_REGISTER_TEST(e, "command_palette", "opens_via_click")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        ctx->ItemClick("//##AppTitleBar/##searchbox");
        ctx->Yield(2);
        IM_CHECK(anyPopupOpen());
    };

    // Pushing OpenCommandPaletteEvent (the Ctrl+Shift+P shortcut path) opens the palette.
    IM_REGISTER_TEST(e, "command_palette", "opens_via_event")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        getTestState().eventQueue->push(ofs::OpenCommandPaletteEvent{});
        ctx->Yield(3);
        IM_CHECK(anyPopupOpen());
    };

    // Type a query and press Enter — the matched command must fire.
    // Uses "player.goto-start" (Go to Start) which has an observable side-effect: cursorPos → 0.
    IM_REGISTER_TEST(e, "command_palette", "enter_invokes_matched_command")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        // Move cursor away from 0 so the seek-to-start is observable.
        eq.push(ofs::SeekEvent{5.0});
        ctx->Yield(2);
        IM_CHECK_GT(proj.playback.cursorPos, 0.0);

        // Open palette; the InputText is auto-focused via SetKeyboardFocusHere on the first frame.
        eq.push(ofs::OpenCommandPaletteEvent{});
        ctx->Yield(3);
        IM_CHECK(anyPopupOpen());

        // Type the command's localized title into the auto-focused input; it uniquely matches its own
        // palette row. Resolved at runtime so the query tracks the active language (the palette searches
        // the localized title, so a hardcoded English string would miss under ui-smoke-loc).
        // KeyCharsReplace yields internally after each key, so the text is committed before returning.
        ctx->KeyCharsReplace(localizedCommandTitle("player.goto-start").c_str());

        // Enter invokes the top-scored match and closes the popup.
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(3);

        // The cursor should be at 0 (SeekEvent{0.0} pushed by player.goto-start).
        IM_CHECK_EQ(proj.playback.cursorPos, 0.0);
    };

    // Phase 5: a dynamic navigation command (not in the static registry) appears in the palette and
    // invokes. Create a scratch axis (S0) so a "Select Axis: S0" command is guaranteed present
    // regardless of fixture contents; invoking it pushes AxisSelectedEvent{S0}, observable as
    // state.activeAxis.
    IM_REGISTER_TEST(e, "command_palette", "navigation_command_invokes")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        // Create S0 (becomes present and active), then move the active axis away so selecting S0 via
        // the palette is observable.
        eq.push(ofs::AddScratchAxisEvent{});
        ctx->Yield(2);
        eq.push(ofs::AxisSelectedEvent{ofs::StandardAxis::L0});
        ctx->Yield(2);
        IM_CHECK(proj.state.activeAxis == ofs::StandardAxis::L0);

        eq.push(ofs::OpenCommandPaletteEvent{});
        ctx->Yield(3);
        IM_CHECK(anyPopupOpen());

        // "<Select Axis> S0" uniquely matches the generated Select-Axis command for S0. The group is
        // localized in the palette haystack (no English fallback), so resolve it at runtime; the
        // scratch-axis code "S0" carries no descriptor and is language-independent.
        const std::string query = localizedGroupName("Select Axis") + " S0";
        ctx->KeyCharsReplace(query.c_str());
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(3);

        IM_CHECK(proj.state.activeAxis == ofs::StandardAxis::S0);
    };

    // Phase 5: the static "Add Scratch Axis" command and the dynamic "Delete Scratch Axis: S0" command
    // round-trip through the palette — adding then removing a scratch axis.
    IM_REGISTER_TEST(e, "command_palette", "add_then_delete_scratch_axis")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        const auto s0 = static_cast<size_t>(ofs::StandardAxis::S0);
        IM_CHECK(!proj.axes[s0].showInStrip); // fixture ships with no scratch axes

        // Add Scratch Axis -> S0 becomes present. Native command, so its title is translated under
        // ui-smoke-loc; resolve it at runtime instead of hardcoding English.
        eq.push(ofs::OpenCommandPaletteEvent{});
        ctx->Yield(3);
        ctx->KeyCharsReplace(localizedCommandTitle("axis.add-scratch").c_str());
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(3);
        IM_CHECK(proj.axes[s0].showInStrip);

        // Delete Scratch Axis: S0 -> S0 gone again. Dynamic command; its group is localized in the
        // palette haystack, so build the query from the active-language group name + the (language-
        // independent) scratch-axis code.
        eq.push(ofs::OpenCommandPaletteEvent{});
        ctx->Yield(3);
        const std::string delQuery = localizedGroupName("Delete Scratch Axis") + " S0";
        ctx->KeyCharsReplace(delQuery.c_str());
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(3);
        IM_CHECK(!proj.axes[s0].showInStrip);
    };
}
