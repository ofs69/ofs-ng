#pragma once

#include "Core/BookmarkChapterState.h"
#include "Core/ExportConfig.h"
#include "Core/FunscriptMetadata.h"
#include "Core/OverlaySettings.h"
#include "Core/ProcessingRegion.h"
#include "Core/ScriptAxisAction.h"
#include "Core/StandardAxis.h"
#include "Core/VectorSet.h"
#include "Video/VideoPlayerSettings.h"
#include "imgui.h"
#include <cstdint>
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace ofs {

struct EffectRegistryState;

// Current on-disk format version for project (.ofp) files. Bump when the schema changes
// incompatibly; Project::load refuses files newer than this rather than silently dropping
// fields.
inline constexpr int kProjectFileVersion = 1;

struct SerializedAxis {
    StandardAxis role = StandardAxis::L0;
    bool isVisible = true;
    bool showInStrip = true;
    bool isLocked = false;
    VectorSet<ScriptAxisAction> actions;
};

struct Project {
    std::string mediaPath; // resolved active media path (== original or intra); handed to the player
    // originalMediaPath is the video the user opened (UTF-8). The intra-optimized copy is NOT persisted:
    // it lives in the shared output dir under a deterministic <fingerprint>.mp4 name (intraOutputPath()),
    // so its existence is rediscovered on load rather than stored.
    std::string originalMediaPath;
    bool intraOptimizeDeclined = false; // sticky "Not Now" for the optimize prompt, scoped to originalMediaPath
    double dummyDuration = 0.0;
    StandardAxis activeAxisRole = StandardAxis::L0;
    std::vector<SerializedAxis> scriptAxes;
    std::vector<ProcessingRegion> processingRegions;

    FunscriptMetadata metadata;
    OverlayState overlaySettings;
    VideoPlayerState videoPlayerState;
    ImVec2 simP1 = {600.f, 300.f};
    ImVec2 simP2 = {600.f, 700.f};
    ImVec2 sim3dPos = {-1.f, -1.f}; // negative sentinel → centered on first render
    float sim3dSize = 300.f;
    SceneView defaultSceneView; // scene memory fallback for time outside any chapter
    BookmarkChapterState bookmarkChapters;
    double totalEditingSeconds = 0.0;
    int64_t createdAtUnix = 0;     // Unix seconds; first save. 0 = never saved.
    int64_t modifiedAtUnix = 0;    // Unix seconds; most recent user save. 0 = never saved.
    int editSessionCount = 0;      // number of editing sessions (project opens) accrued so far
    double playbackPosition = 0.0; // resume point — the playback cursor time (seconds) when the project was saved
    int autoNameSeed = 0;          // per-project offset for region-name / chapter-color sequences
    // Active navigator id (the prev/next-step behavior). Round-trips with the project; mirrors
    // ScriptProject::activeNavigator. Default matches the native follow-overlay navigator.
    std::string activeNavigator = "follow-overlay";
    // Active edit mode id (how timeline edit gestures resolve). Round-trips with the project; mirrors
    // ScriptProject::activeEditMode. Default matches the native edit mode.
    std::string activeEditMode = "native";
    // Active selection mode id (what a selection gesture selects). Round-trips with the project; mirrors
    // ScriptProject::activeSelectionMode. Default matches the native selection mode.
    std::string activeSelectionMode = "native";
    std::optional<ExportConfig> lastExport;
    // Whether the timeline audio waveform is enabled for this project. Opt-in and persisted; mirrors
    // ScriptProject::timelineView.showAudioWaveform. Default off.
    bool showAudioWaveform = false;
    // Per-plugin custom data: a JSON object shaped pluginName → { key → value }. Opaque to the host;
    // round-trips losslessly. Mirrors ScriptProject::pluginData. Object (never null) so it writes as {}.
    nlohmann::json pluginData = nlohmann::json::object();

    static std::optional<Project> load(const std::filesystem::path &path);
    bool save(const std::filesystem::path &path) const;
};

void to_json(nlohmann::json &j, const Project &p);
void from_json(const nlohmann::json &j, Project &p);

// Current on-disk format version for graph preset files. Bump when the schema
// changes incompatibly; loadGraphPreset refuses files newer than this.
inline constexpr int kGraphPresetVersion = 1;

// A shareable single-graph preset, saved separately from a full project. Holds
// the node graph plus the axis tags it targets (no time range, or id) and a small
// metadata header. Params are persisted by name (registry-aware) so they survive
// effect/plugin definition changes.
struct GraphPreset {
    ProcessingNodeGraph graph;
    std::vector<std::string> axisRoleTags;
    int hz = 30;      // region discretization rate (1–120)
    std::string name; // meta.name — region name at save time
    // A shared preset is self-contained: every Script node carries its own source in
    // ProcessingGraphNode::scriptEmbeddedSource (file nodes get their .cs inlined at save time), so
    // the recipient needs none of the author's .cs files. Loading a preset that carries script code
    // is gated behind a trust warning (see ProjectManager).

    // Serialize with registry-aware, name-keyed params. Returns false on I/O error.
    bool save(const std::filesystem::path &path, const EffectRegistryState &reg) const;
};

// Outcome of loading a graph preset. `preset` is nullopt on parse/version/validation
// failure (with `error` set). `missingDeps` lists effect/plugin ids the graph references
// that aren't installed — load still succeeds; those nodes are inactive.
struct GraphLoadResult {
    std::optional<GraphPreset> preset;
    std::string error;
    std::vector<std::string> missingDeps;
};

GraphLoadResult loadGraphPreset(const std::filesystem::path &path, const EffectRegistryState &reg);

// Normalize a node graph deserialized from a project file so the rest of the app (the processing
// system in particular) can trust it. Brings each Effect/PluginNode node's positional params to the
// registry-declared arity — missing slots filled with each param's default, extras dropped — so a
// kernel that indexes params[i] never sees a short or empty array. Then runs the same structural
// checks as loadGraphPreset. Returns false (with a reason in `error`) if the structure is invalid;
// the caller should replace the graph with a default. Nodes whose effect/plugin is not installed are
// left untouched (they are inactive in eval, so their params are never read).
bool sanitizeProjectGraph(ProcessingNodeGraph &g, const EffectRegistryState &reg, std::string &error);

} // namespace ofs
