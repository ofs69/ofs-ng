#pragma once

#include "Modals.h"
#include <atomic>
#include <coroutine>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ofs {

class EventQueue;

// Result hand-off for the in-flight native file dialog. SDL runs each dialog on its own detached
// thread and invokes our callback from it; the callback writes `results` (the chosen UTF-8 paths,
// empty on cancel — at most one for a non-multi dialog) and flips `done`, which
// ModalManager::pumpNativeDialog polls on the main thread. Held by a std::shared_ptr whose copy the
// SDL callback owns, so a dialog still open when the ModalManager is destroyed writes into a live
// object, never a UAF.
struct NativeDialogSlot {
    std::atomic<bool> done{false};
    std::vector<std::string> results;
};

// Renders the app's ImGui modal popups and bridges them to the coroutine flows that raised
// them. Owned by OfsApp. One modal is shown at a time (FIFO); a flow awaiting a Confirm is
// resumed one frame after the user clicks, in the update phase — never during render, so
// resumed business logic / ScriptProject mutations stay out of the render pass.
//
// MUST be destroyed before any service whose flow coroutines it may hold suspended, and before
// any window/service/project a custom-modal body captures: its destructor calls handle.destroy()
// on suspended frames (running their RAII) and frees the body std::functions (and their
// captures). So everything those frames/bodies reference must still be alive. In OfsApp this is
// guaranteed by declaring the ModalManager last among members (destructed first).
//
// It also owns the app's native file dialogs (FileDialogSpec entries): SDL runs each dialog on its
// own thread so the frame loop keeps rendering, and the per-key remembered directory
// (dialog_paths.json) is persisted here.
class ModalManager {
  public:
    explicit ModalManager(EventQueue &eq);
    ~ModalManager();

    ModalManager(const ModalManager &) = delete;
    ModalManager &operator=(const ModalManager &) = delete;

    // Resume the flow whose modal was answered last frame. Call in onUpdate right after
    // EventQueue::drain(), before service updates.
    void pump();

    // Render the front modal. Call last in onImGuiRender so it stacks above the UI.
    void render();

    // True while a modal is queued or a click is awaiting its resume. The app must not idle
    // (block in waitEvents) while busy, or the click->resume frame stalls until the next input.
    [[nodiscard]] bool busy() const { return !queue_.empty() || pendingResult_.has_value(); }

  private:
    struct Entry {
        ModalSpec spec;
        std::function<bool()> body; // when set, rendered instead of spec's message+buttons
        ModalSeverity severity;
        float width;
        bool dismissOnClickAway = false;      // body path: render as a non-modal click-away popup (no dim)
        bool stack = false;                   // render nested on top of the entry before it, not queued behind
        bool opened = false;                  // latches ImGui::OpenPopup once for this entry
        std::optional<FileDialogSpec> dialog; // when set, a native file dialog (no spec/body render)
        std::optional<AxisPickSpec> axisPick; // when set, render the combined axis picker
        std::coroutine_handle<> handle;
        int *resultSlot = nullptr;
        std::string *resultStrSlot = nullptr;
        std::vector<std::string> *resultStrsSlot = nullptr;
        AxisPickResult *resultAxisPick = nullptr;
        // Live editable state for the axisPick path, parallel and shrinking as rows are removed:
        // `axisPickSel[r]` is row r's chosen axis, `axisPickRow[r]` its index into the spec's original
        // rows. `axisPickInit` latches the one-time init from the spec (size can't be used — it shrinks).
        std::vector<int> axisPickSel;
        std::vector<int> axisPickRow;
        bool axisPickInit = false;
    };

    // Render entry `i` and, nested inside it, any contiguous run of stack==true entries above it. Only the
    // topmost is interactive (ImGui blocks the ones below); a resolved entry records its index in
    // pendingIndex_ for pump() to pop. Recursion depth == modal-stack depth (1 in the common case).
    void renderEntry(size_t i);

    // Record the import picker's outcome (plus its surviving rows + chosen axes) into the awaiting
    // flow's result slot and arm the pop/resume path. Must be called inside the popup's Begin/End scope.
    void finishAxisPick(Entry &e, AxisPickResult::Outcome outcome, size_t index);

    // Dispatch / poll / resume the native dialog at the front of the queue. Called from pump().
    void pumpNativeDialog();
    void loadDialogDirs();
    void saveDialogDirs() const;

    std::deque<Entry> queue_;
    std::optional<int> pendingResult_; // result of the answered modal; set in render(), consumed in pump()
    size_t pendingIndex_ = 0;          // queue index of the entry that pendingResult_ belongs to (the stack top)

    std::unordered_map<std::string, std::string> dialogDirs_; // key -> last-used directory (persisted)
    std::shared_ptr<NativeDialogSlot> native_;                // result of the dispatched dialog, if any
    bool nativeDispatched_ = false;                           // the front dialog has been handed to SDL
};

#ifdef OFS_TEST_ENGINE
// Test seam: replace the real SDL dialog with a canned result so UI tests can drive the full
// native-dialog cycle (dispatch -> poll -> resume the awaiting flow -> remember the directory)
// without a real OS dialog. In a test build the override fills the result slot synchronously on the
// main thread (no SDL dialog is opened); pass nullptr to restore real dialogs. Set it before
// triggering a dialog and clear it after.
void setNativeDialogOverrideForTesting(std::function<std::string(const FileDialogSpec &, const std::string &dir)> fn);

// Multi-select sibling of the seam above, for FileDialogMulti / openFilesDialog: returns the full list
// of chosen paths (empty == cancelled). Used only when spec.allowMany is set; non-multi dialogs keep
// using setNativeDialogOverrideForTesting. Set before triggering a dialog, pass nullptr to restore.
void setNativeMultiDialogOverrideForTesting(
    std::function<std::vector<std::string>(const FileDialogSpec &, const std::string &dir)> fn);
#endif

} // namespace ofs
