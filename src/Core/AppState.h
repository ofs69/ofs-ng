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

struct TimelineViewState {
    double visibleTime = 10.0;
    double offsetTime = 0.0;
    // Hide source points in the timeline: disables both their rendering and all point
    // hit-testing so the curve can be scrubbed without grabbing points. Not serialized.
    bool showPoints = true;
    // Show the audio waveform behind the timeline curves. Opt-in (extraction is comparatively costly),
    // so it defaults off and is persisted with the project (see Format/Project showAudioWaveform).
    bool showAudioWaveform = false;
};

} // namespace ofs
