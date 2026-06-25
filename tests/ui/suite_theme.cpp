#include "UI/Theme.h"
#include "helpers/TestState.h"
#include <cmath>
#include <cstring>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_te_context.h>
#include <imgui_te_engine.h>
#include <string>

// Drives the Preferences ▸ Theme tab (src/UI/ConfigurationWindow.cpp::renderThemeTab): loading a
// theme via the selector, Save As, the global "Reset to Defaults" button, per-color reset buttons,
// and editing a color through its ColorEdit swatch. Items are addressed by stable ### ids
// (theme_tab / theme_select / theme_reset_all / theme_name / theme_saveas / col<slot> /
// colreset<slot>), so icon glyphs and visible labels can change without breaking the tests.
//
// The UI-test pref dir is a temp directory wiped per run (tests/test_main_ui.cpp), so Save As /
// save() write throwaway theme files that never touch the real app data dir.

namespace {
// Absolute ("//") window ref: it's passed to WindowResize/WindowClose *after* SetRef(kWin), and a
// relative ref would be re-resolved against the current ref (doubling the path → window not found).
constexpr const char *kWin = "//Preferences###preferences";

// 8-bit quantised channel equality — colors serialise to #RRGGBBAA, so compare at that resolution.
bool colorEq(const ImColor &a, const ImColor &b) {
    auto q = [](float v) { return static_cast<int>(v * 255.f + 0.5f); };
    return q(a.Value.x) == q(b.Value.x) && q(a.Value.y) == q(b.Value.y) && q(a.Value.z) == q(b.Value.z) &&
           q(a.Value.w) == q(b.Value.w);
}

// The swatch is a ColorEdit4 (id col<slot>) whose clickable child is "##ColorButton"; the reset is a
// plain Button (id colreset<slot>).
std::string colSwatchRef(int slot) {
    return "##tl/col" + std::to_string(slot) + "/##ColorButton";
}
std::string colResetRef(int slot) {
    return "##tl/colreset" + std::to_string(slot);
}

// Point SetRef at the Theme-tab body's scrolling child window. WindowInfo() can't resolve it: it
// seeds the child id with the parent window id, but the BeginChild("##theme_scroll") runs inside the
// Theme tab item, so the real id is seeded with the tab on the stack. The child's *name* is
// "<parent>/##theme_scroll_<hash>", so locate it by its structural "##theme_scroll" id. Match on that
// id alone — never on the "<parent>" portion, which is the window's *translated* title and shifts per
// language (e.g. "設定###preferences" under jp).
void setRefThemeChild(ImGuiTestContext *ctx) {
    ImGuiContext &g = *ImGui::GetCurrentContext();
    ImGuiWindow *child = nullptr;
    for (ImGuiWindow *w : g.Windows)
        if (std::strstr(w->Name, "##theme_scroll") != nullptr) {
            child = w;
            break;
        }
    IM_CHECK_SILENT(child != nullptr);
    ctx->SetRef(child);
}

// Select a theme from the ###theme_select dropdown by its (untranslated) name. Each item carries a
// stable "###theme_opt_<name>" id so the visible "(shipped)" suffix can be localized without moving
// the target. With SetRef pointed at the child window the combo's ref ("theme_select") has no '/',
// so ComboClick's "combo/item" split works cleanly.
void selectTheme(ImGuiTestContext *ctx, const char *themeName) {
    setRefThemeChild(ctx);
    ctx->ComboClick((std::string("theme_select/theme_opt_") + themeName).c_str());
    ctx->Yield(2);
}

// loadFixture → open Preferences from Edit menu → grow it so the color tables are unclipped →
// switch to the Theme tab → reset to the shipped Dark theme for a deterministic starting point.
void openThemeTab(ImGuiTestContext *ctx) {
    loadFixture(ctx);
    // The Edit ▸ Preferences menu item is a toggle, and a prior suite may have left the window open
    // (e.g. on the Simulator tab). Only click it when the window isn't already open, so we never
    // accidentally close it.
    if (ctx->WindowInfo(kWin, ImGuiTestOpFlags_NoError).Window == nullptr) {
        ctx->MenuClick("//##MainMenuBar/###menu_edit/###menu_preferences");
        ctx->Yield(2);
    }
    ctx->WindowResize(kWin, ImVec2(760.f, 1000.f));
    ctx->SetRef(kWin);
    ctx->ItemClick("**/theme_tab"); // the tab bar lives in the parent window
    ctx->Yield(2);
    setRefThemeChild(ctx);
    // Known-good baseline: shipped Dark (no-op if already active).
    selectTheme(ctx, "Dark");
    ctx->Yield(2);
}
} // namespace

