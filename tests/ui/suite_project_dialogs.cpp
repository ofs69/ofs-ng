#include "Core/Events.h"
#include "Core/GraphPresetEvents.h"
#include "Core/ProjectLifecycleEvents.h"
#include "Core/ScriptProject.h"
#include "Core/StandardAxis.h"
#include "UI/ModalManager.h"
#include "UI/Notifications.h"
#include "Util/PathUtil.h"
#include "helpers/TestState.h"
#include <atomic>
#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h> // ImGuiItemFlags_Disabled
#include <imgui_te_context.h>
#include <imgui_te_engine.h>
#include <memory>

using namespace ofs;

// Drives ProjectManager's coroutine dialog flows (open/new, save-as, import, export, relocate,
// graph save/load) end to end against the real app. Each flow does `co_await FileDialog` (and some
// `co_await Confirm`); the native picker is replaced by setNativeDialogOverrideForTesting so no real
// OS dialog opens. These branches are unreachable from the unit-test ProjectManager (no ModalManager
// to resume the await) — only the live app + ModalManager + test seam can exercise them.
namespace {
bool anyModalOpen() {
    return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

// The native-dialog override runs on a JobSystem worker. A flow's effect on ScriptProject is often
// observable one frame *before* the worker has actually consumed the override (e.g. an axis the
// fixture already made present), so resetting the override on the effect alone can clear it while a
// dialog is still in flight — which would then resolve to "cancel" (empty) and break the assertion.
// This stub returns a canned path AND records, atomically, how many times the worker invoked it, so
// tests can wait on the call itself before asserting and tearing the override down.
struct DialogStub {
    std::shared_ptr<std::atomic<int>> calls = std::make_shared<std::atomic<int>>(0);
    [[nodiscard]] int count() const { return calls->load(std::memory_order_acquire); }
};

DialogStub installDialog(std::string result) {
    DialogStub s;
    auto calls = s.calls;
    setNativeDialogOverrideForTesting([result = std::move(result), calls](const FileDialogSpec &, const std::string &) {
        calls->fetch_add(1, std::memory_order_release);
        return result;
    });
    return s;
}

// Yield up to `maxFrames` frames, returning early as soon as `pred` holds.
template <class Pred> bool yieldUntil(ImGuiTestContext *ctx, Pred pred, int maxFrames = 240) {
    for (int i = 0; i < maxFrames; ++i) {
        if (pred())
            return true;
        ctx->Yield();
    }
    return pred();
}

std::filesystem::path tempPath(const char *name) {
    return std::filesystem::temp_directory_path() / name;
}

std::filesystem::path fixture(const char *name) {
    return std::filesystem::path(OFS_TESTS_DIR) / "fixtures" / name;
}

size_t presentAxisCount(const ScriptProject &p) {
    size_t n = 0;
    for (const auto &a : p.axes)
        if (a.showInStrip)
            ++n;
    return n;
}
} // namespace

void RegisterProjectDialogsTests(ImGuiTestEngine *e) {
    // Save As routes through saveFlow's FileDialog branch: the override hands back a temp .ofp, the
    // flow writes it (async) and adopts it as the project's filePath.
    IM_REGISTER_TEST(e, "project_dialogs", "save_as_writes_ofp_via_native_dialog")->TestFunc =
        [](ImGuiTestContext *ctx) {
            loadFixture(ctx);
            auto &eq = *getTestState().eventQueue;
            auto &proj = *getTestState().project;

            const auto out = tempPath("ofs_ui_save_as.ofp");
            std::filesystem::remove(out);
            auto dlg = installDialog(ofs::util::toUtf8(out));

            eq.push(SaveProjectEvent{true}); // saveAs => picker
            // filePath is adopted by finalizePendingWrite() in update(), which lags the worker's write;
            // gate on it (not on file existence) so the assert below can't race the finalize.
            IM_CHECK(yieldUntil(ctx, [&] { return proj.state.filePath == ofs::util::toUtf8(out); }));
            IM_CHECK(std::filesystem::exists(out));
            IM_CHECK(dlg.count() == 1);

            setNativeDialogOverrideForTesting(nullptr);
            std::filesystem::remove(out);
        };

    // Open-or-New with an .ofp selected closes the current project and loads the chosen one. A
    // sentinel scratch axis (absent in the fixture) proves the reload replaced project state.
    IM_REGISTER_TEST(e, "project_dialogs", "open_or_new_loads_ofp")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &eq = *getTestState().eventQueue;
        auto &proj = *getTestState().project;
        proj.clearDirtyFlags(); // clean => guardUnsaved proceeds straight to the picker
        proj.axes[static_cast<size_t>(StandardAxis::S9)].showInStrip = true; // sentinel: must be gone after reload

        auto dlg = installDialog(ofs::util::toUtf8(fixture("basic.ofp")));
        eq.push(OpenOrNewProjectRequestEvent{});
        // Wait for the dialog worker first (proves doClose ran and the override was consumed), THEN for
        // the reload to bring L0 back. Checking L0 first would catch it still present from loadFixture,
        // before drain() runs doClose. The sentinel must be gone once the reload completes.
        IM_CHECK(yieldUntil(ctx, [&] { return dlg.count() == 1; }));
        IM_CHECK(yieldUntil(ctx, [&] { return proj.axes[static_cast<size_t>(StandardAxis::L0)].showInStrip; }));
        IM_CHECK(!proj.axes[static_cast<size_t>(StandardAxis::S9)].showInStrip);

        setNativeDialogOverrideForTesting(nullptr);
    };

