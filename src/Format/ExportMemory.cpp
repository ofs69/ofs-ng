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
    try {
        if (auto text = ofs::util::readFile(exportMemoryPath()))
            memory.entries = nlohmann::json::parse(*text).value("exports", std::vector<ProjectExportMemory>{});
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to load export_configs.json: {}", e.what());
    }
    // A hand-edited file could carry more than the cap; trim on read so the bound holds regardless of
    // how the entries got there.
    if (memory.entries.size() > kMaxExportMemories)
        memory.entries.resize(kMaxExportMemories);
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
