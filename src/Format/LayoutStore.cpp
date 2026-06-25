#include "LayoutStore.h"
#include "Util/FileUtil.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"

namespace ofs {
static std::filesystem::path getLayoutsPath() {
    return ofs::util::getPrefPath() / "layouts.json";
}

void to_json(nlohmann::json &j, const DockLayoutPreset &p) {
    j = {{"name", p.name}, {"ini", p.ini}};
}

void from_json(const nlohmann::json &j, DockLayoutPreset &p) {
    p.name = j.value("name", "");
    p.ini = j.value("ini", "");
}

void to_json(nlohmann::json &j, const LayoutStore &s) {
    j = nlohmann::json::object(
        {{"layouts", s.layouts}, {"activeLayoutName", s.activeLayoutName}, {"locked", s.locked}});
}

void from_json(const nlohmann::json &j, LayoutStore &s) {
    s.layouts = j.value("layouts", std::vector<DockLayoutPreset>{});
    s.activeLayoutName = j.value("activeLayoutName", std::string("Default"));
    s.locked = j.value("locked", false);
}

LayoutStore LayoutStore::load() {
    LayoutStore store;
    std::filesystem::path path = getLayoutsPath();
    try {
        auto text = ofs::util::readFile(path);
        if (text) {
            from_json(nlohmann::json::parse(*text), store);
        }
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to load layouts: {}", e.what());
    }
    return store;
}

void LayoutStore::save() const {
    std::filesystem::path path = getLayoutsPath();
    try {
        nlohmann::json j;
        to_json(j, *this);
        ofs::util::writeFile(path, j.dump(4));
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to save layouts: {}", e.what());
    }
}
} // namespace ofs
