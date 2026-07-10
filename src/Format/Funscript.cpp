#include "Funscript.h"
#include "Util/FileUtil.h"
#include "Util/JsonUtil.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"
#include <algorithm>
#include <cmath>

namespace ofs {

// Funscript stores integer milliseconds on disk; the editor works in seconds (double). Convert only at
// this boundary. seconds→ms rounds to the nearest millisecond (not truncate) so a sub-ms remainder isn't
// lost on export.
static constexpr double msToSeconds(int64_t ms) {
    return static_cast<double>(ms) / 1000.0;
}
static int64_t secondsToMs(double seconds) {
    return std::llround(seconds * 1000.0);
}

// Standard on-disk keys (snake_case). Any other key in the metadata object is a custom field.
static constexpr std::initializer_list<std::string_view> kFunscriptStandardKeys = {
    "version", "inverted",  "range",      "type", "title",      "creator", "description",
    "notes",   "video_url", "script_url", "tags", "performers", "license", "duration"};

// Build the on-disk "metadata" object: the shared document metadata (URLs spelled snake_case here) plus
// the funscript-format header fields carried on Funscript. The editor-facing camelCase serialization of
// FunscriptMetadata lives in FunscriptMetadata.cpp and is used by the .ofp project format, not here.
static nlohmann::json funscriptMetadataToJson(const Funscript &f) {
    const FunscriptMetadata &m = f.metadata;
    nlohmann::json j = {{"version", f.version},       {"inverted", f.inverted},
                        {"range", f.range},           {"type", f.type},
                        {"duration", f.duration},     {"title", m.title},
                        {"creator", m.creator},       {"description", m.description},
                        {"notes", m.notes},           {"video_url", m.videoUrl},
                        {"script_url", m.scriptUrl},  {"tags", m.tags},
                        {"performers", m.performers}, {"license", m.license}};
    writeCustomFields(j, m.customFields, kFunscriptStandardKeys);
    return j;
}

static void funscriptMetadataFromJson(const nlohmann::json &j, Funscript &f) {
    f.version = j.value("version", "1.0");
    f.inverted = j.value("inverted", false);
    f.range = j.value("range", 100);
    f.type = j.value("type", "basic");
    f.duration = j.value("duration", (int64_t)0);

    FunscriptMetadata &m = f.metadata;
    m.title = j.value("title", "");
    m.creator = j.value("creator", "");
    m.description = j.value("description", "");
    m.notes = j.value("notes", "");
    m.videoUrl = j.value("video_url", "");
    m.scriptUrl = j.value("script_url", "");
    m.tags = j.value("tags", std::vector<std::string>{});
    m.performers = j.value("performers", std::vector<std::string>{});
    m.license = j.value("license", "");
    readCustomFields(j, m.customFields, kFunscriptStandardKeys);
}

void to_json(nlohmann::json &j, const Funscript &f) {
    j = nlohmann::json{{"actions", f.actions}, {"metadata", funscriptMetadataToJson(f)}};
    if (!f.axes.empty()) {
        auto axesArr = nlohmann::json::array();
        for (const auto &ax : f.axes)
            axesArr.push_back({{"id", ax.id}, {"actions", ax.actions}});
        j["axes"] = std::move(axesArr);
    }
    if (!f.channels.empty()) {
        auto channelsObj = nlohmann::json::object();
        for (const auto &[key, acts] : f.channels)
            channelsObj[key] = {{"actions", acts}};
        j["channels"] = std::move(channelsObj);
    }
}

void from_json(const nlohmann::json &j, Funscript &f) {
    using ofs::util::jsonArrayIf;
    using ofs::util::jsonObjectIf;
    using ofs::util::jsonValueOr;

    f.actions = jsonValueOr(j, "actions", std::vector<Funscript::Action>{});
    if (const auto it = j.find("metadata"); it != j.end() && it->is_object())
        funscriptMetadataFromJson(*it, f);

    // funscript 1.1: "axes" array
    f.axes.clear();
    if (const auto *axes = jsonArrayIf(j, "axes")) {
        for (const auto &ax : *axes) {
            Funscript::AxisEntry entry;
            entry.id = jsonValueOr(ax, "id", std::string{});
            entry.actions = jsonValueOr(ax, "actions", std::vector<Funscript::Action>{});
            if (!entry.id.empty())
                f.axes.push_back(std::move(entry));
        }
    }

    // funscript 2.0: "channels" object
    f.channels.clear();
    if (const auto *channels = jsonObjectIf(j, "channels")) {
        for (const auto &[key, val] : channels->items())
            if (jsonArrayIf(val, "actions"))
                f.channels[key] = val["actions"].get<std::vector<Funscript::Action>>();
    }
}

std::optional<Funscript> Funscript::load(const std::filesystem::path &path) {
    return ofs::util::loadJsonFile<Funscript>(path, "funscript");
}

bool Funscript::save(const std::filesystem::path &path) const {
    try {
        // Emit "metadata" first so the small header block sits at the top of the file — instantly
        // readable/editable in a text editor instead of buried below the whole actions array. to_json
        // builds a plain (alphabetically key-sorted) object; copy its members into an ordered_json in the
        // desired on-disk order. (Reordering, not re-sorting: keys within metadata stay as to_json emits.)
        nlohmann::json body = *this;
        nlohmann::ordered_json j;
        j["metadata"] = body["metadata"];
        j["actions"] = body["actions"];
        if (body.contains("axes"))
            j["axes"] = body["axes"];
        if (body.contains("channels"))
            j["channels"] = body["channels"];
        if (!ofs::util::writeFileAtomic(path, j.dump())) {
            OFS_CORE_ERROR("Failed to open funscript file for writing: {}", ofs::util::toUtf8(path));
            return false;
        }
        return true;
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to serialize funscript: {}", e.what());
        return false;
    }
}

VectorSet<ScriptAxisAction> Funscript::toActions() const {
    auto mapAction = [](const Action &action) {
        return clampedAction(msToSeconds(action.at), action.pos); // import boundary
    };
    auto transformedView = actions | std::views::transform(mapAction);
    VectorSet<ScriptAxisAction> result(transformedView.begin(), transformedView.end());
    return result;
}

Funscript Funscript::fromActions(const VectorSet<ScriptAxisAction> &scriptActions) {
    Funscript fs;
    fs.actions.reserve(scriptActions.size());
    // Round seconds→ms to the nearest millisecond (not truncate) so a timestamp like 3.4567 s
    // exports as 3457 ms rather than losing the sub-ms remainder.
    for (const auto &action : scriptActions)
        fs.actions.push_back({.at = secondsToMs(action.at), .pos = action.pos});
    return fs;
}

static std::vector<Funscript::Action> toActionVec(const VectorSet<ScriptAxisAction> &acts) {
    std::vector<Funscript::Action> result;
    result.reserve(acts.size());
    for (const auto &a : acts)
        result.push_back({.at = secondsToMs(a.at), .pos = a.pos});
    return result;
}

static VectorSet<ScriptAxisAction> fromActionVec(const std::vector<Funscript::Action> &acts) {
    auto mapped = acts | std::views::transform([](const Funscript::Action &a) {
                      return clampedAction(msToSeconds(a.at), a.pos); // import boundary
                  });
    return {mapped.begin(), mapped.end()};
}

std::map<std::string, VectorSet<ScriptAxisAction>> Funscript::toAllAxes() const {
    std::map<std::string, VectorSet<ScriptAxisAction>> result;
    if (!actions.empty())
        result["L0"] = toActions();
    for (const auto &ax : axes)
        result[ax.id] = fromActionVec(ax.actions);
    for (const auto &[key, acts] : channels)
        result[key] = fromActionVec(acts);
    return result;
}

static Funscript buildMultiAxis(const std::vector<std::pair<std::string, VectorSet<ScriptAxisAction>>> &axes,
                                bool useChannels) {
    Funscript fs;
    if (axes.empty())
        return fs;

    // L0 is canonical primary; fall back to first entry
    const auto l0 = std::ranges::find_if(axes, [](const auto &ax) { return ax.first == "L0"; });
    const size_t primaryIdx = l0 != axes.end() ? static_cast<size_t>(std::distance(axes.begin(), l0)) : 0;

    fs.actions = toActionVec(axes[primaryIdx].second);

    for (size_t i = 0; i < axes.size(); ++i) {
        if (i == primaryIdx)
            continue;
        const auto &[tag, acts] = axes[i];
        if (useChannels)
            fs.channels[tag] = toActionVec(acts);
        else
            fs.axes.push_back({.id = tag, .actions = toActionVec(acts)});
    }
    return fs;
}

Funscript Funscript::fromAxes11(const std::vector<std::pair<std::string, VectorSet<ScriptAxisAction>>> &axes) {
    return buildMultiAxis(axes, false);
}

Funscript Funscript::fromAxes20(const std::vector<std::pair<std::string, VectorSet<ScriptAxisAction>>> &axes) {
    return buildMultiAxis(axes, true);
}
} // namespace ofs
