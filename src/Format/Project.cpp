#include "Project.h"
#include "Core/StandardAxis.h"
#include "Services/EffectRegistry.h"
#include "UI/Theme.h" // AppCol_Bookmark default for chapter/bookmark colors lacking a stored value
#include "Util/FileUtil.h"
#include "Util/JsonImGui.h"
#include "Util/JsonUtil.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <iterator>
#include <map>
#include <miniz.h>
#include <ranges>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace ofs {

// ---- Actions binary encoding: double at_ms (LE IEEE 754) + uint8_t pos, then zlib compressed. The
// project file is CBOR, so the blob is stored as a native CBOR byte string — no base64 wrapper needed. ----

// Fixed-width little-endian (de)serialization for the blob's size prefix and packed at-values.
static void writeLE32(uint8_t *p, uint32_t v) {
    for (int b = 0; b < 4; ++b)
        p[b] = static_cast<uint8_t>((v >> (b * 8U)) & 0xFFU);
}
static void writeLE64(uint8_t *p, uint64_t v) {
    for (int b = 0; b < 8; ++b)
        p[b] = static_cast<uint8_t>((v >> (b * 8U)) & 0xFFU);
}
static uint32_t readLE32(const uint8_t *p) {
    uint32_t v = 0;
    for (int b = 0; b < 4; ++b)
        v |= static_cast<uint32_t>(p[b]) << (b * 8U);
    return v;
}
static uint64_t readLE64(const uint8_t *p) {
    uint64_t v = 0;
    for (int b = 0; b < 8; ++b)
        v |= static_cast<uint64_t>(p[b]) << (b * 8U);
    return v;
}

static std::vector<uint8_t> actionsToBlob(const VectorSet<ScriptAxisAction> &actions) {
    if (actions.empty())
        return {};

    const size_t rawSize = actions.size() * 9;
    std::vector<uint8_t> raw(rawSize);
    for (size_t i = 0; i < actions.size(); ++i) {
        const auto bits = std::bit_cast<uint64_t>(actions[i].at * 1000.0);
        uint8_t *p = raw.data() + i * 9;
        writeLE64(p, bits);
        p[8] = static_cast<uint8_t>(actions[i].pos);
    }

    mz_ulong compBound = mz_compressBound(static_cast<mz_ulong>(rawSize));
    // Blob layout: [4-byte LE uncompressed size][compressed payload]
    std::vector<uint8_t> blob(4 + compBound);
    writeLE32(blob.data(), static_cast<uint32_t>(rawSize));
    mz_ulong compSize = compBound;
    if (mz_compress2(blob.data() + 4, &compSize, raw.data(), static_cast<mz_ulong>(rawSize), MZ_DEFAULT_COMPRESSION) !=
        MZ_OK) {
        OFS_CORE_ERROR("actionsToBlob: compression failed");
        return {};
    }
    blob.resize(4 + compSize);
    return blob;
}

static VectorSet<ScriptAxisAction> actionsFromBlob(const std::vector<uint8_t> &blob) {
    if (blob.empty())
        return {};
    if (blob.size() < 4) {
        OFS_CORE_ERROR("actionsFromBlob: blob too short ({})", blob.size());
        return {};
    }

    const uint32_t rawSize = readLE32(blob.data());

    if (rawSize == 0)
        return {};
    if (rawSize % 9 != 0) {
        OFS_CORE_ERROR("actionsFromBlob: corrupt uncompressed size {}", rawSize);
        return {};
    }

    std::vector<uint8_t> raw(rawSize);
    mz_ulong destLen = rawSize;
    const int rc = mz_uncompress(raw.data(), &destLen, blob.data() + 4, static_cast<mz_ulong>(blob.size() - 4));
    if (rc != MZ_OK || destLen != rawSize) {
        OFS_CORE_ERROR("actionsFromBlob: decompression failed (code={}, destLen={}, expected={})", rc, destLen,
                       rawSize);
        return {};
    }

    auto view = std::views::iota(0ULL, rawSize / 9) | std::views::transform([&raw](size_t chunkIdx) {
                    const size_t i = chunkIdx * 9;
                    const uint64_t bits = readLE64(&raw[i]);
                    // Clamp at the load boundary: a foreign or corrupt file may carry a negative `at` or a
                    // pos byte above 100, neither of which is a valid action.
                    return clampedAction(std::bit_cast<double>(bits) / 1000.0, static_cast<int>(raw[i + 8]));
                });
    return {view.begin(), view.end()};
}

// ---- Bookmark / Chapter serialization ----

static nlohmann::json colorToJsonArray(ImU32 c) {
    return nlohmann::json::array({c & 0xFFU, (c >> 8U) & 0xFFU, (c >> 16U) & 0xFFU, (c >> 24U) & 0xFFU});
}

