#include "Services/CommandProviders.h"

#include "Core/Events.h"
#include "Core/IntentEvents.h"
#include "Core/ScriptProject.h"
#include "Core/StandardAxis.h"
#include "Localization/AxisNames.h"
#include "Localization/Translator.h"
#include "Services/EditModeRegistry.h"
#include "Services/NavigatorRegistry.h"
#include "Services/SelectionModeRegistry.h"

#include <spdlog/fmt/fmt.h>
#include <string>

namespace ofs {
namespace {

// FNV-1a fold. Used only to detect change in the nav-relevant state; not a stable on-disk value.
constexpr uint64_t kHashSeed = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

uint64_t hashBytes(uint64_t h, const void *data, size_t n) {
    const auto *p = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= kFnvPrime;
    }
    return h;
}

uint64_t hashDouble(uint64_t h, double d) {
    return hashBytes(h, &d, sizeof(d));
}
uint64_t hashStr(uint64_t h, const std::string &s) {
    return hashBytes(h, s.data(), s.size());
}

} // namespace

void buildNavigationCommands(const ScriptProject &project, std::vector<Command> &out) {
    // Chapters → seek to start. Names are user-supplied and may be empty; fall back to an index label.
    const auto &chapters = project.bookmarks.chapters;
    for (size_t i = 0; i < chapters.size(); ++i) {
        const double t = chapters[i].startTime;
        const size_t num = i + 1;
        out.push_back(Command{
            .id = fmt::format("nav.chapter.{}", i),
            .group = "Go to Chapter",
            .title = chapters[i].name.empty() ? std::string(Str::CmdDynChapterFallback.fmt(num)) : chapters[i].name,
            .inRebindList = false,
            .run = [t](EventQueue &eq) { eq.push(SeekEvent{t}); },
        });
    }

    // Bookmarks → seek to the bookmark time.
    const auto &bookmarks = project.bookmarks.bookmarks;
    for (size_t i = 0; i < bookmarks.size(); ++i) {
        const double t = bookmarks[i].time;
        const size_t num = i + 1;
        out.push_back(Command{
            .id = fmt::format("nav.bookmark.{}", i),
            .group = "Go to Bookmark",
            .title = bookmarks[i].name.empty() ? std::string(Str::CmdDynBookmarkFallback.fmt(num)) : bookmarks[i].name,
            .inRebindList = false,
            .run = [t](EventQueue &eq) { eq.push(SeekEvent{t}); },
        });
    }
}

// True when this role contributes a registry-resident select-axis / toggle-panel command. Standard axes
// are stable binding targets and are always present; a scratch axis (S0–S9) is present only while it
// exists() — created/deleted scratch axes add/remove their commands, which is exactly the set-membership
// change registryProviderSignature folds. (Existence, not panel visibility: a hidden data-bearing axis
// still has a stable id worth binding; its commands are gated live by isEnabled, not by set membership.)
static bool axisProviderPresent(StandardAxis role, const AxisState &axis) {
    return !isScratchAxis(role) || axis.exists();
}

