#include "Format/ExportMemory.h"
#include "Util/FileUtil.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"
#include <algorithm>

namespace ofs {
namespace {
std::filesystem::path exportMemoryPath() {
    return ofs::util::getPrefPath() / "export_configs.json";
}

// COMPAT(2026-09-12): v0.2.9 kept these entries under "lastExports" in settings.json. Read them back
// once, when this file does not exist yet, so upgrading doesn't forget every project's Quick Export
// target. Removable once no v0.2.9 settings.json is in circulation.
std::vector<ProjectExportMemory> readLegacySettingsEntries() {
    try {
        auto text = ofs::util::readFile(ofs::util::getPrefPath() / "settings.json");
        if (!text)
            return {};
        const nlohmann::json j = nlohmann::json::parse(*text);
        return j.value("lastExports", std::vector<ProjectExportMemory>{});
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to read legacy export configs from settings.json: {}", e.what());
        return {};
    }
}
} // namespace

void to_json(nlohmann::json &j, const ProjectExportMemory &m) {
    j = {{"projectPath", m.projectPath}, {"config", m.config}};
}

void from_json(const nlohmann::json &j, ProjectExportMemory &m) {
    m.projectPath = j.value("projectPath", "");
    m.config = j.value("config", ExportConfig{});
}

ExportMemory ExportMemory::load() {
    ExportMemory memory;
    bool migrated = false;
    try {
        if (auto text = ofs::util::readFile(exportMemoryPath()))
            memory.entries = nlohmann::json::parse(*text).value("exports", std::vector<ProjectExportMemory>{});
        else {
            memory.entries = readLegacySettingsEntries();
            migrated = !memory.entries.empty();
        }
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to load export_configs.json: {}", e.what());
    }
    // A hand-edited file could carry more than the cap; trim on read so the bound holds regardless of
    // how the entries got there.
    if (memory.entries.size() > kMaxExportMemories)
        memory.entries.resize(kMaxExportMemories);
    // Write the migrated entries out now rather than waiting for the next export: the settings save that
    // follows any preference change drops the legacy key, and this file is then the only copy.
    if (migrated)
        memory.save();
    return memory;
}

void ExportMemory::save() const {
    try {
        const nlohmann::json j = nlohmann::json::object({{"exports", entries}});
        ofs::util::writeFileAtomic(exportMemoryPath(), j.dump(2));
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to save export_configs.json: {}", e.what());
    }
}

void ExportMemory::remember(std::string projectPath, ExportConfig config) {
    if (projectPath.empty())
        return;
    std::erase_if(entries, [&](const ProjectExportMemory &m) { return m.projectPath == projectPath; });
    entries.insert(entries.begin(),
                   ProjectExportMemory{.projectPath = std::move(projectPath), .config = std::move(config)});
    if (entries.size() > kMaxExportMemories)
        entries.resize(kMaxExportMemories);
}

const ExportConfig *ExportMemory::find(const std::string &projectPath) const {
    if (projectPath.empty())
        return nullptr;
    const auto it = std::ranges::find(entries, projectPath, &ProjectExportMemory::projectPath);
    return it == entries.end() ? nullptr : &it->config;
}

} // namespace ofs
