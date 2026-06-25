#pragma once

#include "Core/EventQueue.h"
#include "Localization/Translator.h"
#include "Util/Coro.h"
#include <coroutine>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ofs {

enum class ModalSeverity { Info, Warning, Error };

enum class FileDialogKind { Open, Save, SelectFolder };

// Describes a native (SDL) file/folder dialog. Carried by value through ShowModalEvent so the
// ModalManager — not the call site — owns the dialog (SDL runs it on its own thread) and the per-key
// remembered-directory persistence. Plain data, no imgui: services raise these too.
struct FileDialogSpec {
    FileDialogKind kind = FileDialogKind::Open;
    bool allowMany = false;                  // Open only: let the user pick several files (FileDialogMulti)
    std::string key;                         // persistence key for the last-used directory
    std::string title;                       // dialog window title
    std::string defaultName;                 // Save only: suggested file name
    std::vector<std::string> filterPatterns; // e.g. {"*.ofp"}; rebuilt to const char*[] on the worker
    std::string filterDesc;                  // human label for the filter set
    std::string fallbackDir;                 // initial dir when nothing is remembered for `key`
    // When set, this directory is ALWAYS the initial location, overriding both the remembered `key`
    // dir and `fallbackDir`, and the chosen result is not remembered. ModalManager creates it if
    // missing. Use for a file kind that only makes sense in one fixed place (e.g. translation
    // overrides, which the loader reads only from <pref>/lang).
    std::string forceDir;
};

// A modal popup request. `buttons` are rendered left-to-right; the awaiter returns the index
// of the clicked one. For a fire-and-forget message use a single button (e.g. {"OK"}).
struct ModalSpec {
    std::string title;
    std::string message;
    std::vector<std::string> buttons;
    ModalSeverity severity = ModalSeverity::Info;
};

// The funscript-import picker: an editable list of (source track → target axis) rows. Each row has a
// combo over the shared `options` list; rows can be removed, and a file can be added (which ends the
// modal with Outcome::AddFile so the flow can run a native picker and re-show with the new track). Kept
// imgui-free (plain data); ModalManager renders the combos and the add/remove controls. `options[i]` is
// the label for axis index i, so a chosen index IS the axis enum. `rows[i]` is the FULL source label —
// the renderer elides it to fit and shows the full text on hover.
struct AxisPickSpec {
    std::string title;
    std::string prompt;               // explanatory line above the rows
    std::vector<std::string> options; // shared combo options (axis display names, index == axis enum)
    std::vector<std::string> rows;    // full source label per row (elided for display, full on hover)
    std::vector<int> defaults;        // initial axis per row (size == rows.size())
};

// What the user did with the import picker. `rows`/`axis` are parallel and cover only the rows still
// present when they acted: `rows[k]` is the surviving row's index into the spec's original `rows`, and
// `axis[k]` is the axis chosen for it. Confirm applies them; AddFile re-shows after appending a track
// (so removals must be honored on AddFile too); Cancel imports nothing.
struct AxisPickResult {
    enum class Outcome { Cancel, Confirm, AddFile };
    Outcome outcome = Outcome::Cancel;
    std::vector<int> rows; // surviving row indices into the spec's original rows
    std::vector<int> axis; // chosen axis per surviving row
};

// A custom-bodied modal. The ModalManager owns the chrome (OpenPopup latch, stable id, width,
// centering, severity title, FIFO serialization, shutdown teardown); `body` renders the popup
// interior each frame and returns true when the modal should close. Kept imgui-free on purpose
// — `width` is a float, not an ImVec2 — so services can raise modals without pulling in imgui.
struct CustomModalSpec {
    std::string title;                            // base of the "{}###ofsmodal" id
    ModalSeverity severity = ModalSeverity::Info; // title tint
    float width = 420.0f;                         // SetNextWindowSize x (Appearing); 0 => auto
    std::function<bool()> body;                   // render interior; return true to close
    // When true this body renders as a NON-modal popup: no dimmed backdrop, the rest of the UI stays
    // live, and ImGui closes it on a click-away or Escape (a flyout, not a decision the user must
    // answer) — used for the footer tool-options popup. The body need not return true; ImGui owns the
    // dismissal, so it stays open while the user drives its widgets.
    bool dismissOnClickAway = false;
};

