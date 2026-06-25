#include "FunscriptMetadata.h"
#include <algorithm>
#include <nlohmann/json.hpp>

namespace ofs {

static bool isStandardKey(std::string_view key, std::initializer_list<std::string_view> standardKeys) {
    return std::ranges::find(standardKeys, key) != standardKeys.end();
}

void writeCustomFields(nlohmann::json &obj, const std::vector<CustomMetadataField> &fields,
                       std::initializer_list<std::string_view> standardKeys) {
    for (const auto &f : fields) {
        if (f.key.empty() || isStandardKey(f.key, standardKeys))
            continue;
        obj[f.key] = f.value;
    }
}

void readCustomFields(const nlohmann::json &obj, std::vector<CustomMetadataField> &out,
                      std::initializer_list<std::string_view> standardKeys) {
    if (!obj.is_object())
        return;
    for (const auto &[key, val] : obj.items()) {
        if (isStandardKey(key, standardKeys))
            continue;
        out.push_back({.key = key, .value = val});
    }
}

// Standard project-side keys (camelCase). Any other key in the metadata object is a custom field.
static constexpr std::initializer_list<std::string_view> kStandardKeys = {
    "title", "creator", "scriptUrl", "videoUrl", "description", "notes", "tags", "performers", "license"};

void to_json(nlohmann::json &j, const FunscriptMetadata &m) {
    j = {{"title", m.title},       {"creator", m.creator},         {"scriptUrl", m.scriptUrl},
         {"videoUrl", m.videoUrl}, {"description", m.description}, {"notes", m.notes},
         {"tags", m.tags},         {"performers", m.performers},   {"license", m.license}};
    writeCustomFields(j, m.customFields, kStandardKeys);
}

void from_json(const nlohmann::json &j, FunscriptMetadata &m) {
    m.title = j.value("title", "");
    m.creator = j.value("creator", "");
    m.scriptUrl = j.value("scriptUrl", "");
    m.videoUrl = j.value("videoUrl", "");
    m.description = j.value("description", "");
    m.notes = j.value("notes", "");
    m.tags = j.value("tags", std::vector<std::string>{});
    m.performers = j.value("performers", std::vector<std::string>{});
    m.license = j.value("license", "");
    readCustomFields(j, m.customFields, kStandardKeys);
}

} // namespace ofs