void buildAxisProviderCommands(const ScriptProject &project, std::vector<Command> &out) {
    // Registry-resident axis commands (source=Dynamic, inRebindList=false → opt-in). Unlike the palette-only
    // providers these carry a stable id so a key
    // can be bound to them: the command exists whenever the *axis* exists, and the contextual no-op is
    // gated by isEnabled (greyed in the palette/rebind UI, ignored by dispatch) rather than blinked out of
    // the set — so a binding survives while its target is e.g. the active axis. The run/isEnabled closures
    // read OfsApp's long-lived ScriptProject live, so they reflect current state without a rebuild.

    // Select Axis → make the axis active. Present for every existing axis; enabled only when the axis is
    // shown in the panel (selecting a hidden axis has no useful effect) and is not already active (a no-op).
    for (size_t i = 0; i < kStandardAxisCount; ++i) {
        const auto role = static_cast<StandardAxis>(i);
        if (!axisProviderPresent(role, project.axes[i]))
            continue;
        out.push_back(Command{
            .id = fmt::format("nav.axis.{}", standardAxisShortName(role)),
            .group = "Select Axis",
            .title = std::string(ofs::loc::localizedAxisName(role)),
            .source = CommandSource::Dynamic,
            .inRebindList = false,
            .run = [role](EventQueue &eq) { eq.push(AxisSelectedEvent{role}); },
            .isEnabled = [&project, role,
                          i] { return project.axes[i].showInStrip && role != project.state.activeAxis; },
        });
    }

    // Toggle in Panel → flip the axis's panel visibility. Collapses the former contextual show-/hide-in-
    // panel pair into one stable id (a key bound to "show" would otherwise vanish the moment the axis was
    // shown). L0 is permanently pinned (ProjectManager rejects hiding it), so it gets no toggle. An empty
    // scratch axis is managed via Add/Delete Scratch Axis, never shown/hidden, so its toggle is disabled.
    for (size_t i = 0; i < kStandardAxisCount; ++i) {
        const auto role = static_cast<StandardAxis>(i);
        if (role == StandardAxis::L0 || !axisProviderPresent(role, project.axes[i]))
            continue;
        out.push_back(Command{
            .id = fmt::format("axis.toggle-panel.{}", standardAxisShortName(role)),
            .group = "Toggle in Panel",
            .title = std::string(ofs::loc::localizedAxisName(role)),
            .source = CommandSource::Dynamic,
            .inRebindList = false,
            .run =
                [&project, role, i](EventQueue &eq) {
                    eq.push(ToggleAxisPanelVisibilityEvent{.axisRole = role, .inPanel = !project.axes[i].showInStrip});
                },
            .isEnabled = [&project, role, i] { return !(isScratchAxis(role) && project.axes[i].actions.empty()); },
        });
    }
}

void buildAxisCommands(const ScriptProject &project, std::vector<Command> &out) {
    // Delete Scratch Axis → remove an empty user-created scratch axis (S0–S9). Standard axes can't be
    // deleted, and neither can a scratch axis that holds actions (it behaves like a standard axis).
    for (size_t i = 0; i < kStandardAxisCount; ++i) {
        const auto &axis = project.axes[i];
        const auto role = static_cast<StandardAxis>(i);
        if (!isScratchAxis(role) || !axis.showInStrip || !axis.actions.empty())
            continue;
        out.push_back(Command{
            .id = fmt::format("axis.delete-scratch.{}", standardAxisShortName(role)),
            .group = "Delete Scratch Axis",
            .title = std::string(ofs::loc::localizedAxisName(role)),
            .inRebindList = false,
            .run = [role](EventQueue &eq) { eq.push(RemoveAxisEvent{.axisRole = role}); },
        });
    }
}

void buildModeSwitchCommands(const ScriptProject &project, const EditModeRegistry &editModes,
                             const NavigatorRegistry &navigators, const SelectionModeRegistry &selectionModes,
                             std::vector<Command> &out) {
    // Registry-resident provider commands (source=Dynamic, inRebindList=false → not offered for binding by
    // *default*, but the user may opt in). One command per
    // registered mode, present whether or not it is active: a binding to "switch to mode X" must survive
    // while X *is* the active mode, so the id can't blink out when it activates (the active bindings.json
    // would drop it on save). The contextual no-op — switching to the already-active mode — is gated by
    // isEnabled instead, so the palette/rebind UI greys it and dispatch ignores it. The set therefore
    // changes only when the mode *set* (plugin load/unload) or the UI language changes, not on every
    // active-mode switch (registryProviderSignature is correspondingly coarse). `project` is OfsApp's
    // long-lived ScriptProject; the isEnabled closures read its active-mode fields live.
    for (const auto &m : editModes.entries()) {
        out.push_back(Command{
            .id = fmt::format("mode.edit.{}", m.id),
            .group = "Switch Edit Mode",
            .title = m.id == kNativeEditModeId ? std::string(Str::FtEditModeNative.sv())
                     : !m.displayName.empty()  ? m.displayName
                                               : m.id,
            .source = CommandSource::Dynamic,
            .inRebindList = false,
            .run = [id = m.id](EventQueue &eq) { eq.push(SetActiveEditModeEvent{.id = id}); },
            .isEnabled = [&project, id = m.id] { return project.activeEditMode != id; },
        });
    }

    for (const auto &n : navigators.entries()) {
        out.push_back(Command{
            .id = fmt::format("mode.navigator.{}", n.id),
            .group = "Switch Navigator",
            .title = n.id == kFollowOverlayNavigatorId ? std::string(Str::FtStepNative.sv())
                     : !n.displayName.empty()          ? n.displayName
                                                       : n.id,
            .source = CommandSource::Dynamic,
            .inRebindList = false,
            .run = [id = n.id](EventQueue &eq) { eq.push(SetActiveNavigatorEvent{.id = id}); },
            .isEnabled = [&project, id = n.id] { return project.activeNavigator != id; },
        });
    }

    for (const auto &s : selectionModes.entries()) {
        out.push_back(Command{
            .id = fmt::format("mode.select.{}", s.id),
            .group = "Switch Selection Mode",
            .title = s.id == kNativeSelectionModeId ? std::string(Str::FtSelectNative.sv())
                     : !s.displayName.empty()       ? s.displayName
                                                    : s.id,
            .source = CommandSource::Dynamic,
            .inRebindList = false,
            .run = [id = s.id](EventQueue &eq) { eq.push(SetActiveSelectionModeEvent{.id = id}); },
            .isEnabled = [&project, id = s.id] { return project.activeSelectionMode != id; },
        });
    }
}