// The one channel from a flow (or any caller) to the ModalManager. Copyable on purpose:
// EventQueue wraps events in std::function, which requires copy-constructibility. The handle
// and resultSlot are non-owning views into the suspended coroutine frame, which stays alive
// until the ModalManager resumes it — so this event must never be duplicated in flight.
// handle/resultSlot both null => a fire-and-forget message (ModalManager resumes nothing).
// When `body` is set, the ModalManager renders it instead of the spec's message+buttons; the
// title still lives in spec.title so the popup id logic is identical for both paths.
struct ShowModalEvent {
    ModalSpec spec;
    std::function<bool()> body;                   // when set, render this instead of spec
    ModalSeverity severity = ModalSeverity::Info; // title tint for the body path
    float width = 420.0f;                         // width for the body path
    bool dismissOnClickAway = false;              // body path: render as a non-modal click-away popup
    // When true, this modal stacks ON TOP of the one already open instead of queueing behind it (FIFO).
    // The underlying modal stays visible (dimmed) and resumes once this one is answered — used to raise a
    // confirmation from inside a long-lived custom-body modal (e.g. the optimize dialog's warnings).
    bool stack = false;
    std::optional<FileDialogSpec> dialog; // when set, a native file dialog (no spec/body render)
    std::optional<AxisPickSpec> axisPick; // when set, render the combined axis picker (no spec/body)
    std::coroutine_handle<> handle{};
    int *resultSlot = nullptr;
    std::string *resultStrSlot = nullptr;               // result slot for the native-dialog (FileDialog) awaiter
    std::vector<std::string> *resultStrsSlot = nullptr; // result slot for the multi-select (FileDialogMulti) awaiter
    AxisPickResult *resultAxisPick = nullptr;           // result slot for the AxisPick awaiter
};

// co_await Confirm{eq, spec} — suspends the flow, shows the modal, resumes with the index of
// the button the user clicked. A typed custom-body awaiter (a flow awaiting a CustomModalSpec
// and getting a result back) is not needed today, but the event already carries handle +
// resultSlot + body, so one can be added later without touching the ModalManager.
struct Confirm {
    EventQueue &eq;
    ModalSpec spec;
    bool stack = false; // stack on top of an already-open modal instead of queueing behind it
    int result = -1;

    // User-declared so this stays a non-aggregate (keeps brace-init at call sites lint-clean).
    Confirm(EventQueue &eq, ModalSpec spec, bool stack = false) : eq(eq), spec(std::move(spec)), stack(stack) {}

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        eq.push(ShowModalEvent{.spec = std::move(spec), .stack = stack, .handle = h, .resultSlot = &result});
    }
    int await_resume() const noexcept { return result; }
};

// co_await FileDialog{eq, spec} — suspends the flow, runs the native SDL dialog on SDL's own thread
// (the frame loop keeps rendering), and resumes with the chosen path as UTF-8 ("" if the user
// cancelled). The ModalManager owns the dialog lifecycle and the remembered-directory store.
struct FileDialog {
    EventQueue &eq;
    FileDialogSpec spec;
    std::string result;

    // User-declared so this stays a non-aggregate (keeps brace-init at call sites lint-clean).
    FileDialog(EventQueue &eq, FileDialogSpec spec) : eq(eq), spec(std::move(spec)) {}

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        eq.push(ShowModalEvent{.dialog = std::move(spec), .handle = h, .resultStrSlot = &result});
    }
    std::string await_resume() { return std::move(result); }
};

// co_await FileDialogMulti{eq, spec} — the multi-select sibling of FileDialog: runs a native Open dialog
// that lets the user pick several files and resumes with the chosen paths (empty if the user cancelled).
// Forces spec.kind = Open / spec.allowMany = true (the only shape the multi result makes sense for).
struct FileDialogMulti {
    EventQueue &eq;
    FileDialogSpec spec;
    std::vector<std::string> result;

