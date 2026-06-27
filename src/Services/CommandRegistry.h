#pragma once

#include "Core/EventQueue.h"
#include "Localization/TrKey.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ofs {

// Native = code-defined static command. Plugin = contributed by a loaded C# plugin. Custom =
// user-defined parameterized command (CustomCommandStore). Dynamic =
// a registry-resident provider command (a stable mode/axis switch). All four reuse removeBySource for
// wholesale refresh.
enum class CommandSource { Native, Plugin, Custom, Dynamic };

// A palette / rebind-UI category. Commands reference a group by its `name` string; the group carries
// the icon shown beside every command in it, so the icon is declared once per category instead of at
// each command site (and dynamically generated commands inherit it for free). `groupIcon` is an
// opaque UTF-8 glyph string (an ICON_* literal from UI/Icons.h) — the registry stays UI-agnostic and
// never interprets it.
struct CommandGroup {
    std::string name;
    const char *groupIcon = ""; // "" = no icon
    // Localized header text. `name` stays the canonical (English) grouping key and stable id; this is
    // the only part shown to the user. index 0 (InvalidTr) ⇒ no catalog key, fall back to `name` — so
    // a plugin's own group, registered without one, displays its raw name. Presentation metadata, like
    // groupIcon: the registry stores and resolves it but never derives behavior from it.
    TrKey displayName{0};
};

// Per-frame context for a held binding. Append fields here; never reorder/remove — this grows by
// struct, not signature, so a future hold source (analog pressure, the firing trigger, modifier
// state) is an added field, not a break that touches every command's tick.
struct HoldTickInfo {
    float dt;            // frame delta, seconds
    float elapsed;       // hold duration at end of this frame (== dt on the first tick)
    bool first;          // true only on the first tick of the hold (use this, not elapsed==dt)
    float analog = 1.0f; // 1.0 for digital keys/buttons; deflection magnitude (0..1) for analog axis sources
    // future fields append below — additive, no signature break
};

// A named, invokable action. Commands are the single action registry; bindings are separate.
// `run` is the one execution choke point — palette, keyboard, gamepad, and future menu bar all
// call CommandRegistry::run(id), never cmd.run directly.
struct Command {
    std::string id;    // namespaced: "core.save", "<plugin>.<action>"
    std::string group; // palette category / rebind-UI group
    // Display title. Native commands hold a TrKey (resolves the active language at render time); plugin
    // and dynamic commands hold an owned string, since they carry no catalog key. TrString::c_str()
    // resolves whichever it holds — no separate raw/localized pair to keep in step. Default-empty so a
    // default-constructed Command (PluginManager builds one field by field) stays valid.
    TrString title{std::string{}};
    // Optional dimmed secondary line under the title in the Shortcut window. Carries the canonical-action
    // summary for a custom command the user gave its own name, so the row still says what it does. Empty
    // for native/plugin/dynamic commands and for an unnamed custom command (whose title already is the
    // summary). An owned string, not a TrKey — a custom summary is composed at build time, not catalogued.
    std::string subtitle;
    CommandSource source = CommandSource::Native;
    // Listed in the Shortcut window's rebind table *by default* — the binding-UI counterpart of
    // `inPalette`. false ⇒ not offered for binding without the user asking (provider/window-opener
    // commands), but a trigger the user does assign still dispatches: this gates the default rebind
    // *listing*, never dispatch. Default differs by author on purpose: true here (a native command is
    // offered for binding unless it opts out), but the C# plugin API defaults it false — a plugin
    // command is palette-only unless it opts in (see Ofs.Api/Commands.cs). The asymmetry matches how
    // each surface is authored, not an oversight.
    bool inRebindList = true;
    bool inPalette = true; // listed in the command palette? (still subject to requiresProject + enabled())
    // Search-only aliases for the command palette, space-separated; appended to the fuzzy-match
    // haystack beside group + title, never displayed. Lets a synonym ("settings", "hotkeys") reach a
    // command whose visible label lacks the word. English-only by design. Default empty — the
    // overwhelming majority need none.
    std::string keywords;
    // Hidden from the palette while no project is open. Default true: almost every command acts on the
    // project, so the few genuinely global ones (open palette/preferences/keybindings) opt out by
    // setting this false. Plugin and dynamic commands never set it, so they inherit the safe default.
    bool requiresProject = true;
    std::function<void(EventQueue &)> run;

    // While-held handler. Non-null ⇒ the command is "holdable": called once per frame for each active
    // Hold binding to this command (BindingSystem::tickHolds). The command stays stateless — it derives
    // cadence (discrete, via holdRepeats) or velocity (continuous) from the info struct. Must return
    // within frame budget. null ⇒ a Hold binding falls back to firing run() once on press.
    std::function<void(EventQueue &eq, const HoldTickInfo &info)> tick;

    // Reserved context-enablement seam: null = always enabled. The palette skips a
    // command whose predicate returns false. No condition language — just a host-supplied callback.
    std::function<bool()> isEnabled;