void buildToolOptionsCommands(const ScriptProject &project, const EditModeRegistry &editModes,
                              const NavigatorRegistry &navigators, const SelectionModeRegistry &selectionModes,
                              std::vector<Command> &out) {
    // Registry-resident (source=Dynamic, inRebindList=false → opt-in). One command per *registered* mode that
    // supplies options (onUi) — not just the active one — so a key can
    // be bound to "open <this mode>'s options" and survive whether or not that mode is currently active.
    // OpenToolOptionsEvent opens the active mode's options for its extension point, so the command is
    // enabled (and dispatch-effective) only while its own mode is the active one; isEnabled gates that, the
    // palette/rebind UI greys the rest. The set changes only when a mode *set* changes (plugin load/unload)
    // — registryProviderSignature folds the mode ids — never on an active-mode switch. A per-extension-
    // point group (parallel to the "Switch *" groups) keeps two modes that share a display name distinct in
    // the palette — "Navigator Options: Peaks" vs "Selection Mode Options: Peaks" — and findable by
    // extension point; the id discriminator keeps the command ids unique for the same reason.
    auto add = [&](const char *disc, const char *group, ToolOptionTarget target, const std::string &activeId,
                   const auto &entry) {
        if (!entry.onUi) // no options UI → no affordance, exactly as the footer omits it
            return;
        out.push_back(Command{
            .id = fmt::format("tooloptions.{}.{}", disc, entry.id),
            .group = group,
            .title = entry.displayName.empty() ? entry.id : entry.displayName,
            .source = CommandSource::Dynamic,
            .inRebindList = false,
            .run = [target](EventQueue &eq) { eq.push(OpenToolOptionsEvent{.target = target}); },
            // activeId aliases the long-lived project's active-mode field; read live so the gate tracks the
            // current selection without a rebuild.
            .isEnabled = [&activeId, id = entry.id] { return activeId == id; },
        });
    };
    for (const auto &e : editModes.entries())
        add("edit", "Edit Mode Options", ToolOptionTarget::Edit, project.activeEditMode, e);
    for (const auto &n : navigators.entries())
        add("nav", "Navigator Options", ToolOptionTarget::Navigator, project.activeNavigator, n);
    for (const auto &s : selectionModes.entries())
        add("select", "Selection Mode Options", ToolOptionTarget::Selection, project.activeSelectionMode, s);
}

void buildRegistryProviderCommands(const ScriptProject &project, const EditModeRegistry &editModes,
                                   const NavigatorRegistry &navigators, const SelectionModeRegistry &selectionModes,
                                   std::vector<Command> &out) {
    // Everything that lives *in* the CommandRegistry as a provider command (source=Dynamic) so a key can
    // be bound to it: mode switches, axis select/toggle, and tool options. OfsApp rebuilds this set into
    // the registry only when registryProviderSignature changes — never per frame. The contextual no-ops
    // are handled by each command's isEnabled, not by membership, so binds survive an active target.
    buildModeSwitchCommands(project, editModes, navigators, selectionModes, out);
    buildAxisProviderCommands(project, out);
    buildToolOptionsCommands(project, editModes, navigators, selectionModes, out);
}

