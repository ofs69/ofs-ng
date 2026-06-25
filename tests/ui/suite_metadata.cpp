#include "Core/Events.h"
#include "Core/ScriptProject.h"
#include "helpers/TestState.h"
#include <imgui_te_context.h>
#include <imgui_te_engine.h>

// Drives the Project Configuration window's Metadata tab (src/UI/ProjectConfigWindow.cpp). The
// window is opened from Edit ▸ Project and reads project.metadata, writing every edit through
// ModifyEvent<FunscriptMetadata> (presets go through ModifyEvent<AppSettings>). basic.ofp
// ships a populated metadata block (see tools/gen_test_fixtures.py), so the editor has real values
// to display, edit, remove and clear.
//
// Items are addressed by their stable ### ids (meta_tab / title / new_tag / add_tag / tag<i> /
// perf<i> / preset_*), not display labels, so icon glyphs and visible text can change freely.

namespace {
constexpr const char *kWin = "//project_config"; // window's ###id; absolute so it ignores SetRef

// loadFixture → open the window → grow it so the whole form is unclipped → switch to Metadata tab.
void openMetadataTab(ImGuiTestContext *ctx) {
    loadFixture(ctx);
    ctx->MenuClick("//##MainMenuBar/###menu_edit/###menu_project_config");
    ctx->Yield(3);
    ctx->SetRef(kWin);
    // The Tags/Performers inputs sit below the default 600px fold; grow the window (within its
    // 1200px max-size constraint) so every field is rendered and locatable without scrolling.
    ctx->WindowResize(kWin, ImVec2(560.f, 1000.f));
    ctx->ItemClick("**/meta_tab");
    ctx->Yield(2);
}
} // namespace

