#pragma once

#include "Localization/TrKey.h"
#include "Services/CommandRegistry.h"
#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace ofs {

struct CustomCommand;
struct ScriptProject;
struct AppSettings;

// One kind of user-definable command, self-describing: it owns its kind-picker label, the editor UI for
// its parameters, the factory that turns a definition into a registry Command, and its summary. The kinds
// (step / move-position / move-time) register through registerBuiltinCommandTemplates — an in-tree, closed
// set. Making it table-driven is what lets the Shortcut window render any kind's editor (and the store
// (de)serialize it) with no hardcoded switch; params (de)serialization is generic, so a kind needs no
// per-kind wire hook.
struct CustomCommandTemplate {
    std::string key;      // stable serialization id, e.g. "step" — also the JSON "kind" value + widget-id slug
    TrKey displayName{0}; // kind-picker label

    // Render this kind's parameter widgets, editing its opaque param bag in place (the generic name + kind
    // picker are drawn by the caller). Main-thread ImGui like every other render function — provider-owned,
    // mirroring the plugin node-UI callbacks that also live in Services. Must read defensively (the bag may
    // be empty / from an older file): `params.value("reps", 1)` with the default supplies missing keys.
    std::function<void(nlohmann::json &params)> renderEditor;

    // Definition → registry Command. The owning project/appSettings are captured at registration (they
    // outlive the registry — OfsApp members), so this needs only the definition.
    std::function<Command(const CustomCommand &def)> build;

    // One-line localized description of `params` (e.g. "Step forward ×3 (frame)"). Used as the row title
    // when the user left the name blank, and as the dimmed subtitle otherwise. Returns a frame-arena
    // (fmtScratch) pointer — main-thread only, never stored across frames; callers copy it into an owned
    // string the same frame. Params (de)serialization is generic — the bag *is* the wire format — so there
    // is no per-template write/read hook.
    std::function<const char *(const nlohmann::json &params)> summary;
};

// Ordered registry of command templates. Insertion order is the kind-picker order; lookup by key resolves
// a definition's template at build / render / (de)serialize time. Populated once at init and read-only after.
class CustomCommandTemplateRegistry {
  public:
    void add(CustomCommandTemplate t) { templates_.push_back(std::move(t)); }

    [[nodiscard]] const CustomCommandTemplate *find(const std::string &key) const {
        for (const auto &t : templates_)
            if (t.key == key)
                return &t;
        return nullptr;
    }

    [[nodiscard]] const std::vector<CustomCommandTemplate> &templates() const { return templates_; }

  private:
    std::vector<CustomCommandTemplate> templates_;
};

// Register the first-party templates (step / move-position / move-time). `project` and `appSettings` are
// captured by reference into the build/render closures and must outlive the registry (OfsApp members).
// The push/scaling logic the built-in step & move verbs use lives here once.
void registerBuiltinCommandTemplates(CustomCommandTemplateRegistry &registry, ScriptProject &project,
                                     const AppSettings &appSettings);

} // namespace ofs
