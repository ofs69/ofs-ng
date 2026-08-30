#include "UI/WebSocketApiWindow.h"

#include "Core/EventQueue.h"
#include "Core/Events.h"
#include "Format/AppSettings.h"
#include "Localization/Translator.h"
#include "Services/WebSocketApi.h"
#include "UI/Theme.h"
#include "Util/FrameAllocator.h"

#include <algorithm>
#include <imgui.h>

namespace ofs {

void WebSocketApiWindow::render(bool &open, const AppSettings &settings, const WebSocketApiStatus &status,
                                EventQueue &eq) {
    if (!open)
        return;
    if (!ImGui::Begin(Str::WsTitle.id("websocket_api"), &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    bool enabled = settings.webSocketServerEnabled;
    if (ImGui::Checkbox(Str::WsServerActive.id("ws_server_active"), &enabled))
        eq.push(ModifyEvent<AppSettings>{[enabled](AppSettings &s) { s.webSocketServerEnabled = enabled; }});

    int port = settings.webSocketPort;
    ImGui::BeginDisabled(enabled);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    if (ImGui::InputInt(Str::WsPort.id("ws_port"), &port, 1, 100)) {
        port = std::clamp(port, 1, 65535);
        eq.push(ModifyEvent<AppSettings>{[port](AppSettings &s) { s.webSocketPort = port; }});
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextUnformatted(Str::WsEndpoint.fmt(fmtScratch("{}", settings.webSocketPort)));
    if (status.running) {
        ImGui::TextColored(theme::GetStyleColorVec4(AppCol_Success), "%s",
                           Str::WsListening.fmt(fmtScratch("{}", status.clientCount)));
    } else if (enabled && !status.error.empty()) {
        ImGui::TextColored(theme::GetStyleColorVec4(AppCol_Error), "%s", Str::WsError.fmt(status.error));
    } else {
        ImGui::TextDisabled("%s", Str::WsStopped.c_str());
    }

    ImGui::End();
}

} // namespace ofs