    // Empty selection => the flow offers an empty project. "Yes" creates one with the default axes.
    IM_REGISTER_TEST(e, "project_dialogs", "open_or_new_empty_confirm_creates_project")->TestFunc =
        [](ImGuiTestContext *ctx) {
            loadFixture(ctx);
            auto &eq = *getTestState().eventQueue;
            auto &proj = *getTestState().project;
            proj.clearDirtyFlags();

            auto dlg = installDialog(""); // user cancels the picker
            eq.push(OpenOrNewProjectRequestEvent{});
            IM_CHECK(yieldUntil(ctx, [&] { return dlg.count() == 1 && anyModalOpen(); }));

            ctx->ItemClick("**/###modalbtn0"); // "Yes"
            ctx->Yield(3);
            IM_CHECK(proj.axes[static_cast<size_t>(StandardAxis::L0)].showInStrip);
            IM_CHECK(proj.state.filePath.empty()); // a fresh, unsaved project

            setNativeDialogOverrideForTesting(nullptr);
        };

    // Same prompt, "No": the flow co_returns and leaves the (already-closed) project empty.
    IM_REGISTER_TEST(e, "project_dialogs", "open_or_new_empty_decline_leaves_no_project")->TestFunc =
        [](ImGuiTestContext *ctx) {
            loadFixture(ctx);
            auto &eq = *getTestState().eventQueue;
            auto &proj = *getTestState().project;
            proj.clearDirtyFlags();

            auto dlg = installDialog("");
            eq.push(OpenOrNewProjectRequestEvent{});
            IM_CHECK(yieldUntil(ctx, [&] { return dlg.count() == 1 && anyModalOpen(); }));

            ctx->ItemClick("**/###modalbtn1"); // "No"
            ctx->Yield(3);
            IM_CHECK(presentAxisCount(proj) == 0); // doClose ran, nothing recreated

            setNativeDialogOverrideForTesting(nullptr);
        };

    // A .funscript selection starts a new project built around that script. The new-project flow now
    // always confirms placement via the mapping picker (single-axis => L0 by default); accepting it puts
    // the script on L0. The sentinel scratch axis proves the path rebuilt state rather than leaving the
    // fixture.
    IM_REGISTER_TEST(e, "project_dialogs", "open_or_new_funscript_starts_project")->TestFunc =
        [](ImGuiTestContext *ctx) {
            loadFixture(ctx);
            auto &eq = *getTestState().eventQueue;
            auto &proj = *getTestState().project;
            proj.clearDirtyFlags();
            proj.axes[static_cast<size_t>(StandardAxis::S9)].showInStrip = true; // sentinel

            auto dlg = installDialog(ofs::util::toUtf8(fixture("single_point.funscript")));
            eq.push(OpenOrNewProjectRequestEvent{});
            const auto l0 = static_cast<size_t>(StandardAxis::L0);
            // dlg.count() first (doClose ran, override consumed), then the mapping picker, then accept it.
            IM_CHECK(yieldUntil(ctx, [&] { return dlg.count() == 1 && anyModalOpen(); }));
            ctx->ItemClick("**/###modalbtn0"); // "Import" — accept the default target (L0)
            IM_CHECK(yieldUntil(ctx, [&] { return proj.axes[l0].showInStrip && !proj.axes[l0].actions.empty(); }));
            IM_CHECK(!proj.axes[static_cast<size_t>(StandardAxis::S9)].showInStrip);

            setNativeDialogOverrideForTesting(nullptr);
        };

