#include "ModalManager.h"
#include "Core/EventQueue.h"
#include "Localization/Translator.h"
#include "UI/Icons.h"
#include "UI/ImGuiHelpers.h"
#include "Util/FileUtil.h"
#include "Util/FrameAllocator.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"
#include "imgui.h"
#include <algorithm>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <vector>

// The real SDL dialog path is compiled out under OFS_TEST_ENGINE (the ui-test build), which drives
// dialogs through the override seam instead — so a headless suite never opens an OS dialog and never
// needs the SDL dialog symbols. Every other build (app + unit tests) compiles the SDL path.
#ifndef OFS_TEST_ENGINE
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_video.h>
#endif

namespace ofs {

namespace {
// Severity tints the title text only; the rest of the popup uses the theme defaults.
ImVec4 severityColor(ModalSeverity s) {
    switch (s) {
    case ModalSeverity::Warning:
        return {1.0f, 0.78f, 0.30f, 1.0f};
    case ModalSeverity::Error:
        return {1.0f, 0.45f, 0.40f, 1.0f};
    case ModalSeverity::Info:
    default:
        return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
}

// ── Native dialog + remembered-directory persistence ────────────────────────────────────────
std::filesystem::path dialogPathsFile() {
    return util::getPrefPath() / "dialog_paths.json";
}

std::string resolveDir(const std::unordered_map<std::string, std::string> &dirs, const std::string &key,
                       const std::string &fallback) {
    auto it = dirs.find(key);
    if (it != dirs.end() && !it->second.empty())
        return it->second;
    return fallback;
}

void remember(std::unordered_map<std::string, std::string> &dirs, const std::string &key, const std::string &result) {
    namespace fs = std::filesystem;
    fs::path p = util::fromUtf8(result); // dialog results are UTF-8
    // Store with a trailing slash. SDL splits a dialog's default location at the last slash into
    // dir+filename; a bare directory would be treated as a filename and open the parent. The Save
    // path's own separator handling is a no-op when the slash is already present.
    std::string dir = util::toUtf8(fs::is_directory(p) ? p : p.parent_path());
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir += '/';
    dirs[key] = dir;
}

#ifdef OFS_TEST_ENGINE
// Set on the main thread before a dialog is triggered, read once when the dialog is dispatched. See
// the header. In a test build there is no SDL dialog, so this is the only source of a dialog result.
// The single-result override drives non-multi dialogs; the multi override drives spec.allowMany ones.
std::function<std::string(const FileDialogSpec &, const std::string &)> g_nativeDialogOverride;
std::function<std::vector<std::string>(const FileDialogSpec &, const std::string &)> g_nativeMultiDialogOverride;
#else
// SDL runs each dialog on its own detached thread and invokes this from it, so it must touch nothing
// but the shared slot: it writes the chosen UTF-8 path (empty on cancel/error) and flips `done`,
// which ModalManager::pumpNativeDialog polls on the main thread. The slot is kept alive by a heap
// shared_ptr handed to SDL as userdata — a dialog still open after the ModalManager is destroyed
// writes into a live object, never a UAF. `filelist` is NULL on error and {NULL} on cancel.
void SDLCALL onNativeDialogResult(void *userdata, const char *const *filelist, int /*filterIndex*/) {
    std::unique_ptr<std::shared_ptr<NativeDialogSlot>> held(static_cast<std::shared_ptr<NativeDialogSlot> *>(userdata));
    NativeDialogSlot &slot = **held;
    // `filelist` is NULL on error, {NULL} on cancel, else a NULL-terminated array of UTF-8 paths (one for
    // a non-multi dialog, possibly several when allow_many was set). Copy them all; an empty list == cancel.
    if (filelist)
        for (const char *const *p = filelist; *p; ++p)
            slot.results.emplace_back(*p);
    slot.done.store(true, std::memory_order_release);
}

// Kicks off the native dialog on the main thread (SDL requires it) and returns immediately; the
// dialog runs on SDL's own thread and resolves through onNativeDialogResult. Translates the spec's
// glob patterns + single description into SDL's filter shape.
void dispatchNativeDialog(const FileDialogSpec &spec, const std::string &dir,
                          const std::shared_ptr<NativeDialogSlot> &slot) {
    // SDL splits the default location at the last separator into folder + file name. For the
    // Open/Folder kinds the whole string is the folder, so guarantee a trailing slash; an empty
    // string means "no preferred location" and must be passed as nullptr.
    auto asFolder = [](std::string s) {
        if (!s.empty() && s.back() != '/' && s.back() != '\\')
            s += '/';
        return s;
    };

    // SDL filter patterns are ';'-separated bare extensions ("ofp;funscript"); "*" means all files.
    // The spec's patterns arrive as globs ("*.ofp"), so strip the leading "*." from each.
    std::string joined;
    for (const auto &p : spec.filterPatterns) {
        std::string ext = p.starts_with("*.") ? p.substr(2) : p;
        if (!joined.empty())
            joined += ';';
        joined += ext;
    }
    const SDL_DialogFileFilter filter{.name = spec.filterDesc.c_str(), .pattern = joined.c_str()};
    const SDL_DialogFileFilter *filters = spec.filterPatterns.empty() ? nullptr : &filter;
    const int nfilters = spec.filterPatterns.empty() ? 0 : 1;

    // Parent the dialog to the app window for proper modality/centering (null is tolerated).
    SDL_Window *parent = SDL_GL_GetCurrentWindow();
    // The callback owns this heap shared_ptr copy and frees it; it keeps the slot alive past our scope.
    auto *held = new std::shared_ptr<NativeDialogSlot>(slot);

    switch (spec.kind) {
    case FileDialogKind::Open: {
        const std::string loc = asFolder(dir);
        SDL_ShowOpenFileDialog(onNativeDialogResult, held, parent, filters, nfilters,
                               loc.empty() ? nullptr : loc.c_str(), spec.allowMany);
        break;
    }
    case FileDialogKind::Save: {
        const std::string loc = asFolder(dir) + spec.defaultName;
        SDL_ShowSaveFileDialog(onNativeDialogResult, held, parent, filters, nfilters,
                               loc.empty() ? nullptr : loc.c_str());
        break;
    }
    case FileDialogKind::SelectFolder: {
        const std::string loc = asFolder(dir);
        SDL_ShowOpenFolderDialog(onNativeDialogResult, held, parent, loc.empty() ? nullptr : loc.c_str(), false);
        break;
    }
    }
}
#endif
} // namespace

#ifdef OFS_TEST_ENGINE
void setNativeDialogOverrideForTesting(std::function<std::string(const FileDialogSpec &, const std::string &)> fn) {
    g_nativeDialogOverride = std::move(fn);
}

void setNativeMultiDialogOverrideForTesting(
    std::function<std::vector<std::string>(const FileDialogSpec &, const std::string &)> fn) {
    g_nativeMultiDialogOverride = std::move(fn);
}
#endif

ModalManager::ModalManager(EventQueue &eq) {
    loadDialogDirs();
    // Registration is mandatory: EventQueue silently drops events with no handler, which would
    // leave a flow's awaiter suspended forever. Must run before EventQueue::freeze().
    eq.on<ShowModalEvent>([this](const ShowModalEvent &e) {
        queue_.push_back(Entry{.spec = e.spec,
                               .body = e.body,
                               .severity = e.severity,
                               .width = e.width,
                               .dismissOnClickAway = e.dismissOnClickAway,
                               .stack = e.stack,
                               .dialog = e.dialog,
                               .axisPick = e.axisPick,
                               .handle = e.handle,
                               .resultSlot = e.resultSlot,
                               .resultStrSlot = e.resultStrSlot,
                               .resultStrsSlot = e.resultStrsSlot,
                               .resultAxisPick = e.resultAxisPick});
    });
}

void ModalManager::loadDialogDirs() {
    try {
        auto text = ofs::util::readFile(dialogPathsFile());
        if (text)
            dialogDirs_ = nlohmann::json::parse(*text).get<std::unordered_map<std::string, std::string>>();
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to load dialog_paths.json: {}", e.what());
    }
}

void ModalManager::saveDialogDirs() const {
    try {
        nlohmann::json j = dialogDirs_;
        ofs::util::writeFileAtomic(dialogPathsFile(), j.dump(2));
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to save dialog_paths.json: {}", e.what());
    }
}

ModalManager::~ModalManager() {
    // Tear down any suspended flows cleanly. The coroutines are parked at an awaiter (not at
    // final_suspend), so destroy() is the correct call: it unwinds frame-local RAII.
    for (auto &e : queue_)
        if (e.handle)
            e.handle.destroy();
}

void ModalManager::pump() {
    if (queue_.empty())
        return;

    // A native file dialog occupies the front of the queue while SDL runs it on its own thread. It
    // has its own dispatch/poll/resume cycle and never uses the ImGui pendingResult_ path.
    if (queue_.front().dialog) {
        pumpNativeDialog();
        return;
    }

    if (!pendingResult_)
        return;

    // The answered modal is the stack top (the only interactive one), not necessarily the front. Its
    // index was recorded in render(); the queue is append-only between render() and here, so it's stable.
    // Pop BEFORE resuming: the resumed flow may co_await again and append a fresh entry, and it may
    // self-destroy at co_return, so the handle must not be touched after resume(). Entries below it keep
    // their open latch, so the modal underneath stays open and becomes interactive again next frame.
    const size_t idx = std::min(pendingIndex_, queue_.size() - 1);
    Entry e = std::move(queue_[idx]);
    queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(idx));
    const int result = *pendingResult_;
    pendingResult_.reset();

