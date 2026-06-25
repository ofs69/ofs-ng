#include "FixtureCompare.h"
#include <cmath>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>

#ifndef OFS_TESTS_DIR
#error "OFS_TESTS_DIR must be defined for fixture tests"
#endif

namespace ofs::test {

std::filesystem::path fixturePath(const std::string &name) {
    return std::filesystem::path(OFS_TESTS_DIR) / "fixtures" / name;
}

namespace {

// axisId -> (timeMs -> pos), with timestamps normalized to whole milliseconds.
using NormalizedAxes = std::map<std::string, std::map<long long, int>>;

long long normalizeMs(const nlohmann::json &at) {
    // funscript "at" is milliseconds; foreign files may store it as a float. Rounding to the
    // nearest whole millisecond is the 3-decimal-of-seconds precision the plan calls for.
    return std::llround(at.get<double>());
}

void collectActions(const nlohmann::json &actionsArr, std::map<long long, int> &out) {
    if (!actionsArr.is_array())
        return;
    for (const auto &a : actionsArr)
        if (a.contains("at") && a.contains("pos"))
            out[normalizeMs(a["at"])] = a["pos"].get<int>();
}

NormalizedAxes normalize(const nlohmann::json &j) {
    NormalizedAxes axes;
    if (j.contains("actions"))
        collectActions(j["actions"], axes["L0"]);
    if (j.contains("axes") && j["axes"].is_array())
        for (const auto &ax : j["axes"]) {
            const std::string id = ax.value("id", "");
            if (!id.empty() && ax.contains("actions"))
                collectActions(ax["actions"], axes[id]);
        }
    if (j.contains("channels") && j["channels"].is_object())
        for (const auto &[key, val] : j["channels"].items())
            if (val.contains("actions"))
                collectActions(val["actions"], axes[key]);
    // Drop empty axes so "absent" and "present but empty" compare equal.
    std::erase_if(axes, [](const auto &kv) { return kv.second.empty(); });
    return axes;
}

bool loadJson(const std::filesystem::path &path, nlohmann::json &out, std::string *diff) {
    std::ifstream f(path);
    if (!f.is_open()) {
        if (diff)
            *diff = "could not open " + path.string();
        return false;
    }
    try {
        f >> out;
    } catch (const std::exception &e) {
        if (diff)
            *diff = "invalid JSON in " + path.string() + ": " + e.what();
        return false;
    }
    return true;
}

} // namespace

bool compareFunscriptFiles(const std::filesystem::path &actual, const std::filesystem::path &expected,
                           std::string *diff) {
    nlohmann::json ja;
    nlohmann::json je;
    if (!loadJson(actual, ja, diff) || !loadJson(expected, je, diff))
        return false;

    const NormalizedAxes na = normalize(ja);
    const NormalizedAxes ne = normalize(je);

    if (na.size() != ne.size()) {
        if (diff)
            *diff = "axis count " + std::to_string(na.size()) + " != expected " + std::to_string(ne.size());
        return false;
    }

    for (const auto &[axisId, expActions] : ne) {
        auto it = na.find(axisId);
        if (it == na.end()) {
            if (diff)
                *diff = "missing axis '" + axisId + "'";
            return false;
        }
        const auto &actActions = it->second;
        if (actActions.size() != expActions.size()) {
            if (diff)
                *diff = "axis '" + axisId + "': action count " + std::to_string(actActions.size()) + " != expected " +
                        std::to_string(expActions.size());
            return false;
        }
        for (const auto &[t, pos] : expActions) {
            auto ait = actActions.find(t);
            if (ait == actActions.end()) {
                if (diff)
                    *diff = "axis '" + axisId + "': missing action at " + std::to_string(t) + "ms";
                return false;
            }
            if (ait->second != pos) {
                if (diff)
                    *diff = "axis '" + axisId + "' at " + std::to_string(t) + "ms: pos " + std::to_string(ait->second) +
                            " != expected " + std::to_string(pos);
                return false;
            }
        }
    }

    return true;
}

} // namespace ofs::test
