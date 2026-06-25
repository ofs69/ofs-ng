#pragma once

#include "Core/FunscriptMetadata.h"
#include "Core/ScriptAxisAction.h"
#include "Core/VectorSet.h"
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace ofs {
struct Funscript {
    struct Action {
        int64_t at = 0; // milliseconds
        int pos = 0;    // 0-100

        NLOHMANN_DEFINE_TYPE_INTRUSIVE(Action, at, pos)
    };

    struct AxisEntry {
        std::string id;
        std::vector<Action> actions;
    };

    std::vector<Action> actions;

    // Funscript-format header fields. These describe the on-disk encoding (version/inverted/range/type)
    // or are file artifacts (duration), not part of the editable document model — the editor never shows
    // or mutates them. They are read on load and written with these defaults on save, living inside the
    // "metadata" json object on disk per the funscript spec.
    std::string version = "1.0";
    bool inverted = false;
    int range = 100;
    std::string type = "basic";
    int64_t duration = 0;

    // The shared document metadata (same struct the editor and project files use). On disk the funscript
    // spells the URLs snake_case (script_url/video_url) — that mapping happens in to_json/from_json.
    FunscriptMetadata metadata;

    std::vector<AxisEntry> axes;                         // funscript 1.1: "axes" array
    std::map<std::string, std::vector<Action>> channels; // funscript 2.0: "channels" object

    [[nodiscard]] bool isMultiAxis() const { return !axes.empty() || !channels.empty(); }

    static std::optional<Funscript> load(const std::filesystem::path &path);
    bool save(const std::filesystem::path &path) const;

    // Single-axis (root actions only)
    [[nodiscard]] VectorSet<ScriptAxisAction> toActions() const;
    static Funscript fromActions(const VectorSet<ScriptAxisAction> &actions);

    // Multi-axis: returns map from axis tag → actions; root actions appear under "L0"
    [[nodiscard]] std::map<std::string, VectorSet<ScriptAxisAction>> toAllAxes() const;

    // Build a funscript 1.1 file (axes[]); first entry with tag "L0" goes to root actions
    static Funscript fromAxes11(const std::vector<std::pair<std::string, VectorSet<ScriptAxisAction>>> &axes);

    // Build a funscript 2.0 file (channels{}); first entry with tag "L0" goes to root actions
    static Funscript fromAxes20(const std::vector<std::pair<std::string, VectorSet<ScriptAxisAction>>> &axes);
};

void to_json(nlohmann::json &j, const Funscript &f);

void from_json(const nlohmann::json &j, Funscript &f);
} // namespace ofs
