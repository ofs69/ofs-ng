#include "Core/Events.h" // ModifyEvent
#include "Core/ScriptProject.h"
#include "Core/TranscodeEvents.h"
#include "Format/AppSettings.h"
#include "Services/CommandRegistry.h"
#include "UI/ModalManager.h"  // setNativeDialogOverrideForTesting / FileDialogSpec — drive the folder picker
#include "UI/Notifications.h" // NotificationState — assert the task entry + failure notification
#include "Util/PathUtil.h"
#include "helpers/TestState.h"
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <imgui_te_context.h>
#include <imgui_te_engine.h>
#include <string>
#include <system_error>

using namespace ofs;

// Drives the intra-frame optimize UI in the live app: the gated command, the options modal, and the
// non-blocking footer task the run surfaces as. The worker spawns real ffmpeg/ffprobe, which are NOT
// staged next to the test binary (they ship beside the app, not bin/test/ui-tests) — and even if present,
// the bogus temp source below can't be decoded. Either way the worker ends in Failed, which is exactly the
// background-task path this exercises: no ffmpeg run and no production "runner seam" stub are needed for it.

namespace {
bool anyModalOpen() {
    return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

template <class Pred> bool yieldUntil(ImGuiTestContext *ctx, Pred pred, int maxFrames = 600) {
    for (int i = 0; i < maxFrames; ++i) {
        if (pred())
            return true;
        ctx->Yield();
    }
    return pred();
}

// Write a tiny file and return its UTF-8 path (a stand-in "source"/"intra copy" — fingerprintable, so
// intraOutputPath() resolves, but not a decodable video, so any encode attempt fails fast).
std::string makeTempFile(const char *name) {
    const std::filesystem::path p = std::filesystem::temp_directory_path() / name;
    std::ofstream(p, std::ios::binary | std::ios::trunc) << "not a real video, just bytes for hashing";
    return ofs::util::toUtf8(p);
}

void setOutputDir(ImGuiTestContext *ctx, std::string dir) {
    getTestState().eventQueue->push(
        ModifyEvent<AppSettings>{[dir = std::move(dir)](AppSettings &s) { s.intraOutputDir = dir; }});
    ctx->Yield(2); // let OfsApp apply the settings mutation before anything reads it back
}
} // namespace

void RegisterTranscodeTests(ImGuiTestEngine *e) {
    // ── The command is registered and gated off until its prerequisites are met ──────────
    // In the fixture (no ffmpeg beside the test binary, dummy player) the gate must hold the command
    // disabled — it must never be spuriously runnable. The output dir is no longer part of the gate (it
    // is picked on demand), so it being empty here is irrelevant; ffmpeg/real-media absence still gates.
    IM_REGISTER_TEST(e, "transcode", "command_gated_when_unconfigured")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        const CommandRegistry &reg = *getTestState().commandRegistry;
        const Command *cmd = reg.find("video.optimize-intra");
        IM_CHECK(cmd != nullptr);  // registered
        IM_CHECK(!cmd->enabled()); // gated off (no ffmpeg / no real media)
    };

    // ── Options modal opens via the event latch and Cancel dismisses it ──────────────────
    // A valid output dir is required to reach the options modal — without one the optimize entry point
    // diverts to the missing-dir prompt (covered separately below). No source is needed: with a dir set
    // the options modal opens (Start is just disabled), so this isolates the open/cancel chrome and ids.
    IM_REGISTER_TEST(e, "transcode", "options_modal_opens_and_cancels")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        const std::string outDir = ofs::util::toUtf8(std::filesystem::temp_directory_path() / "ofs_intra_out");
        std::error_code ec;
        std::filesystem::create_directories(ofs::util::fromUtf8(outDir), ec);
        setOutputDir(ctx, outDir);

        getTestState().eventQueue->push(OpenTranscodeDialogEvent{});
        IM_CHECK(yieldUntil(ctx, [&] { return anyModalOpen(); }));

        // The percentage scale radios and the Cancel button are addressable by their language-stable ids.
        IM_CHECK(ctx->ItemExists("**/intra_scale_full"));
        IM_CHECK(ctx->ItemExists("**/intra_scale_half"));
        ctx->ItemClick("**/intra_options_cancel");
        IM_CHECK(yieldUntil(ctx, [&] { return !anyModalOpen(); }));

