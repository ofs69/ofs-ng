#pragma once
#include "Core/FunscriptMetadata.h"
#include "Core/SimulatorSettings.h"
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace ofs {

// On-disk schema version for settings.json. Bump on an incompatible change; AppSettings::load
// refuses a file newer than this rather than silently misreading fields.
inline constexpr int kAppSettingsVersion = 1;

struct MetadataPreset {
    std::string name;
    FunscriptMetadata metadata;
};

void to_json(nlohmann::json &j, const MetadataPreset &p);
void from_json(const nlohmann::json &j, MetadataPreset &p);

// Gamepad / analog input tunables. deadzone + smoothing
// compensate noisy stick/trigger signals for analog hold sources. Surfaced in the Shortcut window.
struct InputSettings {
    float deadzone = 0.15f; // 0..1 idle threshold for analog hold sources
    float smoothing = 0.5f; // low-pass time constant (s); larger = smoother, laggier
};

void to_json(nlohmann::json &j, const InputSettings &s);
void from_json(const nlohmann::json &j, InputSettings &s);

// Global cadence for "fire while held" key/button bindings (frame-step, move, volume, …). Mirrors the
// hardcoded constants that used to live in OfsApp; surfaced in the Shortcut window so users can tune the
// repeat feel. Converted to ofs::HoldRepeatParams at the dispatch site (kept POD here so AppSettings
// stays free of BindingSystem.h, like InputSettings ↔ AnalogConfig). `accel < 1` makes repeats speed up
// the longer a key is held (the gap shrinks toward holdRepeats' floor); `accel == 1` is a steady cadence.
struct HoldRepeatSettings {
    float initialDelay = 0.30f; // seconds before the second fire
    float interval = 0.06f;     // seconds between repeats after the delay
    float accel = 0.88f;        // <1 shrinks the interval each repeat (acceleration); 1 = steady
    float maxRateHz = 60.0f;    // ceiling an accelerating hold ramps to (repeats/sec); capped to the hard limit
};

void to_json(nlohmann::json &j, const HoldRepeatSettings &s);
void from_json(const nlohmann::json &j, HoldRepeatSettings &s);

// Persisted main-window geometry. Position is SDL global screen coords, size is window (logical) units —
// the same DPI-independent space SDL_CreateWindow uses. `width == 0` means "no saved geometry": the
// window falls back to its start-size heuristic. Validated against the live displays on restore
// (Window::restoreGeometry), so a stale rect from an unplugged monitor can never strand the window.
struct WindowGeometry {
    int x = 0;
    int y = 0;
    int width = 0; // 0 = unset
    int height = 0;
    bool maximized = false;
};

void to_json(nlohmann::json &j, const WindowGeometry &g);
void from_json(const nlohmann::json &j, WindowGeometry &g);

struct AppSettings {
    std::vector<std::string> lastProjectPaths;
    // Whether the next launch reopens lastProjectPaths.front(). Armed whenever a project is opened/saved
    // (so the open-at-exit project is restored), cleared when the user *explicitly* closes a project so a
    // deliberate close stays closed across a restart. The project itself stays in lastProjectPaths (still
    // listed on the welcome screen) — only the auto-reopen is suppressed.
    bool reopenLastProject = true;
    SimulatorState simulator;
    InputSettings input;
    HoldRepeatSettings holdRepeat;
    std::vector<MetadataPreset> metadataPresets;
    float volume = 1.0f;
    float fontSizeBase = 18.0f;
    bool showSimulator = true;
    bool showStatistics = true;
    bool showToolOptions = true;      // the Active Tool Options panel (active mode/navigator/selection onUi)
    std::string activeTheme = "Dark"; // name of the active theme (shipped "Dark"/"Light" or a user theme)
    bool hwdecEnabled = true;
    bool showTimelinePreview = false; // hover-scrub frame preview on the player seek bar
    // Pause video playback on any seek (timeline click, bookmark/chapter jump, frame step, etc.). On by
    // default; off keeps playback rolling through a seek. Project loads always land paused regardless.
    bool pauseOnSeek = true;
    // UI frame-rate cap (0 = unlimited / full refresh). Realized as an integer swap-interval divisor
    // of the display refresh, so it stays tear-free (see Application::updateSwapInterval).
    int maxFps = 0;
    bool autoBackupEnabled = true;
    // Check the GitHub releases feed for a newer version shortly after launch. A manual "Check for
    // updates" command is always available regardless; this only governs the silent startup check.
    bool checkForUpdatesOnStartup = true;
    // Memory budget (MB) for the undo/redo history. Snapshots are packed + compressed (SnapshotCodec) and
    // kept in a single byte arena (SnapshotHistory); the history grows until it hits this cap, then evicts
    // the oldest steps. Bounds RAM rather than a fixed step count, so a deep history of small edits stays
    // cheap while a few huge edits can't blow up memory.
    int undoMemoryLimitMb = 256;
    std::string language;                // "" / "en" = built-in English
    bool liveReloadTranslations = false; // opt-in translator aid; off on the normal path
    // Shared destination for intra-frame-optimized videos (UTF-8). No default is supplied: the
    // intra-optimize command stays disabled until the user picks a directory in Preferences, and the
    // user owns cleanup of its contents — the app never prunes it.
    std::string intraOutputDir;
    WindowGeometry windowGeometry;

    static AppSettings load();

    void save() const;
};

void to_json(nlohmann::json &j, const AppSettings &s);

void from_json(const nlohmann::json &j, AppSettings &s);
} // namespace ofs