static ImU32 colorFromJsonArray(const nlohmann::json &j) {
    if (!j.is_array() || j.size() < 4)
        return ofs::theme::GetColorU32(AppCol_Bookmark);
    return IM_COL32(j[0].get<uint8_t>(), j[1].get<uint8_t>(), j[2].get<uint8_t>(), j[3].get<uint8_t>());
}

void to_json(nlohmann::json &j, const Bookmark &b) {
    j = {{"time", b.time}, {"name", b.name}};
}

void from_json(const nlohmann::json &j, Bookmark &b) {
    b.time = j.value("time", 0.0);
    b.name = j.value("name", "");
}

// VideoMode is needed by VideoFraming (below) and VideoPlayerState; declare its enum
// (de)serializer before either uses it.
NLOHMANN_JSON_SERIALIZE_ENUM(VideoMode, {
                                            {VideoMode::Full, "Full"},
                                            {VideoMode::VrMode, "VrMode"},
                                        })

// ---- Per-chapter scene memory (Chapter::sceneView, Project::defaultSceneView) ----
// Defined before Chapter's serializer so its `sceneView` field can use them via ADL.

void to_json(nlohmann::json &j, const VideoFraming &f) {
    j = {{"zoomFactor", f.zoomFactor},
         {"translation", f.translation},
         {"vrRotation", f.vrRotation},
         {"vrZoom", f.vrZoom}};
}

void from_json(const nlohmann::json &j, VideoFraming &f) {
    f.zoomFactor = j.value("zoomFactor", 1.0f);
    f.translation = j.value("translation", ImVec2{0.f, 0.f});
    f.vrRotation = j.value("vrRotation", ImVec2{0.5f, 0.5f});
    f.vrZoom = j.value("vrZoom", 0.5f);
}

void to_json(nlohmann::json &j, const OverlayAnchor &a) {
    j = {{"p1Norm", a.p1Norm},
         {"p2Norm", a.p2Norm},
         {"widthNorm", a.widthNorm},
         {"vrBarP1", a.vrBarP1},
         {"vrBarP2", a.vrBarP2},
         {"vrBarWidthAngle", a.vrBarWidthAngle},
         {"center3dNorm", a.center3dNorm},
         {"size3dNorm", a.size3dNorm},
         {"vrYaw", a.vrYaw},
         {"vrPitch", a.vrPitch},
         {"vrAngularSize", a.vrAngularSize}};
}

void from_json(const nlohmann::json &j, OverlayAnchor &a) {
    a.p1Norm = j.value("p1Norm", ImVec2{0.5f, 0.4f});
    a.p2Norm = j.value("p2Norm", ImVec2{0.5f, 0.6f});
    a.widthNorm = j.value("widthNorm", 0.12f);
    a.vrBarP1 = j.value("vrBarP1", ImVec2{kVrForwardYaw, 0.15f});
    a.vrBarP2 = j.value("vrBarP2", ImVec2{kVrForwardYaw, -0.15f});
    a.vrBarWidthAngle = j.value("vrBarWidthAngle", 0.06f);
    a.center3dNorm = j.value("center3dNorm", ImVec2{0.5f, 0.5f});
    a.size3dNorm = j.value("size3dNorm", 0.3f);
    a.vrYaw = j.value("vrYaw", kVrForwardYaw);
    a.vrPitch = j.value("vrPitch", 0.0f);
    a.vrAngularSize = j.value("vrAngularSize", 0.5f);
}

void to_json(nlohmann::json &j, const SceneView &s) {
    j = {{"framing", s.framing}, {"anchor", s.anchor}, {"inverted", s.inverted}};
}

void from_json(const nlohmann::json &j, SceneView &s) {
    s.framing = j.value("framing", VideoFraming{});
    s.anchor = j.value("anchor", OverlayAnchor{});
    s.inverted = j.value("inverted", false);
}

void to_json(nlohmann::json &j, const Chapter &c) {
    j = {{"startTime", c.startTime}, {"endTime", c.endTime}, {"name", c.name}, {"color", colorToJsonArray(c.color)}};
    if (c.sceneView)
        j["sceneView"] = *c.sceneView;
}

void from_json(const nlohmann::json &j, Chapter &c) {
    c.startTime = j.value("startTime", 0.0);
    c.endTime = j.value("endTime", 0.0);
    c.name = j.value("name", "");
    c.color = j.contains("color") ? colorFromJsonArray(j["color"]) : ofs::theme::GetColorU32(AppCol_Bookmark);
    if (j.contains("sceneView"))
        c.sceneView = j["sceneView"].get<SceneView>();
}

void to_json(nlohmann::json &j, const BookmarkChapterState &s) {
    j = {{"bookmarks", s.bookmarks}, {"chapters", s.chapters}};
}

void from_json(const nlohmann::json &j, BookmarkChapterState &s) {
    s.bookmarks = j.value("bookmarks", std::vector<Bookmark>{});
    s.chapters = j.value("chapters", std::vector<Chapter>{});
}