void buildDynamicCommands(const ScriptProject &project, const EditModeRegistry &, const NavigatorRegistry &,
                          const SelectionModeRegistry &, std::vector<Command> &out) {
    // The per-frame, palette-only set (inRebindList=false, NOT in the registry): positional navigation
    // (chapters/bookmarks) and scratch-axis deletion. Everything offered for binding — mode switches, axis
    // select/toggle, tool options — is registry-resident (buildRegistryProviderCommands).
    buildNavigationCommands(project, out);
    buildAxisCommands(project, out);
}

uint64_t registryProviderSignature(const ScriptProject &project, const EditModeRegistry &editModes,
                                   const NavigatorRegistry &navigators, const SelectionModeRegistry &selectionModes) {
    // Gates the registry-resident provider rebuild (mode switches + axis select/toggle + tool options).
    // The set changes only when (a) a mode *set* changes (plugin load/unload — adds/removes mode-switch
    // and tool-options commands), (b) a *scratch* axis comes into or out of existence (adds/removes its
    // select/toggle commands — standard axes are always present), or (c) the UI language changes (titles
    // re-localize). It is deliberately blind to which mode/axis is *active* and to panel visibility on an
    // existing axis — those are the isEnabled gates' job, handled live with no rebuild.
    uint64_t h = kHashSeed;
    auto foldSet = [&h](const auto &entries) {
        const uint64_t n = entries.size();
        h = hashBytes(h, &n, sizeof(n));
        for (const auto &e : entries)
            h = hashStr(h, e.id);
    };
    foldSet(editModes.entries());
    foldSet(navigators.entries());
    foldSet(selectionModes.entries());
    uint32_t scratchExistMask = 0;
    for (auto i = static_cast<size_t>(StandardAxis::S0); i < kStandardAxisCount; ++i)
        if (project.axes[i].exists())
            scratchExistMask |= (1u << i);
    h = hashBytes(h, &scratchExistMask, sizeof(scratchExistMask));
    return hashStr(h, ofs::loc::Translator::instance().activeLanguage());
}

uint64_t navigationSignature(const ScriptProject &project) {
    uint64_t h = kHashSeed;

    const auto &chapters = project.bookmarks.chapters;
    const uint64_t chapterCount = chapters.size();
    h = hashBytes(h, &chapterCount, sizeof(chapterCount));
    for (const auto &ch : chapters) {
        h = hashDouble(h, ch.startTime);
        h = hashStr(h, ch.name);
    }

    const auto &bookmarks = project.bookmarks.bookmarks;
    const uint64_t bookmarkCount = bookmarks.size();
    h = hashBytes(h, &bookmarkCount, sizeof(bookmarkCount));
    for (const auto &bm : bookmarks) {
        h = hashDouble(h, bm.time);
        h = hashStr(h, bm.name);
    }

    return h;
}

uint64_t dynamicCommandsSignature(const ScriptProject &project, const EditModeRegistry &, const NavigatorRegistry &,
                                  const SelectionModeRegistry &) {
    // The per-frame palette-only set is now just navigation (chapters/bookmarks) + Delete Scratch Axis.
    // Navigation folds chapters/bookmarks. Delete Scratch Axis is offered only for an empty, shown scratch
    // axis, so fold a "deletable scratch" mask: a scratch axis gaining its first action, or being shown/
    // hidden, flips which commands exist. (Select/toggle/tool-options moved to the registry-resident set —
    // registryProviderSignature owns those; the active axis and active mode no longer gate this set.)
    uint64_t h = navigationSignature(project);
    uint32_t deletableScratchMask = 0;
    for (auto i = static_cast<size_t>(StandardAxis::S0); i < kStandardAxisCount; ++i)
        if (project.axes[i].showInStrip && project.axes[i].actions.empty())
            deletableScratchMask |= (1u << i);
    h = hashBytes(h, &deletableScratchMask, sizeof(deletableScratchMask));
    // Dynamic titles are localized (axis descriptors via localizedAxisName, chapter/bookmark fallbacks via
    // Str::CmdDyn*) and baked into each Command's std::string at build time, so a UI-language switch — which
    // leaves project state untouched — must still invalidate the cache. (navigationSignature stays
    // language-blind; the full signature is what gates the cache.)
    return hashStr(h, ofs::loc::Translator::instance().activeLanguage());
}

} // namespace ofs
