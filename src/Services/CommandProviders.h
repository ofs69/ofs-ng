#pragma once

#include "Services/CommandRegistry.h"
#include <cstdint>
#include <vector>

namespace ofs {

struct ScriptProject;
class EditModeRegistry;
class NavigatorRegistry;
class SelectionModeRegistry;

// ── Dynamic command providers ─────────────────────────────────────────────────────────────────────
//
// Providers read live ScriptProject state and append Commands. They split into two sets:
//   • palette-only (inRebindList=false, NOT in the registry) — positional navigation + scratch-axis delete,
//     rebuilt per-frame-ish by OfsApp when dynamicCommandsSignature changes;
//   • registry-resident (source=Dynamic) — mode switches + axis select/toggle + tool options, rebuilt
//     into the CommandRegistry when registryProviderSignature changes so they can carry a binding.
// Each generated command has a `run` closure that pushes the matching event. As rendered "Group: Title"
// in the palette the palette-only ones read:
//
//   "Go to Chapter: <name>"        -> SeekEvent{chapter.startTime}
//   "Go to Bookmark: <name>"       -> SeekEvent{bookmark.time}
//
// Off the per-frame hot path: call only when the project state changes (see dynamicCommandsSignature).
void buildNavigationCommands(const ScriptProject &project, std::vector<Command> &out);

// Palette-only axis management — Delete Scratch Axis only (select/toggle moved to the registry-resident
// buildAxisProviderCommands so they can be bound):
//
//   "Delete Scratch Axis: S0"      -> RemoveAxisEvent{role}                         (present empty scratch)
void buildAxisCommands(const ScriptProject &project, std::vector<Command> &out);

// Registry-resident axis commands — one Select Axis and one Toggle-in-Panel per axis (source=Dynamic),
// so a key can be bound to them. Present for every axis that exists (standard axes always; a scratch axis
// only while it exists); the contextual no-op (selecting the active axis, hiding L0, toggling an empty
// scratch) is gated by isEnabled, not by membership, so a binding survives while its target is active:
//
//   "Select Axis: R0 (Twist)"      -> AxisSelectedEvent{role}                 (enabled: shown & not active)
//   "Toggle in Panel: R0 (Twist)"  -> ToggleAxisPanelVisibilityEvent{role, !shown}   (enabled: not empty scratch; never
//   L0)
void buildAxisProviderCommands(const ScriptProject &project, std::vector<Command> &out);

// Interaction-mode switch commands — one per registered edit mode / navigator / selection mode. Unlike
// the other providers these are REGISTRY-RESIDENT (source=Dynamic) so they can carry a binding: OfsApp
// adds them to the CommandRegistry and refreshes them on a registryProviderSignature() change. They mirror
// the footer selectors; each pushes the host-owned SetActive*Event its router consumes:
//
//   "Switch Edit Mode: Native"        -> SetActiveEditModeEvent{id}
//   "Switch Navigator: Follow overlay"-> SetActiveNavigatorEvent{id}
//   "Switch Selection Mode: Native"   -> SetActiveSelectionModeEvent{id}
//
// Every registered mode is emitted, including the active one — its command is present but isEnabled=false
// (switching to it is a no-op), so a binding survives while that mode is active. Titles resolve like the
// footer labels: a native id gets a localized label, a plugin entry shows
// its (plugin-localized) display name, else its id. inRebindList=false ⇒ not listed in the rebind UI by
// default; the user opts in to bind one.
void buildModeSwitchCommands(const ScriptProject &project, const EditModeRegistry &editModes,
                             const NavigatorRegistry &navigators, const SelectionModeRegistry &selectionModes,
                             std::vector<Command> &out);

// Tool-options commands — one per *registered* mode (edit/nav/select) that supplies options (onUi),
// registry-resident (source=Dynamic) so a key can be bound to "open <this mode>'s options". Each pushes
// OpenToolOptionsEvent (which opens the active mode's options for its extension point), so a command is
// enabled only while its own mode is active — isEnabled gates that. A per-extension-point group (parallel
// to the "Switch *" groups) keeps modes that share a display name distinct and searchable:
//
//   "Edit Mode Options: Pattern stamp"  -> OpenToolOptionsEvent{Edit}     (enabled while Pattern stamp active)
//   "Navigator Options: Peaks"          -> OpenToolOptionsEvent{Navigator}
void buildToolOptionsCommands(const ScriptProject &project, const EditModeRegistry &editModes,
                              const NavigatorRegistry &navigators, const SelectionModeRegistry &selectionModes,
                              std::vector<Command> &out);

// All registry-resident provider commands (mode switches + axis select/toggle + tool options), in that
// order. OfsApp rebuilds these into the CommandRegistry (source=Dynamic) whenever registryProviderSignature
// changes — never per frame — so each can carry a binding.
void buildRegistryProviderCommands(const ScriptProject &project, const EditModeRegistry &editModes,
                                   const NavigatorRegistry &navigators, const SelectionModeRegistry &selectionModes,
                                   std::vector<Command> &out);

// Signature gating the registry-resident provider rebuild: the three mode SETS + scratch-axis existence
// + the UI language. Coarser than dynamicCommandsSignature on purpose — active mode/axis changes and
// panel-visibility flips on an existing axis are handled live by isEnabled, not a rebuild.
[[nodiscard]] uint64_t registryProviderSignature(const ScriptProject &project, const EditModeRegistry &editModes,
                                                 const NavigatorRegistry &navigators,
                                                 const SelectionModeRegistry &selectionModes);

// The per-frame, palette-only dynamic commands: navigation + scratch-axis deletion. OfsApp merges these
// into the palette each rebuild. Everything offered for binding (mode switches, axis select/toggle, tool
// options) is NOT here — it is registry-resident (buildRegistryProviderCommands). The mode registries are unused now
// but kept in the signature for call-site symmetry with the registry-resident builder.
void buildDynamicCommands(const ScriptProject &project, const EditModeRegistry &editModes,
                          const NavigatorRegistry &navigators, const SelectionModeRegistry &selectionModes,
                          std::vector<Command> &out);

// Cheap, allocation-free hash of the state the palette-only dynamic commands derive from (chapter/bookmark
// times+names and which scratch axes are empty-and-shown → deletable). OfsApp rebuilds its cached
// dynamic-command list only when this value changes, so the per-frame palette path never constructs
// strings/vectors (hot-path rule) yet stays current under any mutation. Seeded non-zero so an empty
// project never hashes to a default cache value. The mode registries are unused (select/toggle/tool-options
// moved to registryProviderSignature) but kept for call-site symmetry.
[[nodiscard]] uint64_t dynamicCommandsSignature(const ScriptProject &project, const EditModeRegistry &editModes,
                                                const NavigatorRegistry &navigators,
                                                const SelectionModeRegistry &selectionModes);

// Navigation-only subset of the signature (chapter/bookmark + present axes). Retained as a focused
// helper; dynamicCommandsSignature builds on it.
[[nodiscard]] uint64_t navigationSignature(const ScriptProject &project);

} // namespace ofs