        setOutputDir(ctx, ""); // restore the app-global slate for later suites
        std::filesystem::remove_all(ofs::util::fromUtf8(outDir), ec);
    };

    // ── No output dir → the optimize entry point opens the folder picker directly ─────────
    // With no output dir configured, openTranscodeOptionsModal goes straight to the folder picker (no
    // scolding modal — there is nothing wrong yet). A valid pick is persisted and re-opens the flow,
    // landing on the real options modal (proven by the scale ids appearing).
    IM_REGISTER_TEST(e, "transcode", "missing_output_dir_picks_folder")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        setOutputDir(ctx, ""); // ensure no output dir regardless of prior suites

        const auto outDir = std::filesystem::temp_directory_path() / "ofs_intra_picked";
        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);
        setNativeDialogOverrideForTesting(
            [outDir](const FileDialogSpec &, const std::string &) { return ofs::util::toUtf8(outDir); });

        getTestState().eventQueue->push(OpenTranscodeDialogEvent{});
        // Picker resolves to outDir → persisted → dialog re-opens → options modal (scale ids present).
        IM_CHECK(yieldUntil(ctx, [&] { return anyModalOpen() && ctx->ItemExists("**/intra_scale_full"); }));

        ctx->ItemClick("**/intra_options_cancel");
        IM_CHECK(yieldUntil(ctx, [&] { return !anyModalOpen(); }));

        setNativeDialogOverrideForTesting(nullptr);
        setOutputDir(ctx, "");
        std::filesystem::remove_all(outDir, ec);
    };

    // ── Selecting MJPEG raises a stacked confirm; declining reverts to the safe H.264 codec ──
    // The options modal stays open underneath (a true stack, not a close/reopen): the MJPEG quality
    // slider appears only while MJPEG is committed, so its presence tracks the codec across the warning.
    IM_REGISTER_TEST(e, "transcode", "mjpeg_selection_warns_and_reverts")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        const std::string outDir = ofs::util::toUtf8(std::filesystem::temp_directory_path() / "ofs_intra_out");
        std::error_code ec;
        std::filesystem::create_directories(ofs::util::fromUtf8(outDir), ec);
        setOutputDir(ctx, outDir);

        getTestState().eventQueue->push(OpenTranscodeDialogEvent{});
        IM_CHECK(yieldUntil(ctx, [&] { return anyModalOpen() && ctx->ItemExists("**/intra_codec_mjpeg"); }));
        IM_CHECK(ctx->ItemExists("**/intra_crf"));      // H.264 quality slider (default codec)
        IM_CHECK(!ctx->ItemExists("**/intra_mjpeg_q")); // no MJPEG slider yet

        // Pick MJPEG → the stacked warning appears over the still-open options modal.
        ctx->ItemClick("**/intra_codec_mjpeg");
        IM_CHECK(yieldUntil(ctx, [&] { return ctx->ItemExists("###ofsmodal1/**/###modalbtn1"); }));
        IM_CHECK(ctx->ItemExists("**/intra_codec_mjpeg")); // options modal is still alive underneath

        // Decline (button 1 = Cancel) → codec falls back to H.264, so the MJPEG slider must disappear.
        ctx->ItemClick("###ofsmodal1/**/###modalbtn1");
        IM_CHECK(yieldUntil(ctx, [&] { return !ctx->ItemExists("###ofsmodal1/**/###modalbtn1"); }));
        IM_CHECK(
            yieldUntil(ctx, [&] { return ctx->ItemExists("**/intra_crf") && !ctx->ItemExists("**/intra_mjpeg_q"); }));

        // Accept this time (button 0 = Use MJPEG) → codec stays MJPEG, so the MJPEG slider shows.
        ctx->ItemClick("**/intra_codec_mjpeg");
        IM_CHECK(yieldUntil(ctx, [&] { return ctx->ItemExists("###ofsmodal1/**/###modalbtn0"); }));
        ctx->ItemClick("###ofsmodal1/**/###modalbtn0");
        IM_CHECK(
            yieldUntil(ctx, [&] { return ctx->ItemExists("**/intra_mjpeg_q") && !ctx->ItemExists("**/intra_crf"); }));

        ctx->ItemClick("**/intra_options_cancel");
        IM_CHECK(yieldUntil(ctx, [&] { return !anyModalOpen(); }));
        setOutputDir(ctx, "");
        std::filesystem::remove_all(ofs::util::fromUtf8(outDir), ec);
    };

    // ── A high-resolution source gates Start behind a "reduce resolution?" confirm ───────
    // The probe (ffprobe) isn't available in the fixture, so feed the source dimensions directly via
    // MediaInfoReadyEvent (matched on the path the modal probes). At Full scale an 8K output exceeds 4K,
    // so Start must raise the stacked warning; "Reduce resolution" dismisses it and leaves the dialog open.
    IM_REGISTER_TEST(e, "transcode", "high_resolution_warns_before_start")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        const std::string outDir = ofs::util::toUtf8(std::filesystem::temp_directory_path() / "ofs_intra_out");
        std::error_code ec;
        std::filesystem::create_directories(ofs::util::fromUtf8(outDir), ec);
        const std::string source = makeTempFile("ofs_intra_8k.bin");
        proj.state.originalMediaPath = source;
        proj.state.mediaPath = source;
        proj.activeSource = MediaSource::Original;
        setOutputDir(ctx, outDir);

        getTestState().eventQueue->push(OpenTranscodeDialogEvent{});
        IM_CHECK(yieldUntil(ctx, [&] { return anyModalOpen() && ctx->ItemExists("**/intra_start"); }));

        // Inject an 8K source size so the high-resolution gate trips (output > UHD 4K at Full scale).
        getTestState().eventQueue->push(MediaInfoReadyEvent{source, MediaInfo{7680, 4320, 60.0, 60.0}});
        ctx->Yield(2);

        ctx->ItemClick("**/intra_start"); // must raise the stacked warning, not start the transcode
        IM_CHECK(yieldUntil(ctx, [&] { return ctx->ItemExists("###ofsmodal1/**/###modalbtn0"); }));
        IM_CHECK(proj.transcode.phase == TranscodePhase::Idle); // nothing kicked off yet

        // "Reduce resolution" (button 0) dismisses the warning; the options modal stays open, no run.
        ctx->ItemClick("###ofsmodal1/**/###modalbtn0");
        IM_CHECK(yieldUntil(ctx, [&] {
            return !ctx->ItemExists("###ofsmodal1/**/###modalbtn0") && ctx->ItemExists("**/intra_start");
        }));
        IM_CHECK(proj.transcode.phase == TranscodePhase::Idle);

        ctx->ItemClick("**/intra_options_cancel");
        IM_CHECK(yieldUntil(ctx, [&] { return !anyModalOpen(); }));
        setOutputDir(ctx, "");
        std::filesystem::remove(ofs::util::fromUtf8(source), ec);
        std::filesystem::remove_all(ofs::util::fromUtf8(outDir), ec);
    };

    // ── Start → request → worker → (failure) surfaces as a footer task, NOT a blocking modal ──────
    IM_REGISTER_TEST(e, "transcode", "start_runs_as_background_task")->TestFunc = [](ImGuiTestContext *ctx) {
        loadFixture(ctx);
        auto &proj = *getTestState().project;
        auto &notes = *getTestState().notifications;

        // Configure a resolvable (but non-encodable) source so Start is enabled.
        const std::string outDir = ofs::util::toUtf8(std::filesystem::temp_directory_path() / "ofs_intra_out");
        std::error_code ec;
        std::filesystem::create_directories(ofs::util::fromUtf8(outDir), ec);
        const std::string source = makeTempFile("ofs_intra_source.bin");
        proj.state.originalMediaPath = source;
        proj.state.mediaPath = source;
        proj.activeSource = MediaSource::Original;
        setOutputDir(ctx, outDir);

        getTestState().eventQueue->push(OpenTranscodeDialogEvent{});
        IM_CHECK(yieldUntil(ctx, [&] { return anyModalOpen() && ctx->ItemExists("**/intra_start"); }));

        const size_t logBefore = notes.log.size();
        ctx->ItemClick(
            "**/intra_start"); // pushes TranscodeRequestEvent; the options modal closes, no modal replaces it
        IM_CHECK(yieldUntil(ctx, [&] { return !anyModalOpen(); }));

        // The worker can't produce a valid output, so it lands in a terminal non-Done phase — all without a modal.
        IM_CHECK(yieldUntil(ctx, [&] {
            return proj.transcode.phase == TranscodePhase::Failed || proj.transcode.phase == TranscodePhase::Cancelled;
        }));
        IM_CHECK(!proj.transcode.active);
        IM_CHECK(proj.state.intraMediaPath.empty()); // a failed run never adopts an output
        IM_CHECK(!anyModalOpen());                   // the migration: progress is a footer task, never a blocking modal

        // The task entry is removed when the run ends, and the failure surfaces as a bell/toast notification.
        IM_CHECK(yieldUntil(ctx, [&] { return notes.tasks.empty(); }));
        if (proj.transcode.phase == TranscodePhase::Failed)
            IM_CHECK(notes.log.size() > logBefore);

        // Restore app-global settings so later suites see a clean slate (the project is reloaded by their
        // own loadFixture, but appSettings persists across the run).
        setOutputDir(ctx, "");
        std::filesystem::remove(ofs::util::fromUtf8(source), ec);
        std::filesystem::remove_all(ofs::util::fromUtf8(outDir), ec);
    };
}