void RegisterThemeTests(ImGuiTestEngine *e) {
    // Selecting another theme in the dropdown loads + applies it (loading path).
    IM_REGISTER_TEST(e, "theme", "select_loads_theme")->TestFunc = [](ImGuiTestContext *ctx) {
        openThemeTab(ctx);
        IM_CHECK(ofs::theme::getActive().isDark); // baseline

        selectTheme(ctx, "Light");
        ctx->Yield(2);
        IM_CHECK_STR_EQ(ofs::theme::getActive().name.c_str(), "Light");
        IM_CHECK(!ofs::theme::getActive().isDark);

        selectTheme(ctx, "Dark"); // restore for later suites
        ctx->Yield(2);
        ctx->WindowClose(kWin);
    };

    // Typing a name + Save As writes a user theme that joins the list and becomes active (saving path).
    IM_REGISTER_TEST(e, "theme", "save_as_creates_user_theme")->TestFunc = [](ImGuiTestContext *ctx) {
        openThemeTab(ctx);

        ctx->ItemInput("theme_name");
        ctx->KeyCharsReplace("My Saved Theme");
        ctx->ItemClick("theme_saveas");
        ctx->Yield(2);

        IM_CHECK_STR_EQ(ofs::theme::getActive().name.c_str(), "My Saved Theme");
        ofs::theme::Theme reloaded;
        IM_CHECK(ofs::theme::load("My Saved Theme", &reloaded)); // persisted to disk
        bool listed = false;
        for (const auto &info : ofs::theme::list())
            listed |= (info.name == "My Saved Theme" && !info.shipped);
        IM_CHECK(listed);

        selectTheme(ctx, "Dark");
        ctx->Yield(2);
        ctx->WindowClose(kWin);
    };

    // The global "Reset to Defaults" button reverts every edited color to the scheme default.
    IM_REGISTER_TEST(e, "theme", "reset_to_defaults_button")->TestFunc = [](ImGuiTestContext *ctx) {
        openThemeTab(ctx);
        const ofs::theme::Theme &def = ofs::theme::defaultDark();

        // Perturb a couple of slots in the live theme (the editor reads these each frame).
        ofs::theme::getActive().colors[AppCol_OverlayLineMajor] = ImColor(1.f, 0.f, 1.f, 1.f);
        ofs::theme::getActive().colors[AppCol_TempoMeasureLine] = ImColor(0.f, 1.f, 0.f, 1.f);
        ctx->Yield();
        IM_CHECK(
            !colorEq(ofs::theme::getActive().colors[AppCol_OverlayLineMajor], def.colors[AppCol_OverlayLineMajor]));

        ctx->ItemClick("theme_reset_all");
        ctx->Yield(2);
        IM_CHECK(colorEq(ofs::theme::getActive().colors[AppCol_OverlayLineMajor], def.colors[AppCol_OverlayLineMajor]));
        IM_CHECK(colorEq(ofs::theme::getActive().colors[AppCol_TempoMeasureLine], def.colors[AppCol_TempoMeasureLine]));

        ctx->WindowClose(kWin);
    };

    // A single color's reset button reverts only that slot, leaving other edits intact.
    IM_REGISTER_TEST(e, "theme", "reset_individual_color")->TestFunc = [](ImGuiTestContext *ctx) {
        openThemeTab(ctx);
        const ofs::theme::Theme &def = ofs::theme::defaultDark();
        constexpr int slotA = AppCol_OverlayLineMajor;
        constexpr int slotB = AppCol_TempoMeasureLine;

        ofs::theme::getActive().colors[slotA] = ImColor(1.f, 0.f, 1.f, 1.f);
        ofs::theme::getActive().colors[slotB] = ImColor(0.f, 1.f, 0.f, 1.f);
        ctx->Yield();

        ctx->ItemClick(colResetRef(slotA).c_str());
        ctx->Yield(2);

        IM_CHECK(colorEq(ofs::theme::getActive().colors[slotA], def.colors[slotA]));           // reverted
        IM_CHECK(colorEq(ofs::theme::getActive().colors[slotB], ImColor(0.f, 1.f, 0.f, 1.f))); // untouched

        // Clean the leftover edit so later suites see a pristine theme.
        ctx->ItemClick(colResetRef(slotB).c_str());
        ctx->Yield(2);
        ctx->WindowClose(kWin);
    };

    // Editing a color through its ColorEdit swatch updates the active theme and persists to disk.
    IM_REGISTER_TEST(e, "theme", "edit_color_via_swatch_persists")->TestFunc = [](ImGuiTestContext *ctx) {
        openThemeTab(ctx);
        constexpr int slot = AppCol_OverlayLineMajor;

        // Save As first so the active theme is a (writable) user theme — save() is a no-op on shipped.
        ctx->ItemInput("theme_name");
        ctx->KeyCharsReplace("Edit Test Theme");
        ctx->ItemClick("theme_saveas");
        ctx->Yield(2);
        IM_CHECK_STR_EQ(ofs::theme::getActive().name.c_str(), "Edit Test Theme");

        // Seed a known starting color (pure red) so the SV-corner drag below is guaranteed to change it.
        const ImColor red(1.f, 0.f, 0.f, 1.f);
        ofs::theme::getActive().colors[slot] = red;
        ctx->Yield();

        // Open the swatch picker and drag the SV box to its top-left corner (saturation 0 → white).
        setRefThemeChild(ctx);
        ctx->ItemClick(colSwatchRef(slot).c_str());
        ctx->Yield();
        ctx->SetRef("//$FOCUSED");
        ctx->MouseMove("##picker/sv", ImGuiTestOpFlags_MoveToEdgeU | ImGuiTestOpFlags_MoveToEdgeL);
        ctx->MouseDown(0);
        ctx->MouseUp(0);
        ctx->PopupCloseAll();
        ctx->Yield(2);

        const ImColor edited = ofs::theme::getActive().colors[slot];
        IM_CHECK(!colorEq(edited, red)); // the widget actually changed the value

        ofs::theme::Theme reloaded;
        IM_CHECK(ofs::theme::load("Edit Test Theme", &reloaded));
        IM_CHECK(colorEq(reloaded.colors[slot], edited)); // applyAndSave persisted the edit

        ctx->SetRef(kWin);
        selectTheme(ctx, "Dark"); // restore for later suites
        ctx->Yield(2);
        ctx->WindowClose(kWin);
    };

    // The "BG Axes" opacity slider in the Axis Colors section writes the theme float — it's a theme
    // value saved with the theme, not an AppSettings field.
    IM_REGISTER_TEST(e, "theme", "bg_axes_opacity_slider")->TestFunc = [](ImGuiTestContext *ctx) {
        openThemeTab(ctx);
        const float before = ofs::theme::getActive().backgroundAxisOpacity;

        setRefThemeChild(ctx);
        ctx->ItemInputValue("**/##bg_axes", 0.7f); // CTRL+click → type
        ctx->Yield(2);
        IM_CHECK_LT(std::abs(ofs::theme::getActive().backgroundAxisOpacity - 0.7f), 0.02f);

        ctx->ItemInputValue("**/##bg_axes", before); // restore for later suites
        ctx->Yield(2);
        ctx->WindowClose(kWin);
    };
}