void to_json(nlohmann::json &j, const VideoPlayerState &s) {
    j = {{"activeMode", s.activeMode}, {"resolutionScale", s.resolutionScale}, {"locked", s.locked}};
}

void from_json(const nlohmann::json &j, VideoPlayerState &s) {
    s.activeMode = j.value("activeMode", VideoMode::Full);
    s.resolutionScale = std::clamp(j.value("resolutionScale", 1.0f), 0.01f, 1.0f);
    s.locked = j.value("locked", false);
}

NLOHMANN_JSON_SERIALIZE_ENUM(ScriptingOverlay, {
                                                   {ScriptingOverlay::Frame, "Frame"},
                                                   {ScriptingOverlay::Tempo, "Tempo"},
                                               })

NLOHMANN_JSON_SERIALIZE_ENUM(TimelineLayout, {
                                                 {TimelineLayout::Overlay, "Overlay"},
                                                 {TimelineLayout::Lanes, "Lanes"},
                                             })

void to_json(nlohmann::json &j, const OverlayState &s) {
    j = nlohmann::json{{"overlay", s.overlay},
                       {"frameFps", s.frameFps},
                       {"tempoBpm", s.tempoBpm},
                       {"tempoOffsetSeconds", s.tempoOffsetSeconds},
                       {"tempoMeasureIndex", s.tempoMeasureIndex}};
}

void from_json(const nlohmann::json &j, OverlayState &s) {
    s.overlay = j.value("overlay", ScriptingOverlay::Frame);
    s.frameFps = j.value("frameFps", 30.0f);
    s.tempoBpm = j.value("tempoBpm", 120.0f);
    s.tempoOffsetSeconds = j.value("tempoOffsetSeconds", 0.0f);
    s.tempoMeasureIndex = j.value("tempoMeasureIndex", 2);
}

void to_json(nlohmann::json &j, const ProcessingEffect &e) {
    j = {{"type", e.type}, {"params", e.params}};
}

void from_json(const nlohmann::json &j, ProcessingEffect &e) {
    e.type = j.value("type", "");
    // The project form stores params as an array; the graph-preset form stores them as a
    // name-keyed object and fills the positional vector itself (registry-aware). jsonValueOr
    // leaves params empty for an object/absent value, reading only an actual array.
    e.params = ofs::util::jsonValueOr(j, "params", std::vector<float>{});
}

// Node type as a stable string tag, not an enum ordinal — so reordering GraphNodeType can
// never silently re-map existing files. Unknown tags fall back to the first entry (Input).
NLOHMANN_JSON_SERIALIZE_ENUM(GraphNodeType, {
                                                {GraphNodeType::Input, "Input"},
                                                {GraphNodeType::Output, "Output"},
                                                {GraphNodeType::Effect, "Effect"},
                                                {GraphNodeType::Add, "Add"},
                                                {GraphNodeType::Subtract, "Subtract"},
                                                {GraphNodeType::Multiply, "Multiply"},
                                                {GraphNodeType::Divide, "Divide"},
                                                {GraphNodeType::Constant, "Constant"},
                                                {GraphNodeType::PluginNode, "PluginNode"},
                                                {GraphNodeType::Script, "Script"},
                                                {GraphNodeType::Discretize, "Discretize"},
                                                {GraphNodeType::Functionalize, "Functionalize"},
                                            })

void to_json(nlohmann::json &j, const ProcessingGraphNode &n) {
    j = {{"id", n.id},
         {"type", n.type},
         {"effect", n.effect},
         {"constantValue", n.constantValue},
         {"posX", n.posX},
         {"posY", n.posY},
         {"role", std::string(standardAxisTag(n.role))},
         {"pluginInputCount", n.pluginInputCount},
         {"pluginOutputCount", n.pluginOutputCount},
         {"pluginSignal", n.pluginSignal},
         {"scriptFile", n.scriptFile},
         {"scriptSignal", n.scriptSignal},
         {"scriptInputCount", n.scriptInputCount},
         {"scriptOutputCount", n.scriptOutputCount},
         {"scriptEmbeddedSource", n.scriptEmbeddedSource},
         {"pluginNodeId", n.pluginNodeId}};
    // Embed structured state (object/array) as a nested JSON value so the project file stays readable and
    // diffable; a missing plugin's state round-trips losslessly. C++ never interprets it. Anything else — a
    // bare scalar (a string/number/bool TState) or a broken hand-edit that won't parse — is preserved
    // verbatim as a JSON string. from_json reads any string back as raw text, so a scalar must NOT be stored
    // as a parsed JSON string: a `"guid"` TState would otherwise come back unquoted and corrupt.
    if (!n.nodeState.empty()) {
        auto parsed = nlohmann::json::parse(n.nodeState, nullptr, /*allow_exceptions=*/false);
        j["nodeState"] = (parsed.is_object() || parsed.is_array()) ? parsed : nlohmann::json(n.nodeState);
    }
}

