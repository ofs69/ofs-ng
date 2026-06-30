#pragma once

namespace ofs {

// Registry-context mirror of the player's smooth cursor. Updated every frame
// from player->getLogicalPosition() so consumers without a player reference
// (e.g. PluginManager's C-ABI host functions) can read it from ScriptProject.
struct PlaybackState {
    double cursorPos = 0.0;
};

struct AppState {
    bool showShortcutWindow = false;
    bool showConfigWindow = false;
    bool showProjectConfigWindow = false;
    bool showLogWindow = false;
    bool showAboutWindow = false;
    bool showBackupRestoreWindow = false;
};

struct ProcessingSelectionState {
    int regionId = -1; // -1 = nothing selected; otherwise matches ProcessingRegion::id
};

// How the multi-axis curves share the timeline's vertical space. Overlay (default) z-stacks every
// visible axis into one shared 0-100 band — best for comparing axes; Lanes gives each drawn axis its
// own horizontal row with its own 0-100 band — best for reading/editing one axis cleanly.
enum class TimelineLayout {
    Overlay,
    Lanes,
};

struct TimelineViewState {
    double visibleTime = 10.0;
    double offsetTime = 0.0;
    // Hide source points in the timeline: disables both their rendering and all point
    // hit-testing so the curve can be scrubbed without grabbing points. Not serialized.
    bool showPoints = true;
    // Show the audio waveform behind the timeline curves. Opt-in (extraction is comparatively costly),
    // so it defaults off and is persisted with the project (see Format/Project showAudioWaveform).
    bool showAudioWaveform = false;
    // Overlay (z-stacked) vs Lanes (one row per axis). Persisted with the project
    // (see Format/Project timelineLayout).
    TimelineLayout layout = TimelineLayout::Overlay;
};

} // namespace ofs
