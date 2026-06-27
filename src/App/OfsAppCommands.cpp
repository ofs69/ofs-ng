// Command + default-binding registration for OfsApp, split out of OfsApp.cpp. This is the static
// command table (the ~500-line reg/regBind/regHold/regHoldAccum block); the dynamic per-axis/-chapter/
// -bookmark commands are built elsewhere (see CommandProviders). Kept as an OfsApp member definition in
// its own translation unit — the lambdas still capture `this` and reach private members directly, so
// this is a pure relocation with no signature or behavior change.
#include "OfsApp.h"

#include "Core/Events.h"
#include "Core/IntentEvents.h"
#include "Core/ScriptProject.h"
#include "Core/StandardAxis.h"
#include "Core/TranscodeEvents.h"
#include "Core/UpdateEvents.h"
#include "Localization/Translator.h"
#include "Services/BindingSystem.h"
#include "Services/CommandRegistry.h"
#include "UI/Icons.h"
#include "UI/ProcessingPanel.h"
#include "Util/Subprocess.h"
#include "Video/VideoPlayer.h"
#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

void OfsApp::initCommands() {
    // Category icons shown beside every command of that group in the palette. Declared once here so
    // both statically registered and dynamically generated (per-axis/-bookmark/-chapter) commands
    // pick up an icon by group name. Groups without an entry (e.g. a plugin's own group) get none.
    for (const auto &g : {
             ofs::CommandGroup{.name = "Core", .groupIcon = ICON_SETTINGS, .displayName = Str::CmdGrpCore},
             ofs::CommandGroup{.name = "Custom", .groupIcon = ICON_WAND_2, .displayName = Str::CmdGrpCustom},
             ofs::CommandGroup{.name = "Edit", .groupIcon = ICON_EDIT, .displayName = Str::CmdGrpEdit},
             ofs::CommandGroup{.name = "Moving", .groupIcon = ICON_MOVE, .displayName = Str::CmdGrpMoving},
             ofs::CommandGroup{.name = "Navigation", .groupIcon = ICON_NAV, .displayName = Str::CmdGrpNavigation},
             ofs::CommandGroup{.name = "Simulator", .groupIcon = ICON_MOVE_3D, .displayName = Str::CmdGrpSimulator},
             ofs::CommandGroup{.name = "Player", .groupIcon = ICON_PLAY, .displayName = Str::CmdGrpPlayer},
             ofs::CommandGroup{.name = "Video", .groupIcon = ICON_FILM, .displayName = Str::CmdGrpVideo},
             ofs::CommandGroup{.name = "View", .groupIcon = ICON_FULLSCREEN, .displayName = Str::CmdGrpView},
             ofs::CommandGroup{.name = "Axis", .groupIcon = ICON_LIST, .displayName = Str::CmdGrpAxis},
             ofs::CommandGroup{.name = "Select Axis", .groupIcon = ICON_LIST, .displayName = Str::CmdGrpSelectAxis},
             ofs::CommandGroup{
                 .name = "Go to Chapter", .groupIcon = ICON_CHAPTER, .displayName = Str::CmdGrpGoToChapter},
             ofs::CommandGroup{
                 .name = "Go to Bookmark", .groupIcon = ICON_BOOKMARK, .displayName = Str::CmdGrpGoToBookmark},
             ofs::CommandGroup{.name = "Toggle in Panel", .groupIcon = ICON_EYE, .displayName = Str::CmdGrpTogglePanel},
             ofs::CommandGroup{
                 .name = "Delete Scratch Axis", .groupIcon = ICON_TRASH, .displayName = Str::CmdGrpDeleteScratchAxis},
             ofs::CommandGroup{
                 .name = "Switch Edit Mode", .groupIcon = ICON_SPARKLE, .displayName = Str::CmdGrpSwitchEditMode},
             ofs::CommandGroup{
                 .name = "Switch Navigator", .groupIcon = ICON_FOOTPRINTS, .displayName = Str::CmdGrpSwitchNavigator},
             ofs::CommandGroup{.name = "Switch Selection Mode",
                               .groupIcon = ICON_BOX_SELECT,
                               .displayName = Str::CmdGrpSwitchSelectionMode},
             ofs::CommandGroup{.name = "Edit Mode Options",
                               .groupIcon = ICON_SLIDERS_HORIZONTAL,
                               .displayName = Str::CmdGrpEditModeOptions},
             ofs::CommandGroup{.name = "Navigator Options",
                               .groupIcon = ICON_SLIDERS_HORIZONTAL,
                               .displayName = Str::CmdGrpNavigatorOptions},
             ofs::CommandGroup{.name = "Selection Mode Options",
                               .groupIcon = ICON_SLIDERS_HORIZONTAL,
                               .displayName = Str::CmdGrpSelectionModeOptions},
         })
        commandRegistry.addGroup(g);

    // Helper: registers a Command + an optional default KeyChord binding (Press) in one call.
    // key == SDLK_UNKNOWN means keyboard-unbound (palette-only or gamepad-only).
    auto reg = [this](const char *id, const char *group, TrString title, std::function<void(ofs::EventQueue &)> run,
                      SDL_Keycode key = SDLK_UNKNOWN, SDL_Keymod mod = SDL_KMOD_NONE, const char *keywords = "") {
        commandRegistry.add(ofs::Command{
            .id = id,
            .group = group,
            .title = std::move(title),
            .keywords = keywords,
            .run = std::move(run),
        });
        if (key != SDLK_UNKNOWN)
            bindingSystem->addDefault(id, ofs::KeyChord{.key = key, .modifiers = mod});
    };
    // Keybind-only: same as reg but inPalette=false — action is assignable to a trigger but never
    // shown in the command palette (Edit, Moving, Navigation, Player, Sync Timestamps).
    auto regBind = [this](const char *id, const char *group, TrString title, std::function<void(ofs::EventQueue &)> run,
                          SDL_Keycode key = SDLK_UNKNOWN, SDL_Keymod mod = SDL_KMOD_NONE) {
        commandRegistry.add(ofs::Command{
            .id = id,
            .group = group,
            .title = std::move(title),
            .inPalette = false,
            .run = std::move(run),
        });
        if (key != SDLK_UNKNOWN)
            bindingSystem->addDefault(id, ofs::KeyChord{.key = key, .modifiers = mod});
    };
    // Keybind-only repeater: a discrete "fire while held" command. `step` performs one fire; the
    // command's tick replays it holdRepeats() times per frame for an app-owned cadence (replacing OS
    // key-repeat), and `run` keeps it usable as a plain Press binding too. The default binding is Hold.
    // The cadence is the single global appSettings.holdRepeat, read live each tick (via holdRepeatParams)
    // so Shortcut-window edits apply instantly and every hold binding shares one tunable feel.
    auto regHold = [this](const char *id, const char *group, TrString title,
                          const std::function<void(ofs::EventQueue &)> &step, SDL_Keycode key, SDL_Keymod mod) {
        commandRegistry.add(ofs::Command{
            .id = id,
            .group = group,
            .title = std::move(title),
            .inPalette = false,
            .run = step,
            .tick =
                [this, step](ofs::EventQueue &eq, const ofs::HoldTickInfo &info) {
                    for (int i = ofs::holdRepeats(info.elapsed, info.dt, holdRepeatParams()); i > 0; --i)
                        step(eq);
                },
        });
        if (key != SDLK_UNKNOWN)
            bindingSystem->addDefault(id, ofs::KeyChord{.key = key, .modifiers = mod}, ofs::ActivationMode::Hold);
    };
    // Accumulating variant of regHold: instead of replaying `step` N times per frame, it fires `step`
    // once with the frame's repeat count and a `first` flag, so the command can emit a single coalesced
    // event — one undo step + one mutate/seek per frame — for a held burst. `first` (info.first) marks
    // the first tick of the hold; pass it through as the event's snapshot flag so the whole hold is one
    // undo step. The Press fallback fires once with reps=1, first=true.
    auto regHoldAccum = [this](const char *id, const char *group, TrString title,
                               const std::function<void(ofs::EventQueue &, int reps, bool first)> &step,
                               SDL_Keycode key, SDL_Keymod mod) {
        commandRegistry.add(ofs::Command{
            .id = id,
            .group = group,
            .title = std::move(title),
            .inPalette = false,
            .run = [step](ofs::EventQueue &eq) { step(eq, 1, true); },
            .tick =
                [this, step](ofs::EventQueue &eq, const ofs::HoldTickInfo &info) {
                    const int reps = ofs::holdRepeats(info.elapsed, info.dt, holdRepeatParams());
                    if (reps > 0)
                        step(eq, reps, info.first);
                },
        });
        if (key != SDLK_UNKNOWN)
            bindingSystem->addDefault(id, ofs::KeyChord{.key = key, .modifiers = mod}, ofs::ActivationMode::Hold);
    };

    // --- Core ---
    // Meta command: offered for binding so users can rebind the trigger, but not listed in the palette itself.
    commandRegistry.add(ofs::Command{
        .id = "command-palette.open",
        .group = "Core",
        .title = Str::CmdPaletteOpen,
        .inRebindList = true,
        .inPalette = false,
        .requiresProject = false,
        .run = [](ofs::EventQueue &eq) { eq.push(ofs::OpenCommandPaletteEvent{}); },
    });
    bindingSystem->addDefault("command-palette.open",
                              ofs::KeyChord{.key = SDLK_P, .modifiers = SDL_KMOD_CTRL | SDL_KMOD_SHIFT});

    // Window openers — palette-only, not offered for binding by default. They flip an AppState visibility
    // flag directly, exactly as the menu bar does; window visibility is UI state, not project state, so no
    // event is needed. inRebindList=false keeps them out of the rebind UI's default list (a user who wants
    // a shortcut can still opt in via "Add command…").
    commandRegistry.add(ofs::Command{
        .id = "core.open-preferences",
        .group = "Core",
        .title = Str::CmdCoreOpenPreferences,
        .inRebindList = false,
        .keywords = "settings options config", // synonyms a user searches for app-wide preferences
        .requiresProject = false,
        .run = [this](ofs::EventQueue &) { appState.showConfigWindow = true; },
    });
    commandRegistry.add(ofs::Command{
        .id = "core.open-project-settings",
        .group = "Core",
        .title = Str::CmdCoreOpenProjectSettings,
        .inRebindList = false,
        .keywords = "metadata creator title properties config", // per-project settings vs. app-wide preferences
        .run = [this](ofs::EventQueue &) { appState.showProjectConfigWindow = true; },
    });
    commandRegistry.add(ofs::Command{
        .id = "core.open-keybindings",
        .group = "Core",
        .title = Str::CmdCoreOpenKeybindings,
        .inRebindList = false,
        .keywords = "hotkeys keybindings", // title says "Shortcuts"; reach it by the other common terms
        .requiresProject = false,
        .run = [this](ofs::EventQueue &) { appState.showShortcutWindow = true; },
    });
    commandRegistry.add(ofs::Command{
        .id = "core.open-about",
        .group = "Core",
        .title = Str::CmdCoreOpenAbout,
        .inRebindList = false,
        .keywords = "about credits attributions licenses version build", // synonyms for the About window
        .requiresProject = false,
        .run = [this](ofs::EventQueue &) { appState.showAboutWindow = true; },
    });
    commandRegistry.add(ofs::Command{
        .id = "core.check-for-updates",
        .group = "Core",
        .title = Str::CmdCoreCheckUpdates,
        .inRebindList = false,
        .keywords = "update upgrade version release new", // synonyms for the update check
        .requiresProject = false,
        .run = [](ofs::EventQueue &eq) { eq.push(ofs::CheckForUpdatesEvent{.userInitiated = true}); },
    });
    reg(
        "core.save", "Core", Str::CmdCoreSave, [](ofs::EventQueue &eq) { eq.push(ofs::SaveProjectEvent{false}); },
        SDLK_S, SDL_KMOD_CTRL, "write store");
    reg(
        "core.quick-export", "Core", Str::CmdCoreQuickExport,
        [](ofs::EventQueue &eq) { eq.push(ofs::QuickExportEvent{}); }, SDLK_S, SDL_KMOD_CTRL | SDL_KMOD_SHIFT,
        "save funscript output write");
    regBind(
        "core.sync-timestamps", "Core", Str::CmdCoreSyncTimestamps,
        [this](ofs::EventQueue &eq) { eq.push(ofs::SeekEvent{player->getActualPosition()}); }, SDLK_S, SDL_KMOD_NONE);
    // Window mode is UI state, not project state, so the command pokes the Window directly (main-thread,
    // like the title-bar buttons) rather than routing an event. requiresProject defaults true via reg,
    // but fullscreen must work with no project open — so register it explicitly with requiresProject=false.
    commandRegistry.add(ofs::Command{
        .id = "view.toggle-fullscreen",
        .group = "View",
        .title = Str::CmdViewToggleFullscreen,
        .keywords = "maximize borderless screen window", // synonyms for the fullscreen toggle
        .requiresProject = false,
        .run = [this](ofs::EventQueue &) { window->toggleFullscreen(); },
    });
    bindingSystem->addDefault("view.toggle-fullscreen", ofs::KeyChord{.key = SDLK_F11});

    // --- Axis ---
    // Adds the next free scratch axis (S0–S9). Disabled (hidden from the palette via the isEnabled
    // seam) once all ten scratch slots are taken. Per-axis "Toggle in Panel" / "Delete Scratch Axis"
    // commands are generated dynamically from project state (see CommandProviders).
    commandRegistry.add(ofs::Command{
        .id = "axis.add-scratch",
        .group = "Axis",
        .title = Str::CmdAxisAddScratch,
        .keywords = "new track layer extra temporary", // ways a user describes adding a spare working axis
        .run = [](ofs::EventQueue &eq) { eq.push(ofs::AddScratchAxisEvent{}); },
        .isEnabled =
            [this] {
                for (auto i = static_cast<size_t>(ofs::StandardAxis::S0); i < ofs::kStandardAxisCount; ++i)
                    if (!scriptProject.axes[i].showInStrip)
                        return true;
                return false;
            },
    });

    // --- Edit ---
    regBind(
        "edit.remove-action", "Edit", Str::CmdEditRemoveAction,
        [this](ofs::EventQueue &eq) {
            // Graph node/link selection takes priority — deleting inside the graph is more specific than
            // dropping the whole region. deleteSelected returns false when nothing in the graph is
            // selected, so we fall through to deleting the selected region itself.
            if (processingPanel && processingPanel->isGraphFocused() &&
                processingPanel->deleteSelected(scriptProject, eq))
                return;
            if (scriptProject.procSelRegionId != -1) {
                eq.push(ofs::DeleteRegionEvent{scriptProject.procSelRegionId});
                return;
            }
            const auto role = scriptProject.state.activeAxis;
            if (role < ofs::StandardAxis::Count)
                eq.push(ofs::EditRequestEvent{.intent = {.kind = ofs::EditIntentKind::RemoveSelected, .axis = role}});
        },
        SDLK_DELETE, SDL_KMOD_NONE);

    auto makeAddAction = [](int pos) {
        return [pos](ofs::EventQueue &eq) {
            eq.push(ofs::EditRequestEvent{.intent = {.kind = ofs::EditIntentKind::AddPointAtPlayhead, .pos = pos}});
        };
    };
    struct AddActionCmd {
        const char *id;
        SDL_Keycode key;
        int pos;
        TrKey title;
    };
    static const AddActionCmd kAddActions[] = {
        {.id = "edit.add-action-0", .key = SDLK_KP_0, .pos = 0, .title = Str::CmdEditAddAction0},
        {.id = "edit.add-action-10", .key = SDLK_KP_1, .pos = 10, .title = Str::CmdEditAddAction10},
        {.id = "edit.add-action-20", .key = SDLK_KP_2, .pos = 20, .title = Str::CmdEditAddAction20},
        {.id = "edit.add-action-30", .key = SDLK_KP_3, .pos = 30, .title = Str::CmdEditAddAction30},
        {.id = "edit.add-action-40", .key = SDLK_KP_4, .pos = 40, .title = Str::CmdEditAddAction40},
        {.id = "edit.add-action-50", .key = SDLK_KP_5, .pos = 50, .title = Str::CmdEditAddAction50},
        {.id = "edit.add-action-60", .key = SDLK_KP_6, .pos = 60, .title = Str::CmdEditAddAction60},
        {.id = "edit.add-action-70", .key = SDLK_KP_7, .pos = 70, .title = Str::CmdEditAddAction70},
        {.id = "edit.add-action-80", .key = SDLK_KP_8, .pos = 80, .title = Str::CmdEditAddAction80},
        {.id = "edit.add-action-90", .key = SDLK_KP_9, .pos = 90, .title = Str::CmdEditAddAction90},
        {.id = "edit.add-action-100", .key = SDLK_KP_DIVIDE, .pos = 100, .title = Str::CmdEditAddAction100},
    };
    for (const auto &c : kAddActions)
        regBind(c.id, "Edit", c.title, makeAddAction(c.pos), c.key, SDL_KMOD_NONE);

    regHold(
        "edit.undo", "Edit", Str::CmdEditUndo, [](ofs::EventQueue &eq) { eq.push(ofs::UndoEvent{}); }, SDLK_Z,
        SDL_KMOD_CTRL);
    regHold(
        "edit.redo", "Edit", Str::CmdEditRedo, [](ofs::EventQueue &eq) { eq.push(ofs::RedoEvent{}); }, SDLK_Y,
        SDL_KMOD_CTRL);
    regBind(
        "edit.copy", "Edit", Str::CmdEditCopy, [](ofs::EventQueue &eq) { eq.push(ofs::CopySelectionEvent{}); }, SDLK_C,
        SDL_KMOD_CTRL);
    regBind(
        "edit.paste", "Edit", Str::CmdEditPaste,
        [this](ofs::EventQueue &eq) {
            eq.push(ofs::EditRequestEvent{.intent = {.kind = ofs::EditIntentKind::Paste,
                                                     .time = scriptProject.playback.cursorPos,
                                                     .exact = false}});
        },
        SDLK_V, SDL_KMOD_CTRL);
    regBind(
        "edit.paste-exact", "Edit", Str::CmdEditPasteExact,
        [this](ofs::EventQueue &eq) {
            eq.push(ofs::EditRequestEvent{.intent = {.kind = ofs::EditIntentKind::Paste,
                                                     .time = scriptProject.playback.cursorPos,
                                                     .exact = true}});
        },
        SDLK_V, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
    regBind(
        "edit.cut", "Edit", Str::CmdEditCut,
        [this](ofs::EventQueue &eq) {
            // Capture the whole group first (lossless), then fan the delete out across it. CopySelection
            // drains before RemoveSelected, so the clipboard captures before the points are removed.
            eq.push(ofs::CopySelectionEvent{});
            eq.push(ofs::EditRequestEvent{
                .intent = {.kind = ofs::EditIntentKind::RemoveSelected, .axis = scriptProject.state.activeAxis}});
        },
        SDLK_X, SDL_KMOD_CTRL);
    regBind(
        "edit.select-all", "Edit", Str::CmdEditSelectAll,
        [this](ofs::EventQueue &eq) {
            const auto role = scriptProject.state.activeAxis;
            if (role < ofs::StandardAxis::Count)
                eq.push(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::All, .axis = role});
        },
        SDLK_A, SDL_KMOD_CTRL);
    regBind(
        "edit.deselect-all", "Edit", Str::CmdEditDeselectAll,
        [this](ofs::EventQueue &eq) {
            // Clear the whole active group. SetAxisSelectionEvent is per-axis (it must not fan out — a
            // plugin targets one axis), so loop the edit set here.
            const ofs::AxisRoles targets = scriptProject.effectiveEditSet();
            for (size_t i = 0; i < ofs::kStandardAxisCount; ++i)
                if (targets.test(i))
                    eq.push(ofs::SetAxisSelectionEvent{.axis = static_cast<ofs::StandardAxis>(i), .selection = {}});
        },
        SDLK_D, SDL_KMOD_CTRL);
    regBind(
        "edit.select-all-left", "Edit", Str::CmdEditSelectAllLeft,
        [this](ofs::EventQueue &eq) {
            const auto role = scriptProject.state.activeAxis;
            if (role >= ofs::StandardAxis::Count)
                return;
            eq.push(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::Box,
                                            .axis = role,
                                            .startTime = 0.0,
                                            .endTime = scriptProject.playback.cursorPos,
                                            .additive = false});
        },
        SDLK_LEFT, SDL_KMOD_CTRL | SDL_KMOD_ALT);
    regBind(
        "edit.select-all-right", "Edit", Str::CmdEditSelectAllRight,
        [this](ofs::EventQueue &eq) {
            const auto role = scriptProject.state.activeAxis;
            if (role >= ofs::StandardAxis::Count)
                return;
            const double duration = player ? player->getDuration() : 0.0;
            eq.push(ofs::SelectRequestEvent{.gesture = ofs::SelectGesture::Box,
                                            .axis = role,
                                            .startTime = scriptProject.playback.cursorPos,
                                            .endTime = duration,
                                            .additive = false});
        },
        SDLK_RIGHT, SDL_KMOD_CTRL | SDL_KMOD_ALT);

    // --- Edit / Moving ---
    // Accumulating step builders: a held burst arrives as reps>1 (move reps units in one event) and
    // first marks the hold's first fire (→ snapshot, so the whole hold is one undo step). `base` is the
    // per-unit position delta; time uses direction (±1) × reps with the unit step resolved in the handler.
    auto makeMovePos = [this](int base) {
        return [this, base](ofs::EventQueue &eq, int reps, bool first) {
            const auto role = scriptProject.state.activeAxis;
            if (role >= ofs::StandardAxis::Count)
                return;
            // first → gesture boundary (snapshot); a later hold fire continues the same undo step.
            eq.push(ofs::EditRequestEvent{
                .intent = {.kind = ofs::EditIntentKind::MoveSelection, .axis = role, .pos = base * reps},
                .gesture = first ? ofs::GesturePhase::Begin : ofs::GesturePhase::Continue});
        };
    };
    auto makeMoveTime = [this](ofs::StepDirection direction, bool seekAfter) {
        return [this, direction, seekAfter](ofs::EventQueue &eq, int reps, bool first) {
            const auto role = scriptProject.state.activeAxis;
            if (role >= ofs::StandardAxis::Count)
                return;
            // first → gesture boundary (snapshot); a later hold fire continues the same undo step.
            eq.push(ofs::EditRequestEvent{.intent = {.kind = ofs::EditIntentKind::MoveSelection,
                                                     .axis = role,
                                                     .direction = direction,
                                                     .reps = reps,
                                                     .seekAfter = seekAfter},
                                          .gesture = first ? ofs::GesturePhase::Begin : ofs::GesturePhase::Continue});
        };
    };
    regHoldAccum("edit.move-actions-left-snapped", "Moving", Str::CmdMoveActionsLeftSnapped,
                 makeMoveTime(ofs::StepDirection::Backward, true), SDLK_LEFT, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
    regHoldAccum("edit.move-actions-right-snapped", "Moving", Str::CmdMoveActionsRightSnapped,
                 makeMoveTime(ofs::StepDirection::Forward, true), SDLK_RIGHT, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
    regHoldAccum("edit.move-actions-left", "Moving", Str::CmdMoveActionsLeft,
                 makeMoveTime(ofs::StepDirection::Backward, false), SDLK_LEFT, SDL_KMOD_SHIFT);
    regHoldAccum("edit.move-actions-right", "Moving", Str::CmdMoveActionsRight,
                 makeMoveTime(ofs::StepDirection::Forward, false), SDLK_RIGHT, SDL_KMOD_SHIFT);
    regHoldAccum("edit.move-actions-up", "Moving", Str::CmdMoveActionsUp, makeMovePos(1), SDLK_UP, SDL_KMOD_SHIFT);
    regHoldAccum("edit.move-actions-down", "Moving", Str::CmdMoveActionsDown, makeMovePos(-1), SDLK_DOWN,
                 SDL_KMOD_SHIFT);
    regBind(
        "edit.move-action-to-playhead", "Moving", Str::CmdMoveActionToPlayhead,
        [this](ofs::EventQueue &eq) {
            const auto role = scriptProject.state.activeAxis;
            if (role >= ofs::StandardAxis::Count)
                return;
            // Not an edit intent — moving an action onto the playhead applies natively (no mode routing).
            eq.push(ofs::MoveActionToCurrentTimeEvent{.axis = role});
        },
        SDLK_END, SDL_KMOD_NONE);

    // --- Navigation ---
    // A held burst walks `reps` actions in one fire and seeks once (navigation only, no undo). `first`
    // is unused here.
    regHoldAccum(
        "navigation.prev-action", "Navigation", Str::CmdNavPrevAction,
        [](ofs::EventQueue &eq, int reps, bool) {
            eq.push(ofs::StepRequestEvent{
                .direction = ofs::StepDirection::Backward, .reps = reps, .granularity = ofs::StepGranularity::Action});
        },
        SDLK_DOWN, SDL_KMOD_NONE);
    regHoldAccum(
        "navigation.next-action", "Navigation", Str::CmdNavNextAction,
        [](ofs::EventQueue &eq, int reps, bool) {
            eq.push(ofs::StepRequestEvent{
                .direction = ofs::StepDirection::Forward, .reps = reps, .granularity = ofs::StepGranularity::Action});
        },
        SDLK_UP, SDL_KMOD_NONE);
    regHoldAccum(
        "navigation.prev-action-multi", "Navigation", Str::CmdNavPrevActionMulti,
        [](ofs::EventQueue &eq, int reps, bool) {
            eq.push(ofs::StepRequestEvent{.direction = ofs::StepDirection::Backward,
                                          .reps = reps,
                                          .granularity = ofs::StepGranularity::ActionAllAxes});
        },
        SDLK_DOWN, SDL_KMOD_CTRL);
    regHoldAccum(
        "navigation.next-action-multi", "Navigation", Str::CmdNavNextActionMulti,
        [](ofs::EventQueue &eq, int reps, bool) {
            eq.push(ofs::StepRequestEvent{.direction = ofs::StepDirection::Forward,
                                          .reps = reps,
                                          .granularity = ofs::StepGranularity::ActionAllAxes});
        },
        SDLK_UP, SDL_KMOD_CTRL);

    auto makeCycleAxis = [this](bool backward) {
        return [this, backward](ofs::EventQueue &eq) {
            std::vector<ofs::StandardAxis> panelAxes;
            for (size_t i = 0; i < ofs::kStandardAxisCount; ++i) {
                if (scriptProject.axes[i].showInStrip)
                    panelAxes.push_back(static_cast<ofs::StandardAxis>(i));
            }
            if (panelAxes.empty())
                return;
            const auto active = scriptProject.state.activeAxis;
            auto it = std::ranges::find(panelAxes, active);
            int idx = (it != panelAxes.end()) ? static_cast<int>(std::distance(panelAxes.begin(), it)) : 0;
            int n = static_cast<int>(panelAxes.size());
            idx = backward ? (idx - 1 + n) % n : (idx + 1) % n;
            eq.push(ofs::AxisSelectedEvent{panelAxes[idx]});
        };
    };
    // Cycle the active axis with Tab / Shift+Tab — the universal tab-cycling idiom. ImGui's own Tab
    // item-cycling stays off (ConfigNavEnableTabbing = false in initImGui), so no collision; like the
    // other unmodified-key shortcuts this yields to ImGui while a navigable window holds keyboard focus.
    regBind("navigation.cycle-axis-forward", "Navigation", Str::CmdNavCycleAxisForward, makeCycleAxis(false), SDLK_TAB,
            SDL_KMOD_NONE);
    regBind("navigation.cycle-axis-backward", "Navigation", Str::CmdNavCycleAxisBackward, makeCycleAxis(true), SDLK_TAB,
            SDL_KMOD_SHIFT);
    // A held burst advances `reps` grid steps in one event → one SeekEvent. Granularity defaults to
    // Frame; the active navigator resolves it (overlay grid by default, or a plugin override).
    regHoldAccum(
        "navigation.prev-step", "Navigation", Str::CmdNavPrevStep,
        [](ofs::EventQueue &eq, int reps, bool) {
            eq.push(ofs::StepRequestEvent{.direction = ofs::StepDirection::Backward, .reps = reps});
        },
        SDLK_LEFT, SDL_KMOD_NONE);
    regHoldAccum(
        "navigation.next-step", "Navigation", Str::CmdNavNextStep,
        [](ofs::EventQueue &eq, int reps, bool) {
            eq.push(ofs::StepRequestEvent{.direction = ofs::StepDirection::Forward, .reps = reps});
        },
        SDLK_RIGHT, SDL_KMOD_NONE);

    // --- Player ---
    regBind(
        "player.play-pause", "Player", Str::CmdPlayerPlayPause,
        [](ofs::EventQueue &eq) { eq.push(ofs::PlayPauseEvent{}); }, SDLK_SPACE, SDL_KMOD_NONE);
    regBind(
        "player.toggle-mute", "Player", Str::CmdPlayerToggleMute,
        [this](ofs::EventQueue &eq) {
            if (!player)
                return;
            const float vol = player->getVolume();
            if (vol > 0.0f) {
                appSettings.volume = vol;
                eq.push(ofs::VolumeChangedEvent{0.0f});
            } else {
                eq.push(ofs::VolumeChangedEvent{appSettings.volume});
            }
        },
        SDLK_M, SDL_KMOD_NONE);
    regHold(
        "player.volume-up", "Player", Str::CmdPlayerVolumeUp,
        [this](ofs::EventQueue &eq) {
            if (player)
                eq.push(ofs::VolumeChangedEvent{player->getVolume() + 0.1f});
        },
        SDLK_UP, SDL_KMOD_CTRL);
    regHold(
        "player.volume-down", "Player", Str::CmdPlayerVolumeDown,
        [this](ofs::EventQueue &eq) {
            if (player)
                eq.push(ofs::VolumeChangedEvent{player->getVolume() - 0.1f});
        },
        SDLK_DOWN, SDL_KMOD_CTRL);
    regHold(
        "player.speed-down", "Player", Str::CmdPlayerSpeedDown,
        [this](ofs::EventQueue &eq) {
            if (player)
                eq.push(ofs::PlaybackSpeedEvent{player->getPlaybackSpeed() - 0.1f});
        },
        SDLK_KP_MINUS, SDL_KMOD_NONE);
    regHold(
        "player.speed-up", "Player", Str::CmdPlayerSpeedUp,
        [this](ofs::EventQueue &eq) {
            if (player)
                eq.push(ofs::PlaybackSpeedEvent{player->getPlaybackSpeed() + 0.1f});
        },
        SDLK_KP_PLUS, SDL_KMOD_NONE);
    // Palette-visible (reg, not regBind): these ship with no default key, so without a palette slot
    // they'd be unreachable. They're exactly the no-hotkey long tail the palette exists for.
    reg(
        "player.goto-start", "Player", Str::CmdPlayerGotoStart,
        [](ofs::EventQueue &eq) { eq.push(ofs::SeekEvent{0.0}); }, SDLK_UNKNOWN, SDL_KMOD_NONE);
    reg(
        "player.goto-end", "Player", Str::CmdPlayerGotoEnd,
        [this](ofs::EventQueue &eq) {
            if (player)
                eq.push(ofs::SeekEvent{player->getDuration()});
        },
        SDLK_UNKNOWN, SDL_KMOD_NONE);

    // --- Video ---
    // Intra-frame optimize: opens the options modal (Step 9). Gated so the palette hides it unless it can
    // actually run — both ffmpeg/ffprobe resolve, a real (non-dummy) video is loaded as the original
    // source, and no transcode is already in flight. The output dir is intentionally NOT part of the gate:
    // when none is set the modal opens the folder picker on demand, so a first-time user can reach the
    // flow instead of finding the command greyed out.
    commandRegistry.add(ofs::Command{
        .id = "video.optimize-intra",
        .group = "Video",
        .title = Str::CmdOptimizeIntra,
        .keywords = "transcode convert", // "Optimize for Seeking" — reach it by what it does under the hood
        .run = [](ofs::EventQueue &eq) { eq.push(ofs::OpenTranscodeDialogEvent{}); },
        .isEnabled = [this] { return canOptimizeIntra(); },
    });
}
