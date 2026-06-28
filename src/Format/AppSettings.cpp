#include "AppSettings.h"
#include "Util/FileUtil.h"
#include "Util/JsonImGui.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"
#include <SDL3/SDL.h>
#include <algorithm>

namespace ofs {
static std::filesystem::path getSettingsPath() {
    return ofs::util::getPrefPath() / "settings.json";
}

void to_json(nlohmann::json &j, const SimulatorState &s) {
    j = nlohmann::json::object({{"p1", s.p1},
                                {"p2", s.p2},
                                {"extraLinesCount", s.extraLinesCount},
                                {"lockedPosition", s.lockedPosition},
                                {"use3dSimulator", s.use3dSimulator},
                                {"sim3dPos", s.sim3dPos},
                                {"sim3dSize", s.sim3dSize},
                                {"enableIndicators", s.enableIndicators},
                                {"enablePosition", s.enablePosition},
                                {"enableHeightLines", s.enableHeightLines},
                                {"swayRange", s.swayRange},
                                {"strokeRange", s.strokeRange},
                                {"surgeRange", s.surgeRange},
                                {"pitchRange", s.pitchRange},
                                {"rollRange", s.rollRange},
                                {"twistRange", s.twistRange},
                                {"labels3dMask", s.labels3dMask.to_ulong()},
                                {"labels3dInDegrees", s.labels3dInDegrees}});
}

void from_json(const nlohmann::json &j, SimulatorState &s) {
    SimulatorState d;
    s.p1 = j.value("p1", d.p1);
    s.p2 = j.value("p2", d.p2);
    s.extraLinesCount = j.value("extraLinesCount", d.extraLinesCount);
    s.lockedPosition = j.value("lockedPosition", d.lockedPosition);
    s.use3dSimulator = j.value("use3dSimulator", d.use3dSimulator);
    s.sim3dPos = j.value("sim3dPos", d.sim3dPos);
    s.sim3dSize = j.value("sim3dSize", d.sim3dSize);
    s.enableIndicators = j.value("enableIndicators", d.enableIndicators);
    s.enablePosition = j.value("enablePosition", d.enablePosition);
    s.enableHeightLines = j.value("enableHeightLines", d.enableHeightLines);
    s.swayRange = j.value("swayRange", d.swayRange);
    s.strokeRange = j.value("strokeRange", d.strokeRange);
    s.surgeRange = j.value("surgeRange", d.surgeRange);
    s.pitchRange = j.value("pitchRange", d.pitchRange);
    s.rollRange = j.value("rollRange", d.rollRange);
    s.twistRange = j.value("twistRange", d.twistRange);
    s.labels3dMask = std::bitset<SimulatorState::kSim3dDofCount>(j.value("labels3dMask", d.labels3dMask.to_ulong()));
    s.labels3dInDegrees = j.value("labels3dInDegrees", d.labels3dInDegrees);
}

AppSettings AppSettings::load() {
    AppSettings settings;
    std::filesystem::path path = getSettingsPath();
    try {
        auto text = ofs::util::readFile(path);
        if (text) {
            const nlohmann::json j = nlohmann::json::parse(*text);
            // Refuse a file written by a newer, incompatible build rather than misreading fields.
            const int version = j.value("version", kAppSettingsVersion);
            if (version > kAppSettingsVersion) {
                OFS_CORE_ERROR("settings.json version {} is newer than supported version {}; using defaults.", version,
                               kAppSettingsVersion);
                return settings;
            }
            from_json(j, settings);
        }
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to load settings: {}", e.what());
    }
    return settings;
}

void AppSettings::save() const {
    std::filesystem::path path = getSettingsPath();
    try {
        nlohmann::json j;
        to_json(j, *this);
        ofs::util::writeFileAtomic(path, j.dump(4));
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to save settings: {}", e.what());
    }
}

void to_json(nlohmann::json &j, const InputSettings &s) {
    j = nlohmann::json::object({{"deadzone", s.deadzone}, {"smoothing", s.smoothing}});
}

void from_json(const nlohmann::json &j, InputSettings &s) {
    InputSettings d;
    s.deadzone = j.value("deadzone", d.deadzone);
    s.smoothing = j.value("smoothing", d.smoothing);
}

void to_json(nlohmann::json &j, const HoldRepeatSettings &s) {
    j = nlohmann::json::object(
        {{"initialDelay", s.initialDelay}, {"interval", s.interval}, {"accel", s.accel}, {"maxRateHz", s.maxRateHz}});
}

void from_json(const nlohmann::json &j, HoldRepeatSettings &s) {
    HoldRepeatSettings d;
    s.initialDelay = j.value("initialDelay", d.initialDelay);
    s.interval = j.value("interval", d.interval);
    s.accel = j.value("accel", d.accel);
    s.maxRateHz = j.value("maxRateHz", d.maxRateHz);
}

void to_json(nlohmann::json &j, const WindowGeometry &g) {
    j = nlohmann::json::object(
        {{"x", g.x}, {"y", g.y}, {"width", g.width}, {"height", g.height}, {"maximized", g.maximized}});
}

void from_json(const nlohmann::json &j, WindowGeometry &g) {
    WindowGeometry d;
    g.x = j.value("x", d.x);
    g.y = j.value("y", d.y);
    g.width = j.value("width", d.width);
    g.height = j.value("height", d.height);
    g.maximized = j.value("maximized", d.maximized);
}

void to_json(nlohmann::json &j, const MetadataPreset &p) {
    j = {{"name", p.name}, {"metadata", p.metadata}};
}

void from_json(const nlohmann::json &j, MetadataPreset &p) {
    p.name = j.value("name", "");
    p.metadata = j.value("metadata", FunscriptMetadata{});
}

void to_json(nlohmann::json &j, const AppSettings &s) {
    j = nlohmann::json::object({{"version", kAppSettingsVersion},
                                {"lastProjectPaths", s.lastProjectPaths},
                                {"reopenLastProject", s.reopenLastProject},
                                {"simulatorVisuals", s.simulator},
                                {"input", s.input},
                                {"holdRepeat", s.holdRepeat},
                                {"metadataPresets", s.metadataPresets},
                                {"volume", s.volume},
                                {"fontSizeBase", s.fontSizeBase},
                                {"showSimulator", s.showSimulator},
                                {"showStatistics", s.showStatistics},
                                {"showToolOptions", s.showToolOptions},
                                {"activeTheme", s.activeTheme},
                                {"hwdecEnabled", s.hwdecEnabled},
                                {"showTimelinePreview", s.showTimelinePreview},
                                {"pauseOnSeek", s.pauseOnSeek},
                                {"maxFps", s.maxFps},
                                {"autoBackupEnabled", s.autoBackupEnabled},
                                {"backupKeepCount", s.backupKeepCount},
                                {"checkForUpdatesOnStartup", s.checkForUpdatesOnStartup},
                                {"undoMemoryLimitMb", s.undoMemoryLimitMb},
                                {"language", s.language},
                                {"liveReloadTranslations", s.liveReloadTranslations},
                                {"intraOutputDir", s.intraOutputDir},
                                {"windowGeometry", s.windowGeometry}});
}

void from_json(const nlohmann::json &j, AppSettings &s) {
    s.lastProjectPaths = j.value("lastProjectPaths", std::vector<std::string>{});
    s.reopenLastProject = j.value("reopenLastProject", true);
    s.simulator = j.value("simulatorVisuals", SimulatorState{});
    s.input = j.value("input", InputSettings{});
    s.holdRepeat = j.value("holdRepeat", HoldRepeatSettings{});
    s.metadataPresets = j.value("metadataPresets", std::vector<MetadataPreset>{});
    s.volume = j.value("volume", 1.0f);
    s.fontSizeBase = j.value("fontSizeBase", 18.0f);
    s.showSimulator = j.value("showSimulator", true);
    s.showStatistics = j.value("showStatistics", true);
    s.showToolOptions = j.value("showToolOptions", true);
    s.activeTheme = j.value("activeTheme", std::string("Dark"));
    s.hwdecEnabled = j.value("hwdecEnabled", true);
    s.showTimelinePreview = j.value("showTimelinePreview", false);
    s.pauseOnSeek = j.value("pauseOnSeek", true);
    s.maxFps = j.value("maxFps", 0);
    s.autoBackupEnabled = j.value("autoBackupEnabled", true);
    s.backupKeepCount = std::max(1, j.value("backupKeepCount", 20));
    s.checkForUpdatesOnStartup = j.value("checkForUpdatesOnStartup", true);
    s.undoMemoryLimitMb = j.value("undoMemoryLimitMb", 256);
    s.language = j.value("language", std::string{});
    s.liveReloadTranslations = j.value("liveReloadTranslations", false);
    s.intraOutputDir = j.value("intraOutputDir", std::string{});
    s.windowGeometry = j.value("windowGeometry", WindowGeometry{});
}
} // namespace ofs
