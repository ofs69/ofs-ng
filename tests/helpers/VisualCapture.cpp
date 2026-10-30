#include "VisualCapture.h"

#include "Core/Events.h"
#include "Core/SimulatorSettings.h"
#include "Util/PathUtil.h"
#include "Video/DummyVideoPlayer.h"
#include "helpers/TestState.h"
#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <glad/gl.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_te_context.h>
#include <imgui_te_engine.h>
#include <nlohmann/json.hpp>

namespace {
bool captureFramebuffer(ImGuiID viewportId, int x, int y, int width, int height, unsigned int *pixels, void *) {
    if (viewportId != ImGui::GetMainViewport()->ID || ImGui::GetIO().DisplayFramebufferScale.x != 1.f ||
        ImGui::GetIO().DisplayFramebufferScale.y != 1.f)
        return false;

    GLint oldReadBuffer = 0;
    GLint oldAlignment = 0;
    GLint oldRowLength = 0;
    glGetIntegerv(GL_READ_BUFFER, &oldReadBuffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &oldAlignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &oldRowLength);
    // The engine requests captures from PostSwap, so read the frame that was just presented.
    glReadBuffer(GL_FRONT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(x, static_cast<int>(ImGui::GetIO().DisplaySize.y) - y - height, width, height, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixels);
    glReadBuffer(static_cast<GLenum>(oldReadBuffer));
    glPixelStorei(GL_PACK_ALIGNMENT, oldAlignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, oldRowLength);
    for (int row = 0; row < height / 2; ++row)
        std::swap_ranges(pixels + row * width, pixels + (row + 1) * width, pixels + (height - row - 1) * width);
    return true;
}

bool saveCapture(ImGuiTestContext *ctx, const std::string &directory, const char *name, const char *window = nullptr) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImRect rect(viewport->Pos, viewport->Pos + viewport->Size);
    if (window != nullptr) {
        ImGuiWindow *target = ImGui::FindWindowByName(window);
        IM_CHECK_RETV(target != nullptr && target->Active, false);
        rect = target->Rect();
        rect.ClipWith(ImRect(viewport->Pos, viewport->Pos + viewport->Size));
    }
    const std::string path = ofs::util::toUtf8(ofs::util::fromUtf8(directory) / ofs::util::fromUtf8(name));
    IM_CHECK_RETV(path.size() < sizeof(ctx->CaptureArgs->InOutputFile), false);
    ctx->CaptureReset();
    ctx->CaptureArgs->InPadding = 0.f;
    ctx->CaptureArgs->InCaptureRect = rect;
    ImStrncpy(ctx->CaptureArgs->InOutputFile, path.c_str(), sizeof(ctx->CaptureArgs->InOutputFile));
    IM_CHECK_RETV(ctx->CaptureScreenshot(ImGuiCaptureFlags_Instant | ImGuiCaptureFlags_HideMouseCursor), false);
    return std::filesystem::exists(ofs::util::fromUtf8(path)) &&
           std::filesystem::file_size(ofs::util::fromUtf8(path)) > 0;
}
} // namespace