    // Importing a single-axis funscript (no matching standard axis) raises the target-axis picker.
    // Accepting it with the default selection (first free scratch slot) places the script there.
    IM_REGISTER_TEST(e, "project_dialogs", "import_funscript_picker_default_places_on_scratch")->TestFunc =
        [](ImGuiTestContext *ctx) {
            loadFixture(ctx);
            auto &eq = *getTestState().eventQueue;
            auto &proj = *getTestState().project;
            const size_t before = presentAxisCount(proj);

            auto dlg = installDialog(ofs::util::toUtf8(fixture("single_point.funscript")));
            eq.push(ImportFunscriptRequestEvent{});
            // The flow loads the file, then awaits the picker modal (not a native dialog).
            IM_CHECK(yieldUntil(ctx, [&] { return dlg.count() == 1 && anyModalOpen(); }));

            ctx->ItemClick("**/###modalbtn0"); // "Import" — accept the default target
            IM_CHECK(yieldUntil(ctx, [&] { return presentAxisCount(proj) > before; }));

            setNativeDialogOverrideForTesting(nullptr);
        };

    // Cancelling the picker aborts the whole import: no axis is added.
    IM_REGISTER_TEST(e, "project_dialogs", "import_funscript_picker_cancel_aborts")->TestFunc =
        [](ImGuiTestContext *ctx) {
            loadFixture(ctx);
            auto &eq = *getTestState().eventQueue;
            auto &proj = *getTestState().project;
            const size_t before = presentAxisCount(proj);

            auto dlg = installDialog(ofs::util::toUtf8(fixture("single_point.funscript")));
            eq.push(ImportFunscriptRequestEvent{});
            IM_CHECK(yieldUntil(ctx, [&] { return dlg.count() == 1 && anyModalOpen(); }));

            ctx->ItemClick("**/###modalbtn1"); // "Cancel"
            IM_CHECK(yieldUntil(ctx, [&] { return !anyModalOpen(); }));
            ctx->Yield(3);
            IM_CHECK(presentAxisCount(proj) == before); // nothing imported

            setNativeDialogOverrideForTesting(nullptr);
        };

    // Choosing a specific target in the picker routes the script to that exact axis. Pick S5 (absent in
    // the fixture) so the assertion is unambiguous: S5 becomes present with the imported action.
    IM_REGISTER_TEST(e, "project_dialogs", "import_funscript_picker_routes_to_chosen_axis")->TestFunc =
        [](ImGuiTestContext *ctx) {
            loadFixture(ctx);
            auto &eq = *getTestState().eventQueue;
            auto &proj = *getTestState().project;
            const auto s5 = static_cast<size_t>(StandardAxis::S5);
            IM_CHECK(!proj.axes[s5].showInStrip);

            auto dlg = installDialog(ofs::util::toUtf8(fixture("single_point.funscript")));
            eq.push(ImportFunscriptRequestEvent{});
            IM_CHECK(yieldUntil(ctx, [&] { return dlg.count() == 1 && anyModalOpen(); }));

            // The combo lives in the modal popup; point the ref at it so ComboClick can resolve the
            // row-indexed combo id (ComboClick splits "combo/item" at the first '/', so no '**/').
            ctx->SetRef("###ofsmodal");
            ctx->ComboClick("###axispick0/S5");
            ctx->ItemClick("**/###modalbtn0"); // "Import"
            IM_CHECK(yieldUntil(ctx, [&] { return proj.axes[s5].showInStrip && !proj.axes[s5].actions.empty(); }));

            setNativeDialogOverrideForTesting(nullptr);
        };

