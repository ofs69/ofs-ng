#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>
// Full json (CustomMetadataField stores a json value by value), routed through JsonImGui so the
// adl_serializer<ImVec2>/<ImColor> specializations are always declared alongside the full-json
// definition. This header is an early json-introducer in many TUs; bundling the specializations here
// prevents an implicit adl_serializer<ImVec2> instantiation from racing ahead of them.
#include "Util/JsonImGui.h"

namespace ofs {

// A metadata key/value not in the standard set — typically imported from another program. The value
// is stored verbatim as any JSON type (null/bool/number/string/array/object), so round-trips are
// lossless. Written inline at the top level of the metadata object.
struct CustomMetadataField {
    std::string key;
    nlohmann::json value;
    bool operator==(const CustomMetadataField &) const = default;
};

struct FunscriptMetadata {
    std::string title;
    std::string creator;
    std::string scriptUrl;
    std::string videoUrl;
    std::string description;
    std::string notes;
    std::vector<std::string> tags;
    std::vector<std::string> performers;
    std::string license; // "", "Free", or "Paid"
    std::vector<CustomMetadataField> customFields;

    bool operator==(const FunscriptMetadata &) const = default;
};

// Inline read/write of custom (non-standard) fields, shared by both metadata structs. Each excludes
// its own standard key set: writeCustomFields skips fields whose key is empty or standard (the
// standard field wins, avoiding collisions); readCustomFields captures every object key not standard.
void writeCustomFields(nlohmann::json &obj, const std::vector<CustomMetadataField> &fields,
                       std::initializer_list<std::string_view> standardKeys);
void readCustomFields(const nlohmann::json &obj, std::vector<CustomMetadataField> &out,
                      std::initializer_list<std::string_view> standardKeys);

void to_json(nlohmann::json &j, const FunscriptMetadata &m);
void from_json(const nlohmann::json &j, FunscriptMetadata &m);

} // namespace ofs
