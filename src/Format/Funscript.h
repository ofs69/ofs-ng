#pragma once

#include "Core/BookmarkChapterState.h"
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

    // Funscript-format header fields describing the on-disk encoding (version/inverted/range/type); the
    // editor never shows or mutates these — read on load, written with these defaults on save, living
    // inside the "metadata" json object on disk per the funscript spec.
    std::string version = "1.0";
    bool inverted = false;
    int range = 100;
    std::string type = "basic";
    // Media length in whole seconds, stamped from the loaded video (or the media-less timeline) at export
    // time. Seconds — not the millisecond unit Action::at uses — to match the funscript metadata.duration
    // convention other tools write. 0 when unknown.
    int64_t duration = 0;

    // The shared document metadata (same struct the editor and project files use). On disk the funscript
    // spells the URLs snake_case (script_url/video_url) — that mapping happens in to_json/from_json.
    FunscriptMetadata metadata;

    // Standard funscript metadata.bookmarks / metadata.chapters (the OFS interop convention). On disk the
    // times are "HH:MM:SS.mmm" strings; here they are seconds, converted in to_json/from_json. A Chapter's
    // color and scene-view are ofs-ng-only and never written to the funscript — only the .ofp project file
    // carries them, so imported chapters keep the default color.
    std::vector<Bookmark> bookmarks;
    std::vector<Chapter> chapters;

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