    // A multi-axis funscript with two unmatched tracks opens a two-row picker (distinct defaults). Pointing
    // both rows at the same axis disables Import; cancelling then leaves the project untouched.
    IM_REGISTER_TEST(e, "project_dialogs", "import_funscript_picker_blocks_duplicate_targets")->TestFunc =
        [](ImGuiTestContext *ctx) {
            loadFixture(ctx);
            auto &eq = *getTestState().eventQueue;
            auto &proj = *getTestState().project;
            const size_t before = presentAxisCount(proj);

            auto dlg = installDialog(ofs::util::toUtf8(fixture("multi_unmatched.funscript")));
            eq.push(ImportFunscriptRequestEvent{});
            IM_CHECK(yieldUntil(ctx, [&] { return dlg.count() == 1 && anyModalOpen(); }));

            ctx->SetRef("###ofsmodal");
            IM_CHECK((ctx->ItemInfo("###modalbtn0").ItemFlags & ImGuiItemFlags_Disabled) == 0); // distinct => enabled
            ctx->ComboClick("###axispick1/S0"); // collide row 1 with row 0's default (S0)
            IM_CHECK((ctx->ItemInfo("###modalbtn0").ItemFlags & ImGuiItemFlags_Disabled) != 0); // duplicate => blocked

            ctx->ItemClick("###modalbtn1"); // "Cancel"
            IM_CHECK(yieldUntil(ctx, [&] { return !anyModalOpen(); }));
            IM_CHECK(presentAxisCount(proj) == before);

            setNativeDialogOverrideForTesting(nullptr);
        };

    // Removing one track of a multi-axis import brings in only the rest: two rows, the first removed via
    // its trash button, so exactly one axis is added.
    IM_REGISTER_TEST(e, "project_dialogs", "import_funscript_picker_removes_a_track")->TestFunc =
        [](ImGuiTestContext *ctx) {
            loadFixture(ctx);
            auto &eq = *getTestState().eventQueue;
            auto &proj = *getTestState().project;
            const size_t before = presentAxisCount(proj);

            auto dlg = installDialog(ofs::util::toUtf8(fixture("multi_unmatched.funscript")));
            eq.push(ImportFunscriptRequestEvent{});
            IM_CHECK(yieldUntil(ctx, [&] { return dlg.count() == 1 && anyModalOpen(); }));

            ctx->SetRef("###ofsmodal");
            ctx->ItemClick("###axispick_remove0"); // drop the first track
            ctx->ItemClick("###modalbtn0");        // "Import" — only the second track remains
            IM_CHECK(yieldUntil(ctx, [&] { return presentAxisCount(proj) == before + 1; }));

            setNativeDialogOverrideForTesting(nullptr);
        };

    // A funscript that maps two tags onto the same axis (root actions => L0, plus a "stroke" track that
    // aliases to L0) opens the picker with both rows defaulting to L0 — a duplicate, so Import is blocked
    // until the user retargets one. Point the aliased row at a free scratch axis, then Import succeeds.
    IM_REGISTER_TEST(e, "project_dialogs", "import_funscript_duplicate_targets_block_until_resolved")->TestFunc =
        [](ImGuiTestContext *ctx) {
            loadFixture(ctx);
            auto &eq = *getTestState().eventQueue;
            auto &proj = *getTestState().project;
            const auto s7 = static_cast<size_t>(StandardAxis::S7);
            IM_CHECK(!proj.axes[s7].showInStrip);

            auto dlg = installDialog(ofs::util::toUtf8(fixture("malformed_alias.funscript")));
            eq.push(ImportFunscriptRequestEvent{});
            IM_CHECK(yieldUntil(ctx, [&] { return dlg.count() == 1 && anyModalOpen(); }));

            ctx->SetRef("###ofsmodal");
            IM_CHECK((ctx->ItemInfo("###modalbtn0").ItemFlags & ImGuiItemFlags_Disabled) != 0); // dup => blocked
            ctx->ComboClick("###axispick1/S7"); // retarget the aliased row to a free axis
            IM_CHECK((ctx->ItemInfo("###modalbtn0").ItemFlags & ImGuiItemFlags_Disabled) == 0); // distinct => enabled
            ctx->ItemClick("###modalbtn0");
            IM_CHECK(yieldUntil(ctx, [&] { return proj.axes[s7].showInStrip && !proj.axes[s7].actions.empty(); }));

            setNativeDialogOverrideForTesting(nullptr);
        };