    // User-declared so this stays a non-aggregate (keeps brace-init at call sites lint-clean).
    FileDialogMulti(EventQueue &eq, FileDialogSpec spec) : eq(eq), spec(std::move(spec)) {}

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        spec.kind = FileDialogKind::Open;
        spec.allowMany = true;
        eq.push(ShowModalEvent{.dialog = std::move(spec), .handle = h, .resultStrsSlot = &result});
    }
    std::vector<std::string> await_resume() { return std::move(result); }
};

// co_await AxisPick{eq, spec} — suspends the flow, shows the editable import picker, and resumes with an
// AxisPickResult (the user's outcome plus the surviving rows and their chosen axes). ModalManager renders
// the combos / add / remove controls and writes the result; the chrome/pop/resume path is shared with
// Confirm.
struct AxisPick {
    EventQueue &eq;
    AxisPickSpec spec;
    AxisPickResult result;

    // User-declared so this stays a non-aggregate (keeps brace-init at call sites lint-clean).
    AxisPick(EventQueue &eq, AxisPickSpec spec) : eq(eq), spec(std::move(spec)) {}

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        eq.push(ShowModalEvent{.axisPick = std::move(spec), .handle = h, .resultAxisPick = &result});
    }
    AxisPickResult await_resume() { return std::move(result); }
};

// Fire-and-forget file dialog for non-coroutine call sites (UI buttons, plain event handlers).
// `onResult` runs on resume (main thread) with the chosen path, or "" when cancelled — the
// callback decides what an empty path means (mirrors the old `if (result) { ... }`).
inline co::Fire pickFile(EventQueue &eq, FileDialogSpec spec, std::function<void(std::string)> onResult) {
    std::string path = co_await FileDialog{eq, std::move(spec)};
    onResult(std::move(path));
}

// Fire-and-forget multi-select open dialog for non-coroutine call sites. `onResult` runs on resume
// (main thread) with the chosen paths, or an empty list when the user cancelled.
inline co::Fire pickFiles(EventQueue &eq, FileDialogSpec spec, std::function<void(std::vector<std::string>)> onResult) {
    std::vector<std::string> paths = co_await FileDialogMulti{eq, std::move(spec)};
    onResult(std::move(paths));
}

// Fire-and-forget confirmation for non-coroutine call sites. Shows `spec` and runs `onResult` on resume
// (main thread) with the index of the clicked button (-1 if the modal was dismissed without a choice).
inline co::Fire confirmAsync(EventQueue &eq, ModalSpec spec, std::function<void(int)> onResult, bool stack = false) {
    int idx = co_await Confirm{eq, std::move(spec), stack};
    onResult(idx);
}

// Fire-and-forget popup. Returns immediately; the modal renders next frame.
inline void showMessage(EventQueue &eq, ModalSpec spec) {
    eq.push(ShowModalEvent{.spec = std::move(spec), .handle = {}, .resultSlot = nullptr});
}

// Fire-and-forget custom-bodied modal. The body owns its own state and side effects (it pushes
// its own events / drives the work); ModalManager only renders the chrome around it.
inline void showCustomModal(EventQueue &eq, CustomModalSpec spec) {
    eq.push(ShowModalEvent{.spec = ModalSpec{.title = std::move(spec.title)},
                           .body = std::move(spec.body),
                           .severity = spec.severity,
                           .width = spec.width,
                           .dismissOnClickAway = spec.dismissOnClickAway,
                           .handle = {},
                           .resultSlot = nullptr});
}

// Convenience single-"OK" popups for the common severities.
inline void showInfo(EventQueue &eq, std::string title, std::string message) {
    showMessage(eq, ModalSpec{.title = std::move(title),
                              .message = std::move(message),
                              .buttons = {Str::ModalOk.c_str()},
                              .severity = ModalSeverity::Info});
}
inline void showWarning(EventQueue &eq, std::string title, std::string message) {
    showMessage(eq, ModalSpec{.title = std::move(title),
                              .message = std::move(message),
                              .buttons = {Str::ModalOk.c_str()},
                              .severity = ModalSeverity::Warning});
}
inline void showError(EventQueue &eq, std::string title, std::string message) {
    showMessage(eq, ModalSpec{.title = std::move(title),
                              .message = std::move(message),
                              .buttons = {Str::ModalOk.c_str()},
                              .severity = ModalSeverity::Error});
}

} // namespace ofs