void from_json(const nlohmann::json &j, ProcessingGraphNode &n) {
    n.id = j.value("id", 0);
    n.type = j.value("type", GraphNodeType::Input);
    n.effect = j.value("effect", ProcessingEffect{});
    n.constantValue = j.value("constantValue", 50.0f);
    n.posX = j.value("posX", 0.0f);
    n.posY = j.value("posY", 0.0f);
    n.role = standardAxisFromTag(j.value("role", "L0")).value_or(StandardAxis::L0);
    n.pluginInputCount = j.value("pluginInputCount", uint8_t{1});
    n.pluginOutputCount = j.value("pluginOutputCount", uint8_t{1});
    n.pluginSignal = j.value("pluginSignal", uint8_t{0});
    n.scriptFile = j.value("scriptFile", std::string{});
    n.scriptSignal = j.value("scriptSignal", uint8_t{1});
    n.scriptInputCount = j.value("scriptInputCount", uint8_t{1});
    n.scriptOutputCount = j.value("scriptOutputCount", uint8_t{1});
    n.scriptEmbeddedSource = j.value("scriptEmbeddedSource", std::string{});
    n.pluginNodeId = j.value("pluginNodeId", std::string{});
    // Carry nodeState back as raw JSON text. A nested object dumps to its canonical text; a string is the
    // broken-hand-edit fallback from to_json — take it verbatim so it survives untouched.
    if (j.contains("nodeState"))
        n.nodeState = j["nodeState"].is_string() ? j["nodeState"].get<std::string>() : j["nodeState"].dump();
    else
        n.nodeState = std::string{};
}

void to_json(nlohmann::json &j, const ProcessingGraphLink &l) {
    j = {{"id", l.id}, {"fromNode", l.fromNode}, {"fromPin", l.fromPin}, {"toNode", l.toNode}, {"toPin", l.toPin}};
}

void from_json(const nlohmann::json &j, ProcessingGraphLink &l) {
    l.id = j.value("id", 0);
    l.fromNode = j.value("fromNode", 0);
    l.fromPin = j.value("fromPin", 0); // default 0 = primary output (single-output graphs)
    l.toNode = j.value("toNode", 0);
    l.toPin = j.value("toPin", 0);
}

void to_json(nlohmann::json &j, const ProcessingNodeGraph &g) {
    j = {{"nodes", g.nodes}, {"links", g.links}, {"nextId", g.nextId}};
}

void from_json(const nlohmann::json &j, ProcessingNodeGraph &g) {
    g.nodes = j.value("nodes", std::vector<ProcessingGraphNode>{});
    g.links = j.value("links", std::vector<ProcessingGraphLink>{});
    g.nextId = j.value("nextId", 1);
}

void to_json(nlohmann::json &j, const ProcessingRegion &r) {
    j = {{"id", r.id},
         {"startTime", r.startTime},
         {"endTime", r.endTime},
         {"name", r.name},
         {"color", colorToJsonArray(r.color)},
         {"nodeGraph", r.nodeGraph},
         {"hz", r.hz},
         {"showSourceActions", r.showSourceActions},
         {"axisRoles", r.axisRoleTags}};
}

void from_json(const nlohmann::json &j, ProcessingRegion &r) {
    r.id = j.value("id", 0);
    r.startTime = j.value("startTime", 0.0);
    r.endTime = j.value("endTime", 0.0);
    r.name = j.value("name", std::string{});
    if (j.contains("color"))
        r.color = colorFromJsonArray(j["color"]);
    r.hz = j.value("hz", kDefaultRegionHz);
    r.showSourceActions = j.value("showSourceActions", true);
    r.nodeGraph = j.contains("nodeGraph") ? j["nodeGraph"].get<ProcessingNodeGraph>() : buildDefaultGraph();
    r.axisRoleTags = j.value("axisRoles", std::vector<std::string>{});
}

void to_json(nlohmann::json &j, const SerializedAxis &a) {
    j = nlohmann::json::object({
        {"role", std::string(standardAxisTag(a.role))},
        {"visible", a.isVisible},
        {"inPanel", a.showInStrip},
        {"locked", a.isLocked},
        {"actions", nlohmann::json::binary(actionsToBlob(a.actions))},
    });
}

void from_json(const nlohmann::json &j, SerializedAxis &a) {
    auto roleOpt = standardAxisFromTag(j.value("role", ""));
    a.role = roleOpt.value_or(StandardAxis::L0);
    a.isVisible = j.value("visible", true);
    a.showInStrip = j.value("inPanel", true);
    a.isLocked = j.value("locked", false);
    const auto it = j.find("actions");
    a.actions = (it != j.end() && it->is_binary()) ? actionsFromBlob(it->get_binary()) : VectorSet<ScriptAxisAction>{};
}

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

