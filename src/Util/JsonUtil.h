#pragma once

#include "Util/FileUtil.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>

// Small helpers for the lossy, forward-tolerant way we read JSON documents. Our load paths
// must survive foreign, legacy and hand-edited files, so a field that is absent or holds the
// wrong type should fall back to a default rather than throw. nlohmann's own `j.value()`
// throws on a present-but-mismatched type, which is why the raw code is littered with
// `j.contains(k) && j[k].is_array()` guards before every `.get<T>()`. These collapse that.
//
// All of this is file I/O, not a per-frame hot path, so the allocations are fine here.
namespace ofs::util {

// Like nlohmann's j.value(key, fallback), but returns `fallback` when the key is present with
// a mismatched type instead of throwing. A missing key, a null, or a type that cannot convert
// to T all yield the fallback; only a well-typed value is returned.
template <class T> T jsonValueOr(const nlohmann::json &j, std::string_view key, T fallback) {
    const auto it = j.find(key);
    if (it == j.end())
        return fallback;
    try {
        return it->get<T>();
    } catch (const nlohmann::json::exception &) {
        return fallback;
    }
}

// Pointer to j[key] if present and an array, else nullptr — for the nested-traversal pattern
// `if (const auto it = j.find(k); it != j.end() && it->is_array())`.
inline const nlohmann::json *jsonArrayIf(const nlohmann::json &j, std::string_view key) {
    const auto it = j.find(key);
    return (it != j.end() && it->is_array()) ? &*it : nullptr;
}

inline const nlohmann::json *jsonObjectIf(const nlohmann::json &j, std::string_view key) {
    const auto it = j.find(key);
    return (it != j.end() && it->is_object()) ? &*it : nullptr;
}

// Read and parse a JSON file. Returns nullopt (after logging) when the file is missing or
// holds invalid JSON. `what` names the document in the log line (e.g. "project", "settings").
inline std::optional<nlohmann::json> parseJsonFile(const std::filesystem::path &path, std::string_view what) {
    auto text = readFile(path);
    if (!text) {
        OFS_CORE_ERROR("Failed to open {} file: {}", what, toUtf8(path));
        return std::nullopt;
    }
    try {
        return nlohmann::json::parse(*text);
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to parse {} file: {}. Error: {}", what, toUtf8(path), e.what());
        return std::nullopt;
    }
}

// parseJsonFile + deserialize to T via from_json/adl_serializer. Same logging contract; a
// deserialization failure is reported and yields nullopt.
template <class T> std::optional<T> loadJsonFile(const std::filesystem::path &path, std::string_view what) {
    auto j = parseJsonFile(path, what);
    if (!j)
        return std::nullopt;
    try {
        return j->get<T>();
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to read {} file: {}. Error: {}", what, toUtf8(path), e.what());
        return std::nullopt;
    }
}

} // namespace ofs::util
