#pragma once

#include "Core/NotifyLevel.h"
#include "Util/RingBuffer.h"
#include <cstdint>
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

// A persistent indicator for an in-flight background job (waveform extraction, …). Unlike a toast it
// does not auto-expire: the producing service removes it (TaskEndedEvent) when the job ends. `progress`
// < 0 renders an indeterminate animated bar; 0..1 a determinate one. `id` is the producer-minted handle
// the cancel button and progress updates key off.
struct TaskItem {
    uint32_t id = 0;
    std::string label;      // e.g. "Audio waveform"
    std::string detail;     // optional sub-line: phase / ETA
    float progress = -1.0f; // < 0 indeterminate, else 0..1
    bool cancellable = false;
    bool hidden = false; // minimized into the bell: no floating panel, listed in the bell popup instead
};

// Notification state, owned by the caller (OfsApp) — mirrors the CommandPaletteState pattern so the
// renderers keep no global UI state. Three channels, by design:
//   • toasts: transient, EVERY level, auto-expiring — pruned once shown and aged out.
//   • log:    persistent bell history, WARNING/ERROR only (Info/Success are toast-only and not kept),
//             backed by a fixed-capacity RingBuffer so it can't grow unbounded over a session.
//   • tasks:  persistent in-flight background jobs, removed by their producer (never auto-expire).
struct NotificationState {
    static constexpr size_t kMax = 100;

    ofs::RingBuffer<NotificationItem> log{kMax};
    std::vector<ToastItem> toasts;
    std::vector<TaskItem> tasks;
    int unread = 0; // unseen bell-log entries (Warning/Error)

    // Published by renderFooterBar each frame so the separate end-of-frame renderToasts() pass can
    // anchor the stack above the bell without re-deriving the footer layout.
    float bellAnchorX = 0.0f; // bell zone right edge, screen px
    float bellAnchorY = 0.0f; // bell zone top edge, screen px
    // Total height (px) the task stack occupied this frame, published by renderTasks so renderToasts can
    // float above it. 0 when no tasks are running, so the toast stack sits on the bell as before.
    float taskStackHeight = 0.0f;
    bool panelOpen = false; // bell popup open this frame → suppress toasts (they'd overlap it)

    void push(ofs::NotifyLevel level, std::string text) {
        const bool keep = level == ofs::NotifyLevel::Warning || level == ofs::NotifyLevel::Error;
        if (keep) {
            log.push_back({.level = level, .text = text});
            ++unread;
        }
        toasts.push_back({.level = level, .text = std::move(text)});
    }

    void startTask(uint32_t id, std::string label, bool cancellable) {
        tasks.push_back({.id = id, .label = std::move(label), .cancellable = cancellable});
    }
    void updateTask(uint32_t id, std::string detail, float progress) {
        for (auto &t : tasks)
            if (t.id == id) {
                t.detail = std::move(detail);
                t.progress = progress;
                return;
            }
    }
    void endTask(uint32_t id) {
        std::erase_if(tasks, [id](const TaskItem &t) { return t.id == id; });
    }

    void clear() {
        log.clear();
        unread = 0;
    }
};

} // namespace ofs::ui