void to_json(nlohmann::json &j, const Project &p) {
    j = nlohmann::json::object({{"ofsProjectVersion", kProjectFileVersion},
                                {"mediaPath", p.mediaPath},
                                {"originalMediaPath", p.originalMediaPath},
                                {"dummyDuration", p.dummyDuration},
                                {"activeAxisRole", std::string(standardAxisTag(p.activeAxisRole))},
                                {"scriptAxes", p.scriptAxes},
                                {"processingRegions", p.processingRegions},
                                {"metadata", p.metadata},
                                {"overlaySettings", p.overlaySettings},
                                {"videoPlayerState", p.videoPlayerState},
                                {"simP1", p.simP1},
                                {"simP2", p.simP2},
                                {"sim3dPos", p.sim3dPos},
                                {"sim3dSize", p.sim3dSize},
                                {"defaultSceneView", p.defaultSceneView},
                                {"bookmarkChapters", p.bookmarkChapters},
                                {"totalEditingSeconds", p.totalEditingSeconds},
                                {"createdAtUnix", p.createdAtUnix},
                                {"modifiedAtUnix", p.modifiedAtUnix},
                                {"editSessionCount", p.editSessionCount},
                                {"playbackPosition", p.playbackPosition},
                                {"autoNameSeed", p.autoNameSeed},
                                {"activeNavigator", p.activeNavigator},
                                {"activeEditMode", p.activeEditMode},
                                {"activeSelectionMode", p.activeSelectionMode},
                                {"showAudioWaveform", p.showAudioWaveform},
                                {"timelineLayout", p.timelineLayout}});
    if (p.lastExport)
        j["lastExport"] = *p.lastExport;
    // Only persist plugin data when something is stored, so an untouched project doesn't carry a "{}".
    if (p.pluginData.is_object() && !p.pluginData.empty())
        j["pluginData"] = p.pluginData;
}

void from_json(const nlohmann::json &j, Project &p) {
    p.mediaPath = j.value("mediaPath", "");
    p.originalMediaPath = j.value("originalMediaPath", "");
    p.dummyDuration = j.value("dummyDuration", 0.0);
    p.activeAxisRole = standardAxisFromTag(j.value("activeAxisRole", "L0")).value_or(StandardAxis::L0);
    p.scriptAxes = j.value("scriptAxes", std::vector<SerializedAxis>{});
    p.processingRegions = j.value("processingRegions", std::vector<ProcessingRegion>{});
    p.metadata = j.value("metadata", FunscriptMetadata{});
    p.overlaySettings = j.value("overlaySettings", OverlayState{});
    p.videoPlayerState = j.value("videoPlayerState", VideoPlayerState{});
    p.simP1 = j.value("simP1", ImVec2{600.f, 300.f});
    p.simP2 = j.value("simP2", ImVec2{600.f, 700.f});
    p.sim3dPos = j.value("sim3dPos", ImVec2{-1.f, -1.f});
    p.sim3dSize = j.value("sim3dSize", 300.f);
    p.defaultSceneView = j.value("defaultSceneView", SceneView{});
    p.bookmarkChapters = j.value("bookmarkChapters", BookmarkChapterState{});
    p.totalEditingSeconds = j.value("totalEditingSeconds", 0.0);
    p.createdAtUnix = j.value("createdAtUnix", static_cast<int64_t>(0));
    p.modifiedAtUnix = j.value("modifiedAtUnix", static_cast<int64_t>(0));
    p.editSessionCount = j.value("editSessionCount", 0);
    p.playbackPosition = j.value("playbackPosition", 0.0);
    p.autoNameSeed = j.value("autoNameSeed", 0);
    // A file without these keys defaults to the native resolver for each interaction seam.
    p.activeNavigator = j.value("activeNavigator", std::string("follow-overlay"));
    p.activeEditMode = j.value("activeEditMode", std::string("native"));
    p.activeSelectionMode = j.value("activeSelectionMode", std::string("native"));
    p.showAudioWaveform = j.value("showAudioWaveform", false);
    // COMPAT(2026-06-30): timeline layout absent in pre-lanes projects; default Overlay (the prior look).
    p.timelineLayout = j.value("timelineLayout", TimelineLayout::Overlay);
    if (j.contains("lastExport"))
        p.lastExport = j["lastExport"].get<ExportConfig>();
    // Absent or a non-object (corrupt) → empty object, never null.
    p.pluginData = nlohmann::json::object();
    if (auto it = j.find("pluginData"); it != j.end() && it->is_object())
        p.pluginData = *it;
}

std::optional<Project> Project::load(const std::filesystem::path &path) {
    // Project files are CBOR (binary). Read raw bytes and decode.
    auto bytes = ofs::util::readFile(path);
    if (!bytes) {
        OFS_CORE_ERROR("Failed to open project file: {}", ofs::util::toUtf8(path));
        return std::nullopt;
    }
    try {
        const nlohmann::json j = nlohmann::json::from_cbor(*bytes);
        // Refuse files written by a newer, incompatible version rather than silently dropping
        // fields the current schema doesn't understand.
        const int version = j.value("ofsProjectVersion", kProjectFileVersion);
        if (version > kProjectFileVersion) {
            OFS_CORE_ERROR("Project file '{}' version {} is newer than supported version {}.", ofs::util::toUtf8(path),
                           version, kProjectFileVersion);
            return std::nullopt;
        }
        return j.get<Project>();
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to read project file: {}. Error: {}", ofs::util::toUtf8(path), e.what());
        return std::nullopt;
    }
}

