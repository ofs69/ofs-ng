#include "Core/ExportConfig.h"

namespace ofs {

// Axes round-trip as their stable TCode tags, never the enum's numeric value — a config written by an
// older build must survive a reordering of StandardAxis.
void to_json(nlohmann::json &j, const ExportConfig &c) {
    std::vector<std::string> axisTags;
    axisTags.reserve(c.axes.size());
    for (const auto role : c.axes)
        axisTags.emplace_back(standardAxisTag(role));
    j = {{"format", c.format}, {"axes", axisTags}, {"outputPath", c.outputPath}};
}

void from_json(const nlohmann::json &j, ExportConfig &c) {
    c.format = j.value("format", 0);
    c.outputPath = j.value("outputPath", "");
    c.axes.clear();
    for (const auto &tag : j.value("axes", std::vector<std::string>{}))
        if (auto role = standardAxisFromTag(tag))
            c.axes.push_back(*role);
}

} // namespace ofs