void RegisterMetadataTests(ImGuiTestEngine *e) {
    // The editor opens against the metadata loaded from basic.ofp.
    IM_REGISTER_TEST(e, "metadata", "editor_shows_loaded_fixture_metadata")->TestFunc = [](ImGuiTestContext *ctx) {
        openMetadataTab(ctx);
        const auto &m = getTestState().project->metadata;
        IM_CHECK_STR_EQ(m.title.c_str(), "Fixture Project");
        IM_CHECK_STR_EQ(m.creator.c_str(), "OFS Test Suite");
        IM_CHECK_STR_EQ(m.license.c_str(), "Free");
        IM_CHECK_EQ(m.tags.size(), static_cast<size_t>(3));
        IM_CHECK_EQ(m.performers.size(), static_cast<size_t>(2));
        IM_CHECK(ctx->WindowInfo(kWin).Window != nullptr); // tab rendered without crashing
        ctx->WindowClose(kWin);
    };

    // Typing in the Title field commits a ModifyEvent<FunscriptMetadata> on each edit → project.metadata.title.
    IM_REGISTER_TEST(e, "metadata", "edit_title_updates_project")->TestFunc = [](ImGuiTestContext *ctx) {
        openMetadataTab(ctx);
        ctx->ItemInput("**/title");
        ctx->KeyCharsReplace("Edited Title");
        ctx->Yield(2);
        IM_CHECK_STR_EQ(getTestState().project->metadata.title.c_str(), "Edited Title");
        ctx->WindowClose(kWin);
    };

    // Typing a tag then pressing Enter (ImGuiInputTextFlags_EnterReturnsTrue) appends it via ModifyEvent.
    IM_REGISTER_TEST(e, "metadata", "add_tag_appends")->TestFunc = [](ImGuiTestContext *ctx) {
        openMetadataTab(ctx);
        auto &proj = *getTestState().project;
        const size_t before = proj.metadata.tags.size();
        ctx->ItemInput("**/new_tag");
        ctx->KeyCharsReplaceEnter("delta");
        ctx->Yield(2);
        IM_CHECK_EQ(proj.metadata.tags.size(), before + 1);
        IM_CHECK_STR_EQ(proj.metadata.tags.back().c_str(), "delta");
        ctx->WindowClose(kWin);
    };

    // Clicking a tag chip removes that tag (via a guarded ModifyEvent) and shifts the rest down.
    IM_REGISTER_TEST(e, "metadata", "remove_tag_via_chip")->TestFunc = [](ImGuiTestContext *ctx) {
        openMetadataTab(ctx);
        auto &proj = *getTestState().project;
        IM_CHECK_EQ(proj.metadata.tags.size(), static_cast<size_t>(3)); // alpha, beta, gamma
        ctx->ItemClick("**/tag0");                                      // remove "alpha"
        ctx->Yield(2);
        IM_CHECK_EQ(proj.metadata.tags.size(), static_cast<size_t>(2));
        IM_CHECK_STR_EQ(proj.metadata.tags[0].c_str(), "beta");
        IM_CHECK_STR_EQ(proj.metadata.tags[1].c_str(), "gamma");
        ctx->WindowClose(kWin);
    };

    // Clicking a performer chip removes that performer (via a guarded ModifyEvent).
    IM_REGISTER_TEST(e, "metadata", "remove_performer_via_chip")->TestFunc = [](ImGuiTestContext *ctx) {
        openMetadataTab(ctx);
        auto &proj = *getTestState().project;
        IM_CHECK_EQ(proj.metadata.performers.size(), static_cast<size_t>(2)); // Performer One, Performer Two
        ctx->ItemClick("**/perf0");                                           // remove "Performer One"
        ctx->Yield(2);
        IM_CHECK_EQ(proj.metadata.performers.size(), static_cast<size_t>(1));
        IM_CHECK_STR_EQ(proj.metadata.performers[0].c_str(), "Performer Two");
        ctx->WindowClose(kWin);
    };

    // Save the loaded metadata as a named preset, clear it with Reset, then Load the preset back —
    // exercising both the Save (ModifyEvent<AppSettings> into metadataPresets) and Load
    // (ModifyEvent<FunscriptMetadata>) paths. The just-saved preset is auto-selected, so Load/Delete act on it without
    // touching the combo.
    IM_REGISTER_TEST(e, "metadata", "preset_save_and_load")->TestFunc = [](ImGuiTestContext *ctx) {
        openMetadataTab(ctx);
        auto &proj = *getTestState().project;
        IM_CHECK_STR_EQ(proj.metadata.title.c_str(), "Fixture Project");

        ctx->ItemInput("**/preset_name");
        ctx->KeyCharsReplace("MyPreset");
        ctx->ItemClick("**/preset_save"); // stores the current metadata under "MyPreset"
        ctx->Yield(2);

        ctx->ItemClick("**/reset_meta"); // wipe live metadata so the Load is observable
        ctx->Yield(2);
        IM_CHECK(proj.metadata.title.empty());

        ctx->ItemClick("**/preset_load"); // restore from the saved preset
        ctx->Yield(2);
        IM_CHECK_STR_EQ(proj.metadata.title.c_str(), "Fixture Project");
        IM_CHECK_EQ(proj.metadata.tags.size(), static_cast<size_t>(3));
        IM_CHECK_EQ(proj.metadata.performers.size(), static_cast<size_t>(2));

        ctx->ItemClick("**/preset_delete"); // don't leak the preset into later runs
        ctx->Yield(2);
        ctx->WindowClose(kWin);
    };

    // The Reset button pushes a ModifyEvent<FunscriptMetadata> that clears every fixture-populated field.
    IM_REGISTER_TEST(e, "metadata", "reset_clears_metadata")->TestFunc = [](ImGuiTestContext *ctx) {
        openMetadataTab(ctx);
        auto &proj = *getTestState().project;
        IM_CHECK(!proj.metadata.title.empty()); // fixture supplied one
        ctx->ItemClick("**/reset_meta");
        ctx->Yield(2);
        IM_CHECK(proj.metadata.title.empty());
        IM_CHECK(proj.metadata.tags.empty());
        IM_CHECK(proj.metadata.performers.empty());
        ctx->WindowClose(kWin);
    };

    // Add a custom (non-standard) String field through the add-row, set its value, then delete it.
    // Exercises the key/type/value editors and the index-guarded ModifyEvent<FunscriptMetadata>
    // lambdas. BeginCombo doesn't register a wildcard-searchable label, so the type dropdown and its
    // ###cf_opt<n> options are addressed by direct id under SetRef'd windows; everything else (input
    // text, buttons) registers normally and uses **/ refs. See [[reference_ui_test_engine_refs]].
    IM_REGISTER_TEST(e, "metadata", "custom_field_add_edit_delete")->TestFunc = [](ImGuiTestContext *ctx) {
        openMetadataTab(ctx);
        auto &proj = *getTestState().project;
        const size_t before = proj.metadata.customFields.size();

        // cf_new_key registers and resolves the meta child window; SetRef to it so the combo's direct
        // id ("###cf_new_type") hashes in the right scope even though BeginCombo isn't label-searchable.
        ImGuiTestItemInfo keyInfo = ctx->ItemInfo("**/cf_new_key");
        IM_CHECK(keyInfo.Window != nullptr);
        ctx->SetRef(keyInfo.Window);
        ctx->ItemClick("###cf_new_type"); // open the type dropdown
        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("###cf_opt3"); // String
        ctx->SetRef(kWin);

        ctx->ItemInput("**/cf_new_key");
        ctx->KeyCharsReplace("device");
        ctx->ItemClick("**/cf_add");
        ctx->Yield(2);
        IM_CHECK_EQ(proj.metadata.customFields.size(), before + 1);
        IM_CHECK_STR_EQ(proj.metadata.customFields.back().key.c_str(), "device");
        IM_CHECK(proj.metadata.customFields.back().value.is_string());

        ctx->ItemInput("**/cf_str");
        ctx->KeyCharsReplace("WeVibe");
        ctx->Yield(2);
        IM_CHECK_STR_EQ(proj.metadata.customFields.back().value.get<std::string>().c_str(), "WeVibe");

        ctx->ItemClick("**/cf_del");
        ctx->Yield(2);
        IM_CHECK_EQ(proj.metadata.customFields.size(), before);
        ctx->WindowClose(kWin);
    };

    // An object/array custom field is edited as raw JSON; the typed value must commit as it is edited,
    // not only once Enter is pressed. The raw-JSON box lives on row 2 under PushID(i), so it's reached
    // by direct id within the $$<i> scope. See [[reference_ui_test_engine_refs]].
    IM_REGISTER_TEST(e, "metadata", "custom_field_object_saves_without_enter")->TestFunc = [](ImGuiTestContext *ctx) {
        openMetadataTab(ctx);
        auto &proj = *getTestState().project;
        IM_CHECK_EQ(proj.metadata.customFields.size(), static_cast<size_t>(0));

        // Add an Object custom field "cfg" (value becomes {}).
        ImGuiTestItemInfo keyInfo = ctx->ItemInfo("**/cf_new_key");
        IM_CHECK(keyInfo.Window != nullptr);
        ctx->SetRef(keyInfo.Window);
        ctx->ItemClick("###cf_new_type");
        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("###cf_opt5"); // Object
        ctx->SetRef(kWin);
        ctx->ItemInput("**/cf_new_key");
        ctx->KeyCharsReplace("cfg");
        ctx->ItemClick("**/cf_add");
        ctx->Yield(2);
        IM_CHECK_EQ(proj.metadata.customFields.size(), static_cast<size_t>(1));
        IM_CHECK(proj.metadata.customFields[0].value.is_object());

        // Type valid JSON into the raw box. It must commit as edited, without pressing Enter.
        ctx->SetRef(keyInfo.Window);
        ctx->ItemInput("$$0/###cf_json");
        ctx->KeyCharsReplace("{\"a\": 1}");
        ctx->Yield(3);

        IM_CHECK(proj.metadata.customFields[0].value.is_object());
        IM_CHECK(proj.metadata.customFields[0].value.contains("a"));
        IM_CHECK_EQ(proj.metadata.customFields[0].value.value("a", 0), 1);
        ctx->WindowClose(kWin);
    };

    // Custom fields must survive the preset round-trip: a preset stores the full FunscriptMetadata
    // (custom fields included), so Save → Reset → Load has to restore an object custom field verbatim.
    // This also exercises the raw-JSON box reseeding from a freshly loaded value (the jsonEdits
    // buffer keyed by the field key is cleared on Reset) without an Enter or manual edit.
    IM_REGISTER_TEST(e, "metadata", "preset_roundtrips_custom_object_field")->TestFunc = [](ImGuiTestContext *ctx) {
        openMetadataTab(ctx);
        auto &proj = *getTestState().project;
        IM_CHECK_EQ(proj.metadata.customFields.size(), static_cast<size_t>(0));

        // Add an Object custom field "cfg" and give it a value.
        ImGuiTestItemInfo keyInfo = ctx->ItemInfo("**/cf_new_key");
        IM_CHECK(keyInfo.Window != nullptr);
        ctx->SetRef(keyInfo.Window);
        ctx->ItemClick("###cf_new_type");
        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("###cf_opt5"); // Object
        ctx->SetRef(kWin);
        ctx->ItemInput("**/cf_new_key");
        ctx->KeyCharsReplace("cfg");
        ctx->ItemClick("**/cf_add");
        ctx->Yield(2);
        ctx->SetRef(keyInfo.Window);
        ctx->ItemInput("$$0/###cf_json");
        ctx->KeyCharsReplace("{\"a\": 1}");
        ctx->Yield(3);
        ctx->SetRef(kWin);
        IM_CHECK_EQ(proj.metadata.customFields.size(), static_cast<size_t>(1));
        IM_CHECK_EQ(proj.metadata.customFields[0].value.value("a", 0), 1);

        // Save as a preset, then Reset to wipe the live metadata (custom field gone).
        ctx->ItemInput("**/preset_name");
        ctx->KeyCharsReplace("CfgPreset");
        ctx->ItemClick("**/preset_save");
        ctx->Yield(2);
        ctx->ItemClick("**/reset_meta");
        ctx->Yield(2);
        IM_CHECK(proj.metadata.customFields.empty());

        // Load the preset back — the object custom field must return with its value intact.
        ctx->ItemClick("**/preset_load");
        ctx->Yield(3);
        IM_CHECK_EQ(proj.metadata.customFields.size(), static_cast<size_t>(1));
        IM_CHECK_STR_EQ(proj.metadata.customFields[0].key.c_str(), "cfg");
        IM_CHECK(proj.metadata.customFields[0].value.is_object());
        IM_CHECK_EQ(proj.metadata.customFields[0].value.value("a", 0), 1);

        ctx->ItemClick("**/preset_delete"); // don't leak the preset into later runs
        ctx->Yield(2);
        ctx->WindowClose(kWin);
    };
}