bool Project::save(const std::filesystem::path &path) const {
    try {
        const std::vector<uint8_t> cbor = nlohmann::json::to_cbor(nlohmann::json(*this));
        if (!ofs::util::writeFileAtomic(path, cbor.data(), cbor.size())) {
            OFS_CORE_ERROR("Failed to open project file for writing: {}", ofs::util::toUtf8(path));
            return false;
        }
        return true;
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to serialize project: {}", e.what());
        return false;
    }
}

// ── Graph preset helpers ─────────────────────────────────────────────────────

static bool isMathNodeType(GraphNodeType t) {
    return t == GraphNodeType::Add || t == GraphNodeType::Subtract || t == GraphNodeType::Multiply ||
           t == GraphNodeType::Divide;
}

// The registry param definitions for a node, or nullptr if it isn't a native Effect or its definition is
// missing from the registry. Plugin nodes carry no scalar params (their state is the TState JSON in
// nodeState), so only native Effect nodes resolve here.
static const std::vector<EffectParamDef> *effectParamDefs(const ProcessingGraphNode &n,
                                                          const EffectRegistryState &reg) {
    if (n.type != GraphNodeType::Effect)
        return nullptr;
    auto it = reg.effects.find(n.effect.type);
    return it == reg.effects.end() ? nullptr : &it->second.paramDefs;
}

// Param names (in definition order) for an Effect/PluginNode, or nullptr if its definition
// isn't in the registry. Fills `scratch` and returns a pointer to it.
static const std::vector<std::string> *paramNames(const ProcessingGraphNode &n, const EffectRegistryState &reg,
                                                  std::vector<std::string> &scratch) {
    scratch.clear();
    const auto *defs = effectParamDefs(n, reg);
    if (!defs)
        return nullptr;
    for (const auto &pd : *defs)
        scratch.push_back(pd.key);
    return &scratch;
}

// Serialize a node's params as a name->value object. If the definition is unavailable
// (missing dep at save time), fall back to a positional array so values aren't lost.
static nlohmann::json nodeParamsToJson(const ProcessingGraphNode &n, const EffectRegistryState &reg) {
    std::vector<std::string> names;
    const auto *keys = paramNames(n, reg, names);
    if (!keys)
        return n.effect.params; // array fallback
    nlohmann::json obj = nlohmann::json::object();
    for (size_t i = 0; i < keys->size() && i < n.effect.params.size(); ++i)
        obj[(*keys)[i]] = n.effect.params[i];
    return obj;
}

// Rebuild a node's positional params from saved json, reconciled against the current
// definition: one slot per current param, value = saved-by-name else default. Unknown saved
// keys are dropped; new params get their default. If the definition is missing, preserve the
// saved values best-effort (the node will be inactive anyway).
static std::vector<float> reconcileNodeParams(const ProcessingGraphNode &n, const nlohmann::json &paramsJson,
                                              const EffectRegistryState &reg) {
    struct ParamDef {
        std::string key;
        float def;
    };
    std::vector<ParamDef> defs;
    if (const auto *pdefs = effectParamDefs(n, reg))
        for (const auto &pd : *pdefs)
            defs.push_back({.key = pd.key, .def = pd.defaultValue});

    if (defs.empty()) { // missing dependency — preserve whatever was saved
        std::vector<float> vals;
        if (paramsJson.is_array()) {
            for (const auto &el : paramsJson)
                vals.push_back(el.is_number() ? el.get<float>() : 0.0f);
        } else if (paramsJson.is_object()) {
            for (const auto &el : paramsJson.items())
                if (el.value().is_number())
                    vals.push_back(el.value().get<float>());
        }
        return vals;
    }

    std::vector<float> out;
    out.reserve(defs.size());
    for (size_t i = 0; i < defs.size(); ++i) {
        const auto &d = defs[i];
        if (paramsJson.is_object() && paramsJson.contains(d.key) && paramsJson[d.key].is_number())
            out.push_back(paramsJson[d.key].get<float>());
        else if (paramsJson.is_array() && i < paramsJson.size() && paramsJson[i].is_number())
            out.push_back(paramsJson[i].get<float>()); // positional: preset saved without the effect def
        else
            out.push_back(d.def);
    }
    return out;
}

