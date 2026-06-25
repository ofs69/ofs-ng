#include "Core/Events.h"
#include "Core/NotifyLevel.h"
#include "UI/Notifications.h"
#include "helpers/TestState.h"
#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName / ImGuiWindow::Active — not in the public API.
#include <imgui_te_context.h>
#include <imgui_te_engine.h>

using namespace ofs;

namespace {
ofs::ui::NotificationState &notif() {
    return *getTestState().notifications;
}

// Push through the real channel: eq.push -> drain() -> OfsApp on<NotifyEvent> -> NotificationState.
void pushNote(NotifyLevel level, const char *msg) {
    getTestState().eventQueue->push(NotifyEvent{.level = level, .message = msg});
}

int logSize() {
    return static_cast<int>(notif().log.size());
}
int toastCount() {
    return static_cast<int>(notif().toasts.size());
}
} // namespace

// UI tests for the footer notification center (bell log + transient toasts). They drive the real app:
// a NotifyEvent flows through drain() into NotificationState, the footer renders the bell/toasts, and
// asserts read back the observable state (log/toasts/unread) plus exercise the bell popup widgets.
void RegisterNotificationsTests(ImGuiTestEngine *e) {
    // Warning + Error are kept in the bell log and counted as unread.
    IM_REGISTER_TEST(e, "notifications", "warning_error_kept_in_log")->TestFunc = [](ImGuiTestContext *ctx) {
        notif().clear();
        pushNote(NotifyLevel::Warning, "a warning");
        pushNote(NotifyLevel::Error, "an error");
        ctx->Yield(2);
        IM_CHECK_EQ(logSize(), 2);
        IM_CHECK_EQ(notif().unread, 2);
    };

    // Info + Success are toast-only: they never enter the bell log and don't bump the unread badge.
    IM_REGISTER_TEST(e, "notifications", "info_success_not_kept")->TestFunc = [](ImGuiTestContext *ctx) {
        notif().clear();
        pushNote(NotifyLevel::Info, "fyi");
        pushNote(NotifyLevel::Success, "done");
        ctx->Yield(2);
        IM_CHECK_EQ(logSize(), 0);
        IM_CHECK_EQ(notif().unread, 0);
    };

    // Every level produces a transient toast, independent of which ones persist in the log.
    IM_REGISTER_TEST(e, "notifications", "all_levels_toast")->TestFunc = [](ImGuiTestContext *ctx) {
        notif().clear();
        notif().toasts.clear();
        pushNote(NotifyLevel::Info, "i");
        pushNote(NotifyLevel::Success, "s");
        pushNote(NotifyLevel::Warning, "w");
        pushNote(NotifyLevel::Error, "e");
        ctx->Yield(2);
        IM_CHECK_EQ(toastCount(), 4); // all four toast
        IM_CHECK_EQ(logSize(), 2);    // but only W/E persist in the bell
    };

    // The toast stack renders as a live floating window while a toast is on screen.
    IM_REGISTER_TEST(e, "notifications", "toast_window_renders")->TestFunc = [](ImGuiTestContext *ctx) {
        notif().clear();
        notif().toasts.clear();
        pushNote(NotifyLevel::Success, "exported script.funscript");
        ctx->Yield(3);
        ImGuiWindow *w = ImGui::FindWindowByName("##toaststack");
        IM_CHECK(w != nullptr);
        IM_CHECK(w->Active); // actually being submitted this frame
    };

    // Clicking the bell opens the popup; Clear all empties the log and resets the unread badge.
    IM_REGISTER_TEST(e, "notifications", "bell_opens_and_clears")->TestFunc = [](ImGuiTestContext *ctx) {
        notif().clear();
        pushNote(NotifyLevel::Error, "boom");
        ctx->Yield(2);
        IM_CHECK_EQ(logSize(), 1);

        ctx->ItemClick("//##AppFooterBar/##bell");
        ctx->Yield(2);
        IM_CHECK(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));

        ctx->ItemClick("**/###clearall");
        ctx->Yield(2);
        IM_CHECK_EQ(logSize(), 0);
        IM_CHECK_EQ(notif().unread, 0);

        // Leave no popup open for the next suite.
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield();
    };
}
