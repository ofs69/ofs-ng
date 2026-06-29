#pragma once

#include "Core/CustomCommand.h"
#include "Services/BindingSystem.h"
#include "Services/CommandRegistry.h"
#include <imgui.h>
#include <string>
#include <vector>

namespace ofs {

class EventQueue;
class CustomCommandStore;
class CustomCommandTemplateRegistry;
struct AppSettings;

class ShortcutWindow {
  public:
    ShortcutWindow(CommandRegistry &commandRegistry, BindingSystem &bindingSystem,
                   const CustomCommandStore &customStore, const CustomCommandTemplateRegistry &templates);
    void render(bool &open, EventQueue &eq, const AppSettings &appSettings);

  public:
    static const char *formatKeyChord(const KeyChord &kc);

  private:
    // Renders the capture modal's interior; returns true when it should close. Driven by
    // bindingSystem_.rebindState() — an empty targetCommandId means dismissed. On Apply it pushes an
    // ApplyBindingCaptureEvent (BindingSystem owns the conflict-resolution / mode-carry orchestration).
    bool renderCaptureModal(EventQueue &eq);
    // Starts a binding capture: pushes the BeginBindingCaptureEvent (BindingSystem fills in rebindState on
    // the next drain) and raises the capture modal, whose own deferred ShowModalEvent renders the body a
    // frame later — by then the state has landed. The capture kind picks the modal title here, so the body
    // need not read the not-yet-populated rebindState for it. The single start path for all four sites
    // (row "+", re-capture, modifier, provider-target).
    void beginCapture(EventQueue &eq, BeginBindingCaptureEvent req);
    // Command currently holding `trigger`, if any other than targetCommandId — null if unbound.
    [[nodiscard]] const Command *conflictingCommand(const Trigger &trigger, const std::string &targetCommandId) const;
    // Draws the preset bar (combo + Load / Save As / Delete) and latches its modals.
    void renderPresetBar();

    // Draws the "Add command…" popup interior: one entry per custom-command kind (template), then one entry
    // per provider *category* (the distinct groups among the inRebindList=false, unbound registry commands).
    // Picking a kind latches the custom editor; picking a category latches the provider-target modal below.
    void renderAddCommandPicker();

    // The provider-category target modal interior (raised for m_addProviderGroup). Lists that group's
    // unbound provider commands (e.g. each axis under "Select Axis"); picking one starts a binding capture
    // for it. Returns true when it should close.
    bool renderProviderTargetModal(EventQueue &eq, const std::string &group);

    // True when `id` currently holds at least one real (non-empty) trigger. Drives both the main list's
    // "show a bound provider row" rule and the "Only bound" filter, and excludes already-bound providers
    // from the Add-command picker.
    [[nodiscard]] bool hasValidBinding(const std::string &id) const;

    // The custom-command editor modal interior; returns true when it should close. Reads/writes
    // m_customDraft and pushes Add/UpdateCustomCommandEvent on Save.
    bool renderCustomEditor(EventQueue &eq);

    CommandRegistry &commandRegistry_;
    BindingSystem &bindingSystem_;
    const CustomCommandStore &customStore_;
    const CustomCommandTemplateRegistry &templates_;
    std::string filter_;           // fuzzy search box (UTF-8 safe)
    bool m_onlyBound = false;      // hide commands that currently have no binding (declutters the list)
    bool m_onlyBoundPrev = false;  // last frame's m_onlyBound — detects the rising edge that auto-expands groups
    bool m_openResetModal = false; // latches the "Reset Shortcuts?" modal open (raised once)

    // "Add command…" picker state. The popup lists custom-command kinds plus provider categories; picking a
    // category latches m_addProviderGroup (the target modal is raised next frame), and picking a target
    // there calls beginCapture() directly.
    std::string m_addProviderGroup; // non-empty ⇒ raise the provider-target modal for this group

    // Preset bar state. The list is cached (refreshed on window open and after each load/save/delete)
    // so render never touches the disk or allocates per frame.
    std::vector<PresetInfo> m_presets;
    bool m_presetsLoaded = false;     // false ⇒ refresh m_presets on the next render (re-open)
    std::string m_selectedPresetSlug; // current combo selection
    std::string m_presetNameBuf;      // Save-As name input
    bool m_openSaveAsModal = false;   // latches the Save-As modal
    std::string m_pendingLoadSlug;    // non-empty ⇒ raise the load-confirm modal
    std::string m_pendingLoadName;    // display name for the load-confirm message
    std::string m_pendingDeleteSlug;  // non-empty ⇒ raise the delete-confirm modal
    std::string m_pendingDeleteName;  // display name for the delete-confirm message

    // Custom-command editor state. The draft survives across frames (the modal body runs deferred), so it
    // lives here, not as a render-local. A non-empty m_customDraft.id means Edit (Update keeps it); an empty
    // id means Add (the store assigns the id) — so no separate editing flag is needed.
    bool m_openCustomModal = false;
    CustomCommand m_customDraft;
    std::string m_pendingDeleteCustomId;   // non-empty ⇒ raise the custom-command delete-confirm modal
    std::string m_pendingDeleteCustomName; // display name for that confirm
};

} // namespace ofs