    // "Add file" in the picker runs the native picker again and appends the chosen file's track(s), so an
    // import can pull in more than the originally selected file. Adding the same single-axis fixture twice
    // yields two rows (distinct free-scratch defaults) → two axes imported.
    IM_REGISTER_TEST(e, "project_dialogs", "import_funscript_picker_adds_a_file")->TestFunc =
        [](ImGuiTestContext *ctx) {
            loadFixture(ctx);
            auto &eq = *getTestState().eventQueue;
            auto &proj = *getTestState().project;
            const size_t before = presentAxisCount(proj);

            auto dlg = installDialog(ofs::util::toUtf8(fixture("single_point.funscript")));
            eq.push(ImportFunscriptRequestEvent{});
            IM_CHECK(yieldUntil(ctx, [&] { return dlg.count() == 1 && anyModalOpen(); }));

            ctx->ItemClick("**/###axispick_add"); // add another file (the override hands back the same fixture)
            // The picker closes, the native picker resolves (2nd call), then the picker re-shows with a 2nd row.
            IM_CHECK(yieldUntil(ctx, [&] { return dlg.count() == 2 && anyModalOpen(); }));
            ctx->ItemClick("**/###modalbtn0"); // Import — both rows default to distinct scratch slots
            IM_CHECK(yieldUntil(ctx, [&] { return presentAxisCount(proj) == before + 2; }));

            setNativeDialogOverrideForTesting(nullptr);
        };

    // Routing an import onto an axis that already holds actions raises a warning confirm — the actions are
    // NOT replaced until the user accepts. Cancelling the confirm leaves the original actions intact.
    IM_REGISTER_TEST(e, "project_dialogs", "import_funscript_overwrite_cancel_preserves")->TestFunc =
        [](ImGuiTestContext *ctx) {
            loadFixture(ctx);
            auto &eq = *getTestState().eventQueue;
            auto &proj = *getTestState().project;
            const auto s5 = static_cast<size_t>(StandardAxis::S5);
            eq.push(AddActionAtTimeEvent{.axis = StandardAxis::S5, .time = 5.0, .pos = 10});
            eq.push(AddActionAtTimeEvent{.axis = StandardAxis::S5, .time = 6.0, .pos = 90});
            ctx->Yield();
            const size_t occupied = proj.axes[s5].actions.size();
            IM_CHECK(occupied == 2);

            auto dlg = installDialog(ofs::util::toUtf8(fixture("single_point.funscript")));
            eq.push(ImportFunscriptRequestEvent{});
            IM_CHECK(yieldUntil(ctx, [&] { return dlg.count() == 1 && anyModalOpen(); }));

            ctx->SetRef("###ofsmodal");
            ctx->ComboClick("###axispick0/S5"); // route onto the occupied axis
            ctx->ItemClick("**/###modalbtn0");  // Import — raises the overwrite confirm, applies nothing yet

            // The overwrite confirm is now up; S5 still holds its original actions.
            IM_CHECK(yieldUntil(ctx, [&] { return anyModalOpen() && proj.axes[s5].actions.size() == occupied; }));
            ctx->ItemClick("**/###modalbtn1"); // Cancel the overwrite
            IM_CHECK(yieldUntil(ctx, [&] { return !anyModalOpen(); }));
            IM_CHECK(proj.axes[s5].actions.size() == occupied); // preserved

            setNativeDialogOverrideForTesting(nullptr);
        };

    // Accepting the overwrite confirm replaces the occupied axis's actions with the imported ones.
    IM_REGISTER_TEST(e, "project_dialogs", "import_funscript_overwrite_confirm_applies")->TestFunc =
        [](ImGuiTestContext *ctx) {
            loadFixture(ctx);
            auto &eq = *getTestState().eventQueue;
            auto &proj = *getTestState().project;
            const auto s5 = static_cast<size_t>(StandardAxis::S5);
            eq.push(AddActionAtTimeEvent{.axis = StandardAxis::S5, .time = 5.0, .pos = 10});
            eq.push(AddActionAtTimeEvent{.axis = StandardAxis::S5, .time = 6.0, .pos = 90});
            ctx->Yield();
            IM_CHECK(proj.axes[s5].actions.size() == 2);

            auto dlg = installDialog(ofs::util::toUtf8(fixture("single_point.funscript")));
            eq.push(ImportFunscriptRequestEvent{});
            IM_CHECK(yieldUntil(ctx, [&] { return dlg.count() == 1 && anyModalOpen(); }));

            ctx->SetRef("###ofsmodal");
            ctx->ComboClick("###axispick0/S5");
            ctx->ItemClick("**/###modalbtn0"); // Import — raises the overwrite confirm
            IM_CHECK(yieldUntil(ctx, [&] { return anyModalOpen() && proj.axes[s5].actions.size() == 2; }));
            ctx->ItemClick("**/###modalbtn0"); // accept the overwrite
            // single_point.funscript carries exactly one action, so the two originals are replaced by it.
            IM_CHECK(yieldUntil(ctx, [&] { return !anyModalOpen() && proj.axes[s5].actions.size() == 1; }));

            setNativeDialogOverrideForTesting(nullptr);
        };

