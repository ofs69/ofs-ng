#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace ofs {

// On-disk schema version for layouts.json. Bump on an incompatible change; LayoutStore::load
// refuses a file newer than this rather than silently misreading fields.
inline constexpr int kLayoutStoreVersion = 1;

// A named docking-layout snapshot. `ini` is the verbatim string from
// ImGui::SaveIniSettingsToMemory (window positions + dock arrangement).
struct DockLayoutPreset {
    std::string name;
    std::string ini;
};

void to_json(nlohmann::json &j, const DockLayoutPreset &p);
void from_json(const nlohmann::json &j, DockLayoutPreset &p);

// User docking layouts plus the active-selection + lock state. Persisted to its own
// `layouts.json` in the pref dir, separate from AppSettings/settings.json. "Default" is the
// implicit built-in layout (rebuilt from DockBuilder) and is never stored in `layouts`.
struct LayoutStore {
    std::vector<DockLayoutPreset> layouts;
    std::string activeLayoutName = "Default";
    bool locked = false;

    static LayoutStore load();
    void save() const;
};

void to_json(nlohmann::json &j, const LayoutStore &s);
void from_json(const nlohmann::json &j, LayoutStore &s);

} // namespace ofs
