#pragma once

#include "Core/NotifyLevel.h"
#include "Util/RingBuffer.h"
#include <string>
#include <utility>
#include <vector>

namespace ofs::ui {

// A persistent entry in the bell log (Warning/Error only).
struct NotificationItem {
    ofs::NotifyLevel level = ofs::NotifyLevel::Info;
    std::string text;
};

// A transient on-screen toast (every level). shownAt is the ImGui time it first became visible;
// -1 = pushed but not yet shown. The toast layer owns this field (stamps it on first sight, resets
// it to pause on hover, forces it past life to dismiss) and prunes the entry once it expires.
struct ToastItem {
    ofs::NotifyLevel level = ofs::NotifyLevel::Info;
    std::string text;
    double shownAt = -1.0;
};

// Notification state, owned by the caller (OfsApp) — mirrors the CommandPaletteState pattern so the
// renderers keep no global UI state. Two channels, by design:
//   • toasts: transient, EVERY level, auto-expiring — pruned once shown and aged out.
//   • log:    persistent bell history, WARNING/ERROR only (Info/Success are toast-only and not kept),
//             backed by a fixed-capacity RingBuffer so it can't grow unbounded over a session.
struct NotificationState {
    static constexpr size_t kMax = 100;

    ofs::RingBuffer<NotificationItem> log{kMax};
    std::vector<ToastItem> toasts;
    int unread = 0; // unseen bell-log entries (Warning/Error)

    // Published by renderFooterBar each frame so the separate end-of-frame renderToasts() pass can
    // anchor the stack above the bell without re-deriving the footer layout.
    float bellAnchorX = 0.0f; // bell zone right edge, screen px
    float bellAnchorY = 0.0f; // bell zone top edge, screen px
    bool panelOpen = false;   // bell popup open this frame → suppress toasts (they'd overlap it)

    void push(ofs::NotifyLevel level, std::string text) {
        const bool keep = level == ofs::NotifyLevel::Warning || level == ofs::NotifyLevel::Error;
        if (keep) {
            log.push_back({.level = level, .text = text});
            ++unread;
        }
        toasts.push_back({.level = level, .text = std::move(text)});
    }

    void clear() {
        log.clear();
        unread = 0;
    }
};

} // namespace ofs::ui
