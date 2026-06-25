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

// One kind of user-definable command, expressed as a self-describing provider: it owns its kind-picker
// label, the editor UI for its parameters, the factory that turns a definition into a registry Command,
// and its param (de)serialization. The first-party kinds (step / move-position / move-time) register
// through registerBuiltinCommandTemplates; a plugin-contributed template would register the same way
// (the reserved seam). Making the kind set table-driven is what lets the
// Shortcut window render any kind's editor — and the store build/serialize it — with no hardcoded switch.
struct CustomCommandTemplate {
    std::string key;      // stable serialization id, e.g. "step" — also the JSON "kind" value + widget-id slug
    TrKey displayName{0}; // kind-picker label

    // Render this kind's parameter widgets into `draft` (the generic name + kind picker are drawn by the
    // caller). Main-thread ImGui like every other render function — provider-owned, mirroring the plugin
    // node-UI callbacks that also live in Services.
    std::function<void(CustomCommand &draft)> renderEditor;

    // Definition → registry Command. The owning project/appSettings are captured at registration (they
    // outlive the registry — OfsApp members), so this needs only the definition.
    std::function<Command(const CustomCommand &def)> build;

    // This kind's params ↔ JSON. The caller writes/reads the shared id/name/kind; these add or read only
    // the fields this kind owns. readParams returns false to forward-tolerantly skip a malformed entry.
    std::function<void(const CustomCommand &def, nlohmann::json &out)> writeParams;
    std::function<bool(const nlohmann::json &in, CustomCommand &def)> readParams;
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