    if (e.resultSlot)
        *e.resultSlot = result;
    if (e.handle)
        e.handle.resume();
}

void ModalManager::pumpNativeDialog() {
    // Caller (pump) only invokes this when the front entry's dialog is engaged; re-check so the
    // optional access is provably guarded.
    const auto &dialogOpt = queue_.front().dialog;
    if (!dialogOpt)
        return;
    const FileDialogSpec &spec = *dialogOpt;

    if (!nativeDispatched_) {
        // Resolve the remembered directory on the main thread. A forced dir always wins and must
        // exist for the dialog to open there, so create it first.
        std::string dir;
        if (!spec.forceDir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(util::fromUtf8(spec.forceDir), ec);
            dir = spec.forceDir;
        } else {
            dir = resolveDir(dialogDirs_, spec.key, spec.fallbackDir);
        }
        native_ = std::make_shared<NativeDialogSlot>();
#ifdef OFS_TEST_ENGINE
        // Headless: never open a real OS dialog. Fill the slot from the matching test override (or, with
        // none installed, an empty result == cancel) synchronously; the poll below picks it up next frame,
        // mirroring the one-frame latency of the real async path. A multi-select request reads the multi
        // override; everything else reads the single one.
        if (spec.allowMany) {
            native_->results =
                g_nativeMultiDialogOverride ? g_nativeMultiDialogOverride(spec, dir) : std::vector<std::string>{};
        } else if (g_nativeDialogOverride) {
            std::string r = g_nativeDialogOverride(spec, dir);
            if (!r.empty())
                native_->results.push_back(std::move(r));
        }
        native_->done.store(true, std::memory_order_release);
#else
        // SDL runs the dialog on its own thread and resolves the slot from onNativeDialogResult; the
        // slot (held alive by a shared_ptr the callback owns) outlives this ModalManager if needed.
        dispatchNativeDialog(spec, dir, native_);
#endif
        nativeDispatched_ = true;
        return;
    }

    if (!native_->done.load(std::memory_order_acquire))
        return; // still open; busy() (queue non-empty) keeps the app awake until it resolves

    // Pop BEFORE resuming (same reasoning as pump()): the resumed flow may queue another modal.
    Entry e = std::move(queue_.front());
    queue_.pop_front();
    std::vector<std::string> results = std::move(native_->results);
    native_.reset();
    nativeDispatched_ = false;

    // The remembered directory tracks the first chosen path (a multi-select shares one folder).
    std::string first = results.empty() ? std::string() : results.front();
    // A forced-dir dialog deliberately doesn't persist where the user ended up — it always reopens
    // at its fixed location next time.
    if (!first.empty() && e.dialog && e.dialog->forceDir.empty()) {
        remember(dialogDirs_, e.dialog->key, first);
        saveDialogDirs();
    }
    if (e.resultStrSlot)
        *e.resultStrSlot = std::move(first);
    if (e.resultStrsSlot)
        *e.resultStrsSlot = std::move(results);
    if (e.handle)
        e.handle.resume();
}

void ModalManager::render() {
    if (queue_.empty())
        return;
    // Native file dialogs are OS windows owned by SDL on its own thread — nothing to draw here.
    if (queue_.front().dialog)
        return;
    renderEntry(0);
}

void ModalManager::finishAxisPick(Entry &e, AxisPickResult::Outcome outcome, size_t index) {
    if (e.resultAxisPick) {
        e.resultAxisPick->outcome = outcome;
        e.resultAxisPick->rows = e.axisPickRow;
        e.resultAxisPick->axis = e.axisPickSel;
    }
    pendingResult_ = 0;
    pendingIndex_ = index;
    ImGui::CloseCurrentPopup();
}

void ModalManager::renderEntry(size_t index) {
    Entry &e = queue_[index]; // non-const: the axis-picker path mutates `selections` across frames

    // ImGui hashes only the text after "###", so every modal sharing "###ofsmodal" would collide once two
    // are open at once (a stacked child over its parent). The front keeps the legacy id (UI tests address
    // "###ofsmodal"); stacked entries get a depth suffix for a distinct popup id. Index is stable per entry
    // while a stack is live (a child never outlives its parent), so the id is stable across frames.
    const char *id =
        index == 0 ? fmtScratch("{}###ofsmodal", e.spec.title) : fmtScratch("{}###ofsmodal{}", e.spec.title, index);
    if (!e.opened) {
        ImGui::OpenPopup(id);
        e.opened = true;
    }

    // Center on the main viewport (the body modals were previously centered on their owning
    // window; there is no owner-window context here, so the viewport is the unified anchor).
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    // Font-relative shell widths so they track DPI; body modals carry their own (font-relative) width.
    const float fs = ImGui::GetFontSize();
    const float width = e.axisPick ? fs * 30.0f : (e.body ? e.width : fs * 26.25f);
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Appearing);
    // AlwaysAutoResize sizes the window to its content every frame. A body/axis-pick modal whose
    // content has no intrinsic width — e.g. a 2-column form whose value column is WidthStretch — would
    // otherwise collapse below the intended width, shrinking a little more each frame. Pin the requested
    // width as a *minimum* so the modal still grows for a longer translation but never shrinks below it.
    // Message boxes (no body) keep hugging their content.
    if (e.body || e.axisPick)
        ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    // NoTitleBar: the severity-tinted title is drawn in the body below. A native title bar would
    // repeat that text (its label is the part of `id` before "###") and carries no close button.
    // A click-away entry (the footer tool-options popup) renders as a NON-modal popup: no dimmed
    // backdrop, the rest of the UI stays live, and ImGui closes it on a genuine click-away while keeping
    // it open as the user drives its widgets (and any nested combos). Every other modal stays a true
    // BeginPopupModal. Same id/flags either way.
    const bool flyout = e.body && e.dismissOnClickAway;
    const ImGuiWindowFlags popupFlags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar;
    if (flyout ? ImGui::BeginPopup(id, popupFlags) : ImGui::BeginPopupModal(id, nullptr, popupFlags)) {
        if (e.axisPick) {
            // Editable import picker: one combo per row, plus per-row remove and an add-file button.
            // `axisPickSel`/`axisPickRow` (init once from the spec) hold the live, shrinking state; on a
            // user action they are copied into resultAxisPick with the chosen outcome. Every route sets
            // pendingResult_ so pump() pops + resumes the awaiting flow.
            const AxisPickSpec &ap = *e.axisPick;
            if (!e.axisPickInit) {
                e.axisPickSel = ap.defaults;
                e.axisPickRow.resize(ap.rows.size());
                for (size_t i = 0; i < ap.rows.size(); ++i)
                    e.axisPickRow[i] = static_cast<int>(i);
                e.axisPickInit = true;
            }

            ImGui::PushStyleColor(ImGuiCol_Text, severityColor(e.severity));
            ImGui::TextUnformatted(ap.title.c_str());
            ImGui::PopStyleColor();
            ImGui::Separator();
            if (!ap.prompt.empty()) {
                ImGui::TextWrapped("%s", ap.prompt.c_str());
                ImGui::Separator();
            }
            // Source labels are file/track names that can be arbitrarily long, so cap the label column
            // (font-relative) and elide to it — the full name shows on hover. The arrow column then sits
            // at a stable offset regardless of name length.
            const float maxLabelCol = ImGui::GetFontSize() * 10.0f;
            float labelColW = 0.0f;
            for (int rid : e.axisPickRow)
                labelColW = std::max(labelColW, ImGui::CalcTextSize(ap.rows[rid].c_str()).x);
            labelColW = std::min(labelColW, maxLabelCol);
            const float trashW = ImGui::CalcTextSize(ICON_TRASH).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            // Leave a clear gap between the (possibly elided) label and the arrow column.
            const float arrowColX = labelColW + ImGui::GetStyle().ItemSpacing.x * 2.0f;
            int removeRow = -1;
            for (size_t r = 0; r < e.axisPickRow.size(); ++r) {
                const std::string &full = ap.rows[e.axisPickRow[r]];
                // Align the row's text to the framed widgets (combo/button) so it sits vertically centered
                // rather than at the top of the taller row.
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(ui::elide(full.c_str(), labelColW));
                ImGui::SetItemTooltip("%s", full.c_str());
                ImGui::SameLine(arrowColX);
                ImGui::TextUnformatted(GLYPH_ARROW_RIGHT);
                ImGui::SameLine();
                // Combo fills the row but leaves room for the trailing remove button.
                const float comboW = std::max(ImGui::GetFontSize() * 4.0f, ImGui::GetContentRegionAvail().x - trashW -
                                                                               ImGui::GetStyle().ItemSpacing.x);
                ImGui::SetNextItemWidth(comboW);
                const int sel = e.axisPickSel[r];
                const char *preview =
                    (sel >= 0 && sel < static_cast<int>(ap.options.size())) ? ap.options[sel].c_str() : "";
                // Row index baked into the ###id so UI tests can address each combo (ComboClick splits
                // the ref at the first '/', so the id must contain none).
                if (ImGui::BeginCombo(fmtScratch("###axispick{}", r), preview)) {
                    for (size_t i = 0; i < ap.options.size(); ++i) {
                        const bool chosen = e.axisPickSel[r] == static_cast<int>(i);
                        if (ImGui::Selectable(ap.options[i].c_str(), chosen))
                            e.axisPickSel[r] = static_cast<int>(i);
                        if (chosen)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::Button(fmtScratch("{}###axispick_remove{}", ICON_TRASH, r)))
                    removeRow = static_cast<int>(r);
                ImGui::SetItemTooltip("%s", Str::PmImportRemoveRow.c_str());
            }
            if (removeRow >= 0 && removeRow < static_cast<int>(e.axisPickRow.size())) {
                e.axisPickRow.erase(e.axisPickRow.begin() + removeRow);
                e.axisPickSel.erase(e.axisPickSel.begin() + removeRow);
            }

            if (ImGui::Button(fmtScratch("{}###axispick_add", Str::PmImportAddFile.icon(ICON_PLUS))))
                finishAxisPick(e, AxisPickResult::Outcome::AddFile, index);

            // Two rows targeting the same axis would silently overwrite each other, so block import until
            // every target is distinct.
            bool dup = false;
            for (size_t a = 0; a < e.axisPickSel.size() && !dup; ++a)
                for (size_t b = a + 1; b < e.axisPickSel.size(); ++b)
                    if (e.axisPickSel[b] == e.axisPickSel[a]) {
                        dup = true;
                        break;
                    }
            if (dup) {
                ImGui::PushStyleColor(ImGuiCol_Text, severityColor(ModalSeverity::Error));
                ImGui::TextWrapped("%s", Str::ModalDuplicateAxis.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::Separator();
            ImGui::BeginDisabled(dup || e.axisPickRow.empty());
            if (ImGui::Button(Str::ModalImport.id("modalbtn0")))
                finishAxisPick(e, AxisPickResult::Outcome::Confirm, index);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button(Str::ModalCancel.id("modalbtn1")) || ImGui::IsKeyPressed(ImGuiKey_Escape))
                finishAxisPick(e, AxisPickResult::Outcome::Cancel, index);
        } else if (e.body) {
            // Custom-body path: the manager draws the severity title; the body draws the rest and
            // returns true to close. A sentinel result drives the same pop/resume path as buttons
            // (handle/resultSlot are null for fire-and-forget bodies, so pump() just pops). A flyout
            // body (dismissOnClickAway) is closed by ImGui's native click-away/Escape instead — caught
            // after EndPopup below — so it need not return true itself.
            ImGui::PushStyleColor(ImGuiCol_Text, severityColor(e.severity));
            ImGui::TextUnformatted(e.spec.title.c_str());
            ImGui::PopStyleColor();
            ImGui::Separator();
            if (e.body()) {
                pendingResult_ = 0;
                pendingIndex_ = index;
                ImGui::CloseCurrentPopup();
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, severityColor(e.spec.severity));
            ImGui::TextUnformatted(e.spec.title.c_str());
            ImGui::PopStyleColor();
            ImGui::Separator();
            ImGui::TextWrapped("%s", e.spec.message.c_str());
            ImGui::Separator();

            // The visible label is data (varies per modal), so pin a stable ###id by button index
            // — UI tests address buttons by ###modalbtn<i>, never by their display text.
            for (size_t i = 0; i < e.spec.buttons.size(); ++i) {
                if (i != 0)
                    ImGui::SameLine();
                if (ImGui::Button(fmtScratch("{}###modalbtn{}", e.spec.buttons[i], i))) {
                    pendingResult_ = static_cast<int>(i);
                    pendingIndex_ = index;
                    ImGui::CloseCurrentPopup();
                }
            }
            // Escape maps to the last button (the cancel/dismiss slot by convention).
            if (!e.spec.buttons.empty() && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                pendingResult_ = static_cast<int>(e.spec.buttons.size() - 1);
                pendingIndex_ = index;
                ImGui::CloseCurrentPopup();
            }
        }

        // Stacked child: render the next entry nested inside this popup's scope so ImGui draws it on top
        // (dimming this one) and routes input to it. Its OpenPopup must be issued from within this Begin
        // scope, which the recursion does. Dialog entries never stack — they own the queue front instead.
        if (index + 1 < queue_.size() && queue_[index + 1].stack && !queue_[index + 1].dialog)
            renderEntry(index + 1);

        ImGui::EndPopup();
    }

    // A non-modal flyout closes itself on click-away/Escape (ImGui pops it from the popup stack), which
    // never runs the CloseCurrentPopup→pendingResult_ path above. Catch that here so pump() still pops the
    // queue entry (resuming nothing — flyout bodies are fire-and-forget). e.opened guards against the
    // pre-open frame; the !pendingResult_ guard avoids double-setting when the body self-closed.
    if (flyout && e.opened && !pendingResult_ && !ImGui::IsPopupOpen(id)) {
        pendingResult_ = 0;
        pendingIndex_ = index;
    }
}

} // namespace ofs