static bool graphHasCycle(const ProcessingNodeGraph &g) {
    std::unordered_map<int, std::vector<int>> adj;
    for (const auto &l : g.links)
        adj[l.fromNode].push_back(l.toNode);
    std::unordered_set<int> visited;
    std::unordered_set<int> inStack;
    std::function<bool(int)> dfs = [&](int node) -> bool {
        if (inStack.contains(node))
            return true;
        if (visited.contains(node))
            return false;
        visited.insert(node);
        inStack.insert(node);
        if (auto it = adj.find(node); it != adj.end())
            for (int nxt : it->second)
                if (dfs(nxt))
                    return true;
        inStack.erase(node);
        return false;
    };
    return std::ranges::any_of(g.nodes, [&](const auto &n) { return !visited.contains(n.id) && dfs(n.id); });
}

// Strict structural validation. Returns false (with a human-readable reason in `error`) on any
// invariant violation. App-saved graphs always pass; only foreign/corrupt files fail.
static bool validateGraph(const ProcessingNodeGraph &g, std::string &error) {
    constexpr size_t kMaxNodes = 4096;
    if (g.nodes.size() > kMaxNodes) {
        error = fmt::format("too many nodes ({})", g.nodes.size());
        return false;
    }

    std::unordered_set<int> ids; // node and link ids share one monotonic space (allocId)
    int maxId = 0;
    for (const auto &n : g.nodes) {
        if (n.id <= 0) {
            error = fmt::format("node id {} must be positive", n.id);
            return false;
        }
        if (!ids.insert(n.id).second) {
            error = fmt::format("duplicate id {}", n.id);
            return false;
        }
        maxId = std::max(maxId, n.id);
        if (!std::isfinite(n.posX) || !std::isfinite(n.posY)) {
            error = fmt::format("node {} has a non-finite position", n.id);
            return false;
        }
        if (n.type == GraphNodeType::Input || n.type == GraphNodeType::Output) {
            const int r = static_cast<int>(n.role);
            if (r < 0 || r >= static_cast<int>(kStandardAxisCount)) {
                error = fmt::format("node {} has an out-of-range axis role", n.id);
                return false;
            }
        }
    }

    std::set<std::pair<int, int>> inPins;
    for (const auto &l : g.links) {
        if (l.id <= 0) {
            error = fmt::format("link id {} must be positive", l.id);
            return false;
        }
        if (!ids.insert(l.id).second) {
            error = fmt::format("id {} is reused by a link", l.id);
            return false;
        }
        maxId = std::max(maxId, l.id);
        const auto *to = g.findNode(l.toNode);
        const auto *from = g.findNode(l.fromNode);
        if (!from || !to) {
            error = fmt::format("link {} references a missing node", l.id);
            return false;
        }
        if (l.fromNode == l.toNode) {
            error = fmt::format("link {} is a self-loop", l.id);
            return false;
        }
        // Output pin count drives the valid fromPin range (canonical arity — see ProcessingRegion.h).
        const int outputCount = nodeOutputPinCount(*from);
        if (l.fromPin < 0 || l.fromPin >= outputCount) {
            error = fmt::format("link {} has an invalid output pin {}", l.id, l.fromPin);
            return false;
        }
        // Input pin count drives the valid toPin range. A node with no inputs accepts no link.
        const int inputCount = nodeInputPinCount(*to);
        if (inputCount == 0) {
            error = fmt::format("link {} targets a node with no inputs", l.id);
            return false;
        }
        if (l.toPin < 0 || l.toPin >= inputCount) {
            error = fmt::format("link {} has an invalid input pin {}", l.id, l.toPin);
            return false;
        }
        if (!inPins.insert({l.toNode, l.toPin}).second) {
            error = fmt::format("node {} input pin {} has more than one link", l.toNode, l.toPin);
            return false;
        }
    }

    if (g.nextId <= maxId) {
        error = fmt::format("nextId {} must exceed the largest id {}", g.nextId, maxId);
        return false;
    }
    if (graphHasCycle(g)) {
        error = "the graph contains a cycle";
        return false;
    }
    return true;
}

static std::vector<std::string> collectMissingDeps(const ProcessingNodeGraph &g, const EffectRegistryState &reg) {
    std::set<std::string> missing;
    for (const auto &n : g.nodes) {
        if (n.type == GraphNodeType::Effect && !reg.effects.contains(n.effect.type))
            missing.insert(fmt::format("effect: {}", n.effect.type));
        else if (n.type == GraphNodeType::PluginNode && !reg.pluginNodes.contains(n.pluginNodeId))
            missing.insert(fmt::format("plugin: {}", n.pluginNodeId));
    }
    return {missing.begin(), missing.end()};
}