unsigned int registerVisualCapture(ImGuiTestEngine *engine, std::string &outputDirectory) {
    GLuint texture = 0;
    GLint oldTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    const unsigned char background[] = {32, 36, 40, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, background);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture));
    static_cast<ofs::DummyVideoPlayer *>(getTestState().videoPlayer)->setFakeVideoForTesting(1280, 720, texture);
    auto &engineIo = ImGuiTestEngine_GetIO(engine);
    engineIo.ScreenCaptureFunc = captureFramebuffer;
    engineIo.ConfigMouseDrawCursor = false;
    auto *test = IM_REGISTER_TEST(engine, "visual_capture", "migration_baseline");
    test->UserData = &outputDirectory;
    test->TestFunc = [](ImGuiTestContext *ctx) {
        const auto &outputDirectory = *static_cast<const std::string *>(ctx->Test->UserData);
        std::filesystem::create_directories(ofs::util::fromUtf8(outputDirectory));
        loadFixture(ctx);
        auto &project = *getTestState().project;
        auto &events = *getTestState().eventQueue;
        IM_CHECK(project.state.mediaPath.empty());
        events.push(ofs::ModifyEvent<ofs::SimulatorState>{[](ofs::SimulatorState &s) {
            s.use3dSimulator = false;
            s.extraLinesCount = 4;
            s.enablePosition = true;
        }});
        for (int axis = 0; axis < 6; ++axis) {
            ofs::VectorSet<ofs::ScriptAxisAction> actions;
            for (int i = 0; i <= 120; ++i)
                actions.insert({i * 0.5, static_cast<int>(50.0 + 43.0 * std::sin(i * 0.8 + axis * 0.7))});
            events.push(ofs::CommitAxisActionsEvent{.axis = static_cast<ofs::StandardAxis>(axis), .actions = actions});
            events.push(
                ofs::ToggleAxisPanelVisibilityEvent{.axisRole = static_cast<ofs::StandardAxis>(axis), .inPanel = true});
            events.push(
                ofs::ToggleAxisVisibilityEvent{.axisRole = static_cast<ofs::StandardAxis>(axis), .visible = true});
        }
        events.push(ofs::AxisSelectedEvent{ofs::StandardAxis::L0});
        events.push(ofs::SeekEvent{3.0});
        events.push(ofs::SetTimelineZoomEvent{12.0});
        ctx->MenuClick("//##MainMenuBar/###menu_layout/###layout_default");
        ctx->MouseMoveToPos(ImGui::GetMainViewport()->Pos + ImVec2(1.f, 1.f));
        ctx->Yield(3);
        const auto zoomDeadline = SDL_GetTicks() + 2000;
        while (std::abs(project.timelineView.visibleTime - 12.0) > 0.000001) {
            IM_CHECK(SDL_GetTicks() < zoomDeadline);
            ctx->Yield();
        }
        IM_CHECK(saveCapture(ctx, outputDirectory, "01-workspace-2d.png"));
        IM_CHECK(saveCapture(ctx, outputDirectory, "02-timeline-overlay.png", "Timeline###timeline"));
        IM_CHECK(saveCapture(ctx, outputDirectory, "03-titlebar.png", "##AppTitleBar"));
        IM_CHECK(saveCapture(ctx, outputDirectory, "04-video-controls.png", "Video Controls###video_controls"));

        events.push(ofs::SetTimelineLayoutEvent{ofs::TimelineLayout::Lanes});
        events.push(ofs::ModifyEvent<ofs::SimulatorState>{[](ofs::SimulatorState &s) { s.use3dSimulator = true; }});
        ctx->Yield(20);
        IM_CHECK(saveCapture(ctx, outputDirectory, "05-workspace-3d.png"));
        IM_CHECK(saveCapture(ctx, outputDirectory, "06-simulator.png", "Simulator###Simulator"));
        IM_CHECK(saveCapture(ctx, outputDirectory, "07-timeline-lanes.png", "Timeline###timeline"));

        events.push(ofs::CreateRegionEvent{.axisRole = ofs::StandardAxis::L0, .startTime = 0.0, .endTime = 8.0});
        ctx->Yield(3);
        IM_CHECK_EQ(project.regions.size(), 1U);
        auto region = project.regions.front();
        region.name = "Rendering reference";
        auto &graph = region.nodeGraph;
        graph.nodes = {
            {.id = 1, .type = ofs::GraphNodeType::Input, .posX = 20.f, .posY = 40.f},
            {.id = 2, .type = ofs::GraphNodeType::Constant, .constantValue = 12.f, .posX = 20.f, .posY = 240.f},
            {.id = 3, .type = ofs::GraphNodeType::Add, .posX = 290.f, .posY = 90.f},
            {.id = 4, .type = ofs::GraphNodeType::Output, .posX = 650.f, .posY = 110.f}};
        graph.links = {{.id = 5, .fromNode = 1, .toNode = 3},
                       {.id = 6, .fromNode = 2, .toNode = 3, .toPin = 1},
                       {.id = 7, .fromNode = 3, .toNode = 4}};
        graph.nextId = 8;
        events.push(ofs::ModifyRegionEvent{region.id, region});
        ctx->Yield(30);
        IM_CHECK(saveCapture(ctx, outputDirectory, "08-processing-workspace.png"));
        IM_CHECK(saveCapture(ctx, outputDirectory, "09-processing-graph.png", "Processing###video_player"));

        ctx->MenuClick("//##MainMenuBar/###menu_edit/###menu_preferences");
        ctx->Yield(3);
        ctx->WindowResize("Preferences###preferences", ImVec2(760.f, 850.f));
        ctx->WindowMove("Preferences###preferences", ImGui::GetMainViewport()->Pos + ImVec2(120.f, 80.f));
        ctx->Yield(5);
        IM_CHECK(saveCapture(ctx, outputDirectory, "10-preferences.png", "Preferences###preferences"));
        ctx->WindowClose("Preferences###preferences");
        ctx->MenuClick("//##MainMenuBar/###menu_view/###menu_view_log");
        ctx->Yield(5);
        IM_CHECK(saveCapture(ctx, outputDirectory, "11-log-filter.png", "Log###log"));

        const auto &io = ImGui::GetIO();
        const nlohmann::json metadata = {
            {"imgui_version", IMGUI_VERSION},
            {"display_size", {io.DisplaySize.x, io.DisplaySize.y}},
            {"framebuffer_scale", {io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y}},
            {"font_size_base", ImGui::GetStyle().FontSizeBase},
            {"font_scale_dpi", ImGui::GetStyle().FontScaleDpi},
            {"renderer", io.BackendRendererName},
            {"media", "none; synthetic actions and solid-color dummy texture"}};
        std::ofstream manifest(ofs::util::fromUtf8(outputDirectory) / L"manifest.json");
        manifest << metadata.dump(2) << '\n';
        IM_CHECK(manifest.good());
    };
    return texture;
}