    [[nodiscard]] bool enabled() const { return !isEnabled || isEnabled(); }
    [[nodiscard]] bool holdable() const { return static_cast<bool>(tick); }
};

// Insertion-ordered registry. Registration is freeze-safe (no event-handler mutation) — legal
// at init AND at runtime plugin (re)load. CommandRegistry::run(id) is the single invocation
// choke point; no source calls cmd.run directly.
class CommandRegistry {
  public:
    explicit CommandRegistry(EventQueue &eq) : eventQueue_(eq) {}

    void add(Command cmd) {
        // A command listed nowhere — neither the rebind UI nor the palette — can never be discovered to
        // bind or invoke. The plugin boundary rejects this (PluginManager::hostRegisterCommand); assert
        // the same invariant on the native path, which trusts its callers rather than returning an error.
        assert((cmd.inRebindList || cmd.inPalette) &&
               "command must be listed in the rebind UI or the palette, else it is undiscoverable");
        commands_.push_back(std::move(cmd));
    }

    // Register a category's display icon. Idempotent-ish: re-registering a name overwrites its icon.
    void addGroup(CommandGroup group) {
        for (auto &g : groups_)
            if (g.name == group.name) {
                g.groupIcon = group.groupIcon;
                g.displayName = group.displayName;
                return;
            }
        groups_.push_back(std::move(group));
    }

    // Icon for a group name, or "" if the group has none / is unregistered (e.g. a plugin's own group).
    [[nodiscard]] const char *groupIcon(const std::string &name) const {
        for (const auto &g : groups_)
            if (g.name == name)
                return g.groupIcon;
        return "";
    }

    // Localized display name for a group, or the raw `name` if the group is unregistered or carries no
    // catalog key (plugin groups). Mirrors groupIcon — the one place command-group text is resolved, so
    // the Shortcut window and command palette render a group identically in the active language.
    [[nodiscard]] const char *groupDisplayName(const std::string &name) const {
        for (const auto &g : groups_)
            if (g.name == name)
                return g.displayName.index != 0 ? g.displayName.c_str() : g.name.c_str();
        return name.c_str();
    }

    [[nodiscard]] const std::vector<CommandGroup> &groups() const { return groups_; }

    // No-op if id is missing or run is null — safe to call with stale ids (e.g. plugin unloaded).
    // Records the use for frecency ranking, so keyboard/gamepad dispatch counts the same as a palette
    // click (the palette invoke path bypasses run() for dynamic commands and calls recordUse directly).
    void run(const std::string &id) {
        recordUse(id);
        for (const auto &cmd : commands_) {
            if (cmd.id == id) {
                if (cmd.run)
                    cmd.run(eventQueue_);
                return;
            }
        }
    }

    // ── Transient command frecency (session-only, never serialized) ────────────────────────────────
    // Powers the command palette's "frequently / recently used first" ordering. A monotonic counter
    // stands in for wall-clock recency. Frequency dominates, recency breaks ties (see frecency()).

    void recordUse(const std::string &id) {
        Use &u = uses_[id];
        u.count += 1;
        u.lastSeq = ++seq_;
    }

    // Packed sort key: 0 if the command was never used this session, otherwise frequency in the high
    // bits and recency in the low bits — so a higher use count always wins, and among equal counts the
    // more recently used wins. O(1) hash lookup (NOT a scan of commands_): callers invoke it once per
    // command per frame while building the palette list.
    [[nodiscard]] uint64_t frecency(const std::string &id) const {
        const auto it = uses_.find(id);
        if (it == uses_.end())
            return 0;
        return (static_cast<uint64_t>(it->second.count) << 40) | (it->second.lastSeq & 0xFFFFFFFFFFull);
    }

    void removeByGroup(const std::string &group) {
        std::erase_if(commands_, [&group](const Command &c) { return c.group == group; });
    }

    void removeBySource(CommandSource source) {
        std::erase_if(commands_, [source](const Command &c) { return c.source == source; });
    }

    // Validity window: the returned pointer points into commands_, which add() / removeByGroup() /
    // removeBySource() mutate — a vector realloc or erase invalidates it. Treat the result as valid only
    // within the current event handler / frame; never cache a Command* across a plugin or custom-command
    // (re)load. Look up by id again when in doubt.
    [[nodiscard]] const Command *find(const std::string &id) const {
        for (const auto &cmd : commands_)
            if (cmd.id == id)
                return &cmd;
        return nullptr;
    }

    // Same validity window as find(): the span dangles across add()/removeByGroup()/removeBySource().
    // Read it within the frame; don't retain references to its elements across a plugin/custom reload.
    [[nodiscard]] const std::vector<Command> &all() const { return commands_; }

  private:
    struct Use {
        uint32_t count = 0;
        uint64_t lastSeq = 0;
    };

    EventQueue &eventQueue_;
    std::vector<Command> commands_;
    std::vector<CommandGroup> groups_;
    std::unordered_map<std::string, Use> uses_; // transient frecency, keyed by command id
    uint64_t seq_ = 0;                          // monotonic use counter (recency proxy)
};

} // namespace ofs
