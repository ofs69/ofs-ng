#include "Core/Events.h"
#include "Core/ProjectLifecycleEvents.h"
#include "Core/StandardAxis.h"
#include "UI/ModalManager.h"
#include "UI/Modals.h"
#include "Util/PathUtil.h"
#include "helpers/TestState.h"
#include <filesystem>
#include <imgui.h>
#include <imgui_te_context.h>
#include <imgui_te_engine.h>
#include <memory>

using namespace ofs;

namespace {
// BeginPopupModal pushes onto ImGui's OpenPopupStack, so this reports whether any modal is up
// without touching the current window — safe to call between frames (see suite_command_palette).
bool anyModalOpen() {
    return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
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
} // namespace

// UI tests for the coroutine-driven modal system (ModalManager + co::Fire flows). They drive the
// real app: events flow through drain() -> a flow's co_await Confirm -> ModalManager renders the
// popup -> a click resumes the flow in pump(). Asserts use observable effects (popup open/closed,
// project state) — never the Save button, which would open a blocking native file dialog.
void RegisterModalsTests(ImGuiTestEngine *e) {
    // A fire-and-forget message shows a one-button popup that the OK button dismisses.
    IM_REGISTER_TEST(e, "modals", "message_shows_and_dismisses")->TestFunc = [](ImGuiTestContext *ctx) {
        auto &eq = *getTestState().eventQueue;
        showMessage(eq,
                    {.title = "Modal Test", .message = "Hello", .buttons = {"OK"}, .severity = ModalSeverity::Info});
        ctx->Yield(3);
        IM_CHECK(anyModalOpen());

        ctx->ItemClick("**/###modalbtn0");
        ctx->Yield(3);
        IM_CHECK(!anyModalOpen());
    };

    // Two queued messages render one at a time (FIFO): dismissing the first reveals the second.
    IM_REGISTER_TEST(e, "modals", "messages_serialize_fifo")->TestFunc = [](ImGuiTestContext *ctx) {
        auto &eq = *getTestState().eventQueue;
        showMessage(eq, {.title = "First", .message = "1", .buttons = {"OK"}, .severity = ModalSeverity::Info});
        showMessage(eq, {.title = "Second", .message = "2", .buttons = {"OK"}, .severity = ModalSeverity::Info});
        ctx->Yield(3);
        IM_CHECK(anyModalOpen()); // first

        ctx->ItemClick("**/###modalbtn0");
        ctx->Yield(3);
        IM_CHECK(anyModalOpen()); // second now shown

        ctx->ItemClick("**/###modalbtn0");
        ctx->Yield(3);
        IM_CHECK(!anyModalOpen());
    };

    // Closing a dirty project raises the unsaved-changes modal; "Cancel" resumes the flow with the
    // cancel result, so the project stays open.
    IM_REGISTER_TEST(e, "modals", "unsaved_cancel_keeps_project")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        const auto l0 = static_cast<size_t>(StandardAxis::L0);
        IM_CHECK(proj.axes[l0].showInStrip);

        proj.state.settingsDirty = true; // force isDirty() so the prompt appears
        eq.push(CloseProjectRequestEvent{});
        ctx->Yield(3);
        IM_CHECK(anyModalOpen());

        ctx->ItemClick("**/###modalbtn2"); // index 2 = "Cancel" in {Save, Don't Save, Cancel}
        ctx->Yield(3);
        IM_CHECK(!anyModalOpen());
        IM_CHECK(proj.axes[l0].showInStrip); // cancel => project untouched
    };

    // Same prompt, "Don't Save" => the flow proceeds past the await and doClose() clears the project.
    IM_REGISTER_TEST(e, "modals", "unsaved_discard_closes_project")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        const auto l0 = static_cast<size_t>(StandardAxis::L0);
        IM_CHECK(proj.axes[l0].showInStrip);

        proj.state.settingsDirty = true;
        eq.push(CloseProjectRequestEvent{});
        ctx->Yield(3);
        IM_CHECK(anyModalOpen());