bool GraphPreset::save(const std::filesystem::path &path, const EffectRegistryState &reg) const {
    try {
        nlohmann::json graphJson = graph; // generic form: params as arrays
        std::set<std::string> effects;
        std::set<std::string> plugins;
        for (size_t i = 0; i < graph.nodes.size(); ++i) {
            const auto &n = graph.nodes[i];
            if (n.type == GraphNodeType::Effect)
                effects.insert(n.effect.type);
            else if (n.type == GraphNodeType::PluginNode)
                plugins.insert(n.pluginNodeId);
            else
                continue;
            graphJson["nodes"][i]["effect"]["params"] = nodeParamsToJson(n, reg); // override → name-keyed
        }

        nlohmann::json j = nlohmann::json::object({
            {"ofsGraphVersion", kGraphPresetVersion},
            {"app", "ofs-ng"},
            {"meta", nlohmann::json::object({
                         {"name", name},
                         {"requires", nlohmann::json::object({
                                          {"effects", std::vector<std::string>(effects.begin(), effects.end())},
                                          {"plugins", std::vector<std::string>(plugins.begin(), plugins.end())},
                                      })},
                     })},
            {"axisRoles", axisRoleTags},
            {"hz", hz},
            {"nodeGraph", graphJson},
        });
        // Script sources travel on their nodes (scriptEmbeddedSource), serialized inside nodeGraph —
        // no separate top-level map. The caller inlines file-backed nodes' sources before saving.
        if (!ofs::util::writeFileAtomic(path, j.dump(4))) {
            OFS_CORE_ERROR("Failed to open graph file for writing: {}", ofs::util::toUtf8(path));
            return false;
        }
        return true;
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Failed to serialize graph: {}", e.what());
        return false;
    }
}

GraphLoadResult loadGraphPreset(const std::filesystem::path &path, const EffectRegistryState &reg) {
    GraphLoadResult result;

    auto text = ofs::util::readFile(path);
    if (!text) {
        result.error = "Could not open the file.";
        return result;
    }
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(*text);
    } catch (const std::exception &e) {
        result.error = fmt::format("Invalid JSON: {}", e.what());
        return result;
    }

    using ofs::util::jsonObjectIf;
    const int version = j.value("ofsGraphVersion", kGraphPresetVersion);
    if (version > kGraphPresetVersion) {
        result.error = fmt::format("File version {} is newer than supported version {}.", version, kGraphPresetVersion);
        return result;
    }
    if (!j.contains("nodeGraph")) {
        result.error = "Missing 'nodeGraph'.";
        return result;
    }

    GraphPreset preset;
    try {
        preset.graph = j["nodeGraph"].get<ProcessingNodeGraph>();
    } catch (const std::exception &e) {
        result.error = fmt::format("Malformed nodeGraph: {}", e.what());
        return result;
    }
    preset.axisRoleTags = j.value("axisRoles", std::vector<std::string>{});
    preset.hz = j.value("hz", kDefaultRegionHz);
    if (const auto *meta = jsonObjectIf(j, "meta"))
        preset.name = meta->value("name", std::string{});

    // Reconcile params by name against the registry. from_json left object-form params empty,
    // so read the raw per-node json (node order matches, since from_json parses in order).
    const auto &nodesJson = j["nodeGraph"].value("nodes", nlohmann::json::array());
    if (nodesJson.is_array() && nodesJson.size() == preset.graph.nodes.size()) {
        for (size_t i = 0; i < preset.graph.nodes.size(); ++i) {
            auto &node = preset.graph.nodes[i];
            if (node.type != GraphNodeType::Effect && node.type != GraphNodeType::PluginNode)
                continue;
            nlohmann::json paramsJson = nlohmann::json::array();
            const auto &nj = nodesJson[i];
            if (nj.contains("effect") && nj["effect"].contains("params"))
                paramsJson = nj["effect"]["params"];
            node.effect.params = reconcileNodeParams(node, paramsJson, reg);
        }
    }

    std::string err;
    if (!validateGraph(preset.graph, err)) {
        result.error = fmt::format("Invalid graph structure: {}.", err);
        return result;
    }

    result.missingDeps = collectMissingDeps(preset.graph, reg);
    result.preset = std::move(preset);
    return result;
}

bool sanitizeProjectGraph(ProcessingNodeGraph &g, const EffectRegistryState &reg, std::string &error) {
    for (auto &node : g.nodes) {
        // Collect the declared param defaults for this node's native effect. A missing dependency
        // leaves an empty list → we skip the node (it is inactive in eval, so params are never read).
        // Plugin nodes have no scalar params (state lives in nodeState), so only Effect nodes apply.
        std::vector<float> defaults;
        if (const auto *pdefs = effectParamDefs(node, reg))
            for (const auto &pd : *pdefs)
                defaults.push_back(pd.defaultValue);
        if (defaults.empty())
            continue;

        // Bring positional params to the declared arity: pad short/empty arrays with defaults (the
        // crash a fuzzer found: an empty list makes params.data() null, which kernels deref), drop
        // any extras so the array matches what the kernel indexes.
        auto &p = node.effect.params;
        for (size_t i = p.size(); i < defaults.size(); ++i)
            p.push_back(defaults[i]);
        if (p.size() > defaults.size())
            p.resize(defaults.size());
    }
    return validateGraph(g, error);
}

} // namespace ofs