    // Export format 0 (Funscript 1.0) takes a folder and writes one file per device axis into it.
    IM_REGISTER_TEST(e, "project_dialogs", "export_folder_writes_files")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &eq = *getTestState().eventQueue;

        // basic.ofp's axes are empty; export skips empty axes, so give L0 a point to write.
        eq.push(AddActionAtTimeEvent{.axis = StandardAxis::L0, .time = 1.0, .pos = 50});
        ctx->Yield();

        const auto dir = tempPath("ofs_ui_export_folder");
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        auto dlg = installDialog(ofs::util::toUtf8(dir));

        // The write now runs on a worker, so file existence alone is mid-write (the handle may still be
        // open). Wait for lastExport, recorded after the worker resumes — by then the file is closed.
        // basic.ofp carries no export config, so lastExport is nullopt until this export records it.
        auto &proj = *getTestState().project;
        eq.push(ExportFunscriptRequestEvent{.axes = {StandardAxis::L0}, .format = 0}); // no targetPath => picker
        IM_CHECK(yieldUntil(ctx, [&] { return proj.state.lastExport.has_value(); }));
        IM_CHECK(std::filesystem::exists(dir) && !std::filesystem::is_empty(dir));
        IM_CHECK(dlg.count() == 1);

        setNativeDialogOverrideForTesting(nullptr);
        std::filesystem::remove_all(dir);
    };

    // Export format 1 (Funscript 1.1) takes a single output file and writes the multi-axis script.
    IM_REGISTER_TEST(e, "project_dialogs", "export_file_writes_multiaxis")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &eq = *getTestState().eventQueue;

        // basic.ofp's axes are empty; export skips empty axes, so give L0 and R0 points to write.
        eq.push(AddActionAtTimeEvent{.axis = StandardAxis::L0, .time = 1.0, .pos = 50});
        eq.push(AddActionAtTimeEvent{.axis = StandardAxis::R0, .time = 1.0, .pos = 60});
        ctx->Yield();

        const auto out = tempPath("ofs_ui_export_multiaxis.funscript");
        std::filesystem::remove(out);
        auto dlg = installDialog(ofs::util::toUtf8(out));

        // Wait for lastExport (recorded after the worker resumes), not bare file existence — the worker
        // may still hold the handle mid-write. basic.ofp carries no export config, so it starts nullopt.
        auto &proj = *getTestState().project;
        eq.push(ExportFunscriptRequestEvent{.axes = {StandardAxis::L0, StandardAxis::R0}, .format = 1});
        IM_CHECK(yieldUntil(ctx, [&] { return proj.state.lastExport.has_value(); }));
        IM_CHECK(std::filesystem::exists(out));
        IM_CHECK(dlg.count() == 1);

        setNativeDialogOverrideForTesting(nullptr);
        std::filesystem::remove(out);
    };

    // guardUnsaved's Save branch when no file path exists: the unsaved prompt's "Save" leads into the
    // save-as picker, the write succeeds, then proceed() (here doClose) runs.
    IM_REGISTER_TEST(e, "project_dialogs", "unsaved_save_uses_picker_then_proceeds")->TestFunc =
        [](ImGuiTestContext *ctx) {
            loadFixture(ctx);
            auto &eq = *getTestState().eventQueue;
            auto &proj = *getTestState().project;
            const auto l0 = static_cast<size_t>(StandardAxis::L0);

            const auto out = tempPath("ofs_ui_unsaved_save.ofp");
            std::filesystem::remove(out);
            proj.state.filePath.clear();     // force the save-as picker (no known path)
            proj.state.settingsDirty = true; // force the unsaved prompt
            auto dlg = installDialog(ofs::util::toUtf8(out));

            eq.push(CloseProjectRequestEvent{});
            IM_CHECK(yieldUntil(ctx, [&] { return anyModalOpen(); }));
            ctx->ItemClick("**/###modalbtn0"); // "Save"

            IM_CHECK(yieldUntil(ctx, [&] { return std::filesystem::exists(out); }));
            // Save succeeded => proceed() (doClose) cleared the project.
            IM_CHECK(yieldUntil(ctx, [&] { return !proj.axes[l0].showInStrip; }));
            IM_CHECK(dlg.count() == 1);

            setNativeDialogOverrideForTesting(nullptr);
            std::filesystem::remove(out);
        };

    // Loading a project whose media is missing fires relocateFlow: "Relocate" then a cancelled picker
    // (empty result) leaves the media unset and the flow finishes via openProjectVideo. Two dialogs
    // run: the project-open picker (returns the .ofp) and the relocate-media picker (returns empty).
    IM_REGISTER_TEST(e, "project_dialogs", "relocate_flow_prompts_on_missing_media")->TestFunc =
        [](ImGuiTestContext *ctx) {
            loadFixture(ctx);
            auto &eq = *getTestState().eventQueue;
            auto &proj = *getTestState().project;

            // Author a project file that points at a media file that does not exist.
            const auto projFile = tempPath("ofs_ui_relocate.ofp");
            std::filesystem::remove(projFile);
            proj.state.filePath = ofs::util::toUtf8(projFile);
            proj.state.mediaPath = "Q:/ofs_does_not_exist/missing_video.mp4";
            proj.axes[static_cast<size_t>(StandardAxis::L0)].dirty = true;
            eq.push(SaveProjectEvent{false}); // filePath set => writes directly, no picker
            IM_CHECK(yieldUntil(ctx, [&] { return std::filesystem::exists(projFile); }));

            // Reopen it: load detects the missing media and raises the relocate prompt.
            proj.clearDirtyFlags();
            auto open = installDialog(ofs::util::toUtf8(projFile));
            eq.push(OpenOrNewProjectRequestEvent{});
            IM_CHECK(yieldUntil(ctx, [&] { return open.count() == 1 && anyModalOpen(); }));

            auto relocate = installDialog(""); // the relocate picker is cancelled
            ctx->ItemClick("**/###modalbtn0"); // "Relocate"
            IM_CHECK(yieldUntil(ctx, [&] { return relocate.count() == 1; }));
            IM_CHECK(yieldUntil(ctx, [&] { return !anyModalOpen(); }));

            setNativeDialogOverrideForTesting(nullptr);
            std::filesystem::remove(projFile);
        };

    // Save a region's node graph to a .json, then load it back into the same region. The default graph
    // ships no embedded scripts and matches the region's axes, so the load applies immediately.
    IM_REGISTER_TEST(e, "project_dialogs", "graph_save_and_load_roundtrip")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &eq = *getTestState().eventQueue;
        auto &proj = *getTestState().project;

        eq.push(CreateRegionEvent{.axisRole = StandardAxis::L0, .startTime = 1.0, .endTime = 5.0});
        IM_CHECK(yieldUntil(ctx, [&] { return !proj.regions.empty(); }));
        const int regionId = proj.regions[0].id;

        const auto graphFile = tempPath("ofs_ui_graph.json");
        std::filesystem::remove(graphFile);
        auto save = installDialog(ofs::util::toUtf8(graphFile));
        eq.push(SaveGraphEvent{regionId});
        IM_CHECK(yieldUntil(ctx, [&] { return std::filesystem::exists(graphFile); }));
        IM_CHECK(save.count() == 1);

        // Load it back: matching axes + no trust needed => applied directly, no pending load left. Wait
        // on the dialog call itself (the load leaves no flag: pendingGraphLoad stays empty throughout).
        auto load = installDialog(ofs::util::toUtf8(graphFile));
        eq.push(LoadGraphEvent{regionId});
        IM_CHECK(yieldUntil(ctx, [&] { return load.count() == 1; }));
        ctx->Yield(3); // resume + applyLoadedGraph
        IM_CHECK(!proj.pendingGraphLoad.has_value());
        IM_CHECK(proj.findRegion(regionId) != nullptr);

        setNativeDialogOverrideForTesting(nullptr);
        std::filesystem::remove(graphFile);
    };
}