        ctx->ItemClick("**/###modalbtn1"); // index 1 = "Don't Save" in {Save, Don't Save, Cancel}
        ctx->Yield(3);
        IM_CHECK(!anyModalOpen());
        IM_CHECK(!proj.axes[l0].showInStrip); // discard => doClose() reset the axes
    };

    // A non-dirty close needs no prompt: the flow runs to completion synchronously, no modal appears.
    IM_REGISTER_TEST(e, "modals", "clean_close_skips_modal")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        const auto l0 = static_cast<size_t>(StandardAxis::L0);
        proj.clearDirtyFlags();

        eq.push(CloseProjectRequestEvent{});
        ctx->Yield(3);
        IM_CHECK(!anyModalOpen());
        IM_CHECK(!proj.axes[l0].showInStrip); // closed without a prompt
    };

    // A custom-bodied modal renders its body (a button) and closes when the body returns true.
    IM_REGISTER_TEST(e, "modals", "custom_modal_shows_and_closes")->TestFunc = [](ImGuiTestContext *ctx) {
        auto &eq = *getTestState().eventQueue;
        showCustomModal(eq, {.title = "Custom Test", .width = 300.0f, .body = []() -> bool {
                                 ImGui::TextUnformatted("custom body");
                                 return ImGui::Button("Done###done");
                             }});
        ctx->Yield(3);
        IM_CHECK(anyModalOpen());

        ctx->ItemClick("**/###done");
        ctx->Yield(3);
        IM_CHECK(!anyModalOpen());
    };

    // Custom-bodied and spec modals share one FIFO queue: a custom modal raised first renders
    // first, and dismissing it reveals the spec message behind it.
    IM_REGISTER_TEST(e, "modals", "custom_modal_fifo_with_spec")->TestFunc = [](ImGuiTestContext *ctx) {
        auto &eq = *getTestState().eventQueue;
        showCustomModal(
            eq, {.title = "Custom First", .body = []() -> bool { return ImGui::Button("DoneCustom###donecustom"); }});
        showMessage(eq, {.title = "Spec Second", .message = "2", .buttons = {"OK"}, .severity = ModalSeverity::Info});
        ctx->Yield(3);
        IM_CHECK(anyModalOpen()); // custom first

        ctx->ItemClick("**/###donecustom");
        ctx->Yield(3);
        IM_CHECK(anyModalOpen()); // spec now shown

        ctx->ItemClick("**/###modalbtn0");
        ctx->Yield(3);
        IM_CHECK(!anyModalOpen());
    };

    // The migrated Export Funscript dialog: opening it from the File menu raises the modal, and
    // Cancel closes it (the whole open path now runs through ModalManager's custom-body branch).
    IM_REGISTER_TEST(e, "modals", "export_dialog_opens_and_cancels")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        ctx->MenuClick("//##MainMenuBar/###menu_file/###menu_export");
        ctx->Yield(3);
        IM_CHECK(anyModalOpen());

        ctx->ItemClick("**/###export_cancel");
        ctx->Yield(3);
        IM_CHECK(!anyModalOpen());
    };

    // Quick Export with nothing exported yet falls back to the regular Export modal (same dialog the
    // File menu opens), so the user picks format/axes/path the first time.
    IM_REGISTER_TEST(e, "modals", "quick_export_without_config_opens_dialog")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;
        proj.state.lastExport.reset();

        eq.push(QuickExportEvent{});
        ctx->Yield(3);
        IM_CHECK(anyModalOpen());

        ctx->ItemClick("**/###export_cancel");
        ctx->Yield(3);
        IM_CHECK(!anyModalOpen());
    };

    // Once a config is remembered, Quick Export replays it with no dialog and writes straight to the
    // stored path.
    IM_REGISTER_TEST(e, "modals", "quick_export_with_config_skips_dialog")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &eq = *getTestState().eventQueue;

        // basic.ofp's axes are empty; export skips empty axes, so give L0 a point to write.
        eq.push(ofs::AddActionAtTimeEvent{.axis = ofs::StandardAxis::L0, .time = 1.0, .pos = 50});
        ctx->Yield();

        auto outDir = std::filesystem::temp_directory_path() / "ofs_ui_quick_export";
        std::filesystem::remove_all(outDir);
        proj.state.lastExport =
            ofs::ExportConfig{.format = 0, .axes = {ofs::StandardAxis::L0}, .outputPath = outDir.string()};

        eq.push(QuickExportEvent{});
        // The write runs on a worker thread (JobAwait) and the coroutine resumes a later frame, so
        // poll rather than assume a fixed frame count — a fixed Yield() raced the worker (flaky).
        IM_CHECK(
            yieldUntil(ctx, [&] { return std::filesystem::exists(outDir) && !std::filesystem::is_empty(outDir); }));
        IM_CHECK(!anyModalOpen()); // silent replay — no picker

        std::filesystem::remove_all(outDir);
    };

    // Escape dismisses a spec modal by selecting the last button (the cancel/dismiss slot). Drives
    // the IsKeyPressed(Escape) branch in render() that the click-based tests never reach.
    IM_REGISTER_TEST(e, "modals", "escape_dismisses_modal")->TestFunc = [](ImGuiTestContext *ctx) {
        auto &eq = *getTestState().eventQueue;
        showMessage(eq, {.title = "Esc Test", .message = "press escape", .buttons = {"OK"}});
        ctx->Yield(3);
        IM_CHECK(anyModalOpen());

        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(3);
        IM_CHECK(!anyModalOpen());
    };

    // An Error-severity message tints its title via the Error branch of severityColor(); a warning
    // covers the Warning branch. Both render and dismiss like any other one-button popup.
    IM_REGISTER_TEST(e, "modals", "severity_error_and_warning_render")->TestFunc = [](ImGuiTestContext *ctx) {
        auto &eq = *getTestState().eventQueue;
        showError(eq, "Boom", "an error");
        ctx->Yield(3);
        IM_CHECK(anyModalOpen());
        ctx->ItemClick("**/###modalbtn0");
        ctx->Yield(3);

        showWarning(eq, "Careful", "a warning");
        ctx->Yield(3);
        IM_CHECK(anyModalOpen());
        ctx->ItemClick("**/###modalbtn0");
        ctx->Yield(3);
        IM_CHECK(!anyModalOpen());
    };

    // The native file-dialog path, driven through the test seam so no real OS dialog opens. A
    // FileDialog flow suspends; ModalManager fills the (overridden) dialog result, polls it,
    // resumes the flow with the chosen path, and persists the remembered dir.
    // Exercises pump()'s dialog branch, pumpNativeDialog (dispatch/poll/resume), render()'s
    // dialog-skip, resolveDir, remember and saveDialogDirs.
    IM_REGISTER_TEST(e, "modals", "native_dialog_resolves_and_remembers")->TestFunc = [](ImGuiTestContext *ctx) {
        auto &eq = *getTestState().eventQueue;
        const auto picked = std::filesystem::temp_directory_path() / "ofs_native_dlg" / "chosen.funscript";
        std::filesystem::create_directories(picked.parent_path());
        setNativeDialogOverrideForTesting(
            [picked](const FileDialogSpec &, const std::string &) { return ofs::util::toUtf8(picked); });

        auto result = std::make_shared<std::string>("UNSET");
        pickFile(eq,
                 {.kind = FileDialogKind::Save, .key = "ut_native", .title = "Pick", .defaultName = "chosen.funscript"},
                 [result](std::string p) { *result = std::move(p); });

        ctx->Yield(8); // dispatch -> worker returns canned path -> done -> resume
        IM_CHECK(*result == ofs::util::toUtf8(picked));
        IM_CHECK(!anyModalOpen()); // native dialogs draw no ImGui popup
        // The chosen file's directory is now remembered for key "ut_native".
        IM_CHECK(std::filesystem::exists(ofs::util::getPrefPath() / "dialog_paths.json"));

        // Second dialog on the same key: resolveDir now finds the remembered directory (rather than
        // falling back), and the override is handed that dir.
        auto seenDir = std::make_shared<std::string>();
        setNativeDialogOverrideForTesting([picked, seenDir](const FileDialogSpec &, const std::string &dir) {
            *seenDir = dir;
            return ofs::util::toUtf8(picked);
        });
        *result = "UNSET";
        pickFile(eq, {.kind = FileDialogKind::Save, .key = "ut_native", .title = "Pick again"},
                 [result](std::string p) { *result = std::move(p); });
        ctx->Yield(8);
        IM_CHECK(*result == ofs::util::toUtf8(picked));
        IM_CHECK(!seenDir->empty()); // the remembered directory was resolved and passed through

        setNativeDialogOverrideForTesting(nullptr);
        std::filesystem::remove_all(picked.parent_path());
    };

    // A cancelled native dialog (empty result) resumes the flow with "" and remembers nothing.
    IM_REGISTER_TEST(e, "modals", "native_dialog_cancel_returns_empty")->TestFunc = [](ImGuiTestContext *ctx) {
        auto &eq = *getTestState().eventQueue;
        setNativeDialogOverrideForTesting([](const FileDialogSpec &, const std::string &) { return std::string(); });

        auto result = std::make_shared<std::string>("UNSET");
        pickFile(eq, {.kind = FileDialogKind::Open, .key = "ut_cancel", .title = "Pick"},
                 [result](std::string p) { *result = std::move(p); });

        ctx->Yield(8);
        IM_CHECK(result->empty()); // cancel => empty path
        IM_CHECK(!anyModalOpen());

        setNativeDialogOverrideForTesting(nullptr);
    };

    // The multi-select path (FileDialogMulti / pickFiles): the seam returns several paths, the awaiting flow
    // resumes with the whole list, and the first path's directory is remembered like the single path does.
    IM_REGISTER_TEST(e, "modals", "native_multi_dialog_resolves_all_paths")->TestFunc = [](ImGuiTestContext *ctx) {
        auto &eq = *getTestState().eventQueue;
        const auto dir = std::filesystem::temp_directory_path() / "ofs_native_multi";
        std::filesystem::create_directories(dir);
        const auto a = ofs::util::toUtf8(dir / "a.funscript");
        const auto b = ofs::util::toUtf8(dir / "b.funscript");
        auto sawAllowMany = std::make_shared<bool>(false);
        setNativeMultiDialogOverrideForTesting([a, b, sawAllowMany](const FileDialogSpec &spec, const std::string &) {
            *sawAllowMany = spec.allowMany; // the multi seam only fires for an allowMany request
            return std::vector<std::string>{a, b};
        });

        auto result = std::make_shared<std::vector<std::string>>();
        pickFiles(eq, {.key = "ut_multi", .title = "Pick many"},
                  [result](std::vector<std::string> p) { *result = std::move(p); });

        ctx->Yield(8); // dispatch -> seam returns canned paths -> done -> resume
        IM_CHECK(*sawAllowMany);
        IM_CHECK_EQ(result->size(), (size_t)2);
        IM_CHECK((*result)[0] == a);
        IM_CHECK((*result)[1] == b);
        IM_CHECK(!anyModalOpen());
        IM_CHECK(
            std::filesystem::exists(ofs::util::getPrefPath() / "dialog_paths.json")); // first path's dir remembered

        setNativeMultiDialogOverrideForTesting(nullptr);
        std::filesystem::remove_all(dir);
    };

    // A cancelled multi dialog (no override / empty list) resumes the flow with an empty list.
    IM_REGISTER_TEST(e, "modals", "native_multi_dialog_cancel_returns_empty")->TestFunc = [](ImGuiTestContext *ctx) {
        auto &eq = *getTestState().eventQueue;
        setNativeMultiDialogOverrideForTesting(
            [](const FileDialogSpec &, const std::string &) { return std::vector<std::string>{}; });

        auto result = std::make_shared<std::vector<std::string>>();
        result->emplace_back("UNSET");
        pickFiles(eq, {.key = "ut_multi_cancel", .title = "Pick"},
                  [result](std::vector<std::string> p) { *result = std::move(p); });

        ctx->Yield(8);
        IM_CHECK(result->empty()); // cancel => empty list
        IM_CHECK(!anyModalOpen());

        setNativeMultiDialogOverrideForTesting(nullptr);
    };
}
