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
    bool showMetadataWindow = false;
    bool showLogWindow = false;
    bool showAboutWindow = false;
    bool showBackupRestoreWindow = false;
    bool showWebSocketApiWindow = false;
};

struct ProcessingSelectionState {
    int regionId = -1; // -1 = nothing selected; otherwise matches ProcessingRegion::id
};

// How the multi-axis script lines share the timeline's vertical space. Overlay (default) z-stacks every
// visible axis into one shared 0-100 band — best for comparing axes; Lanes gives each drawn axis its
// own horizontal row with its own 0-100 band — best for reading/editing one axis cleanly.
enum class TimelineLayout {
    Overlay,
    Lanes,
};

struct TimelineViewState {
    static constexpr double kDefaultVisibleTime = 10.0;
    // Zoom bounds, in seconds of visible timeline. The floor is set by the millisecond grid authored actions
    // sit on: a dot bucket spans 2*dotRadius px and snaps to a power-of-two ladder over 1 ms, so the first
    // bucket that can separate two adjacent grid slots is the 0.5 ms step — which needs a window of
    // 0.0005 * width / (2*dotRadius), about 12 ms across a 400 px lane. Zooming all the way in therefore
    // always resolves a cluster into its individual points, in a narrow Lanes row as well as a full-width
    // band. A 1 ms bucket is not enough: it lands adjacent slots in the same bucket as often as not, because
    // at->bucket division is not exact at the boundary.
    static constexpr double kMinVisibleTime = 0.01;
    static constexpr double kMaxVisibleTime = 300.0;

    // Live, eased span the timeline is drawing this frame (mirrored from the window every frame).
    double visibleTime = kDefaultVisibleTime;
    // The zoom the user settled on — the span visibleTime eases toward. Persisted with the project
    // (see Format/Project timelineVisibleTime).
    double targetVisibleTime = kDefaultVisibleTime;
    double offsetTime = 0.0;
    // Hide source points in the timeline: disables both their rendering and all point
    // hit-testing so the script line can be scrubbed without grabbing points. Not serialized.
    bool showPoints = true;
    // Show the audio waveform behind the timeline script lines. Opt-in (extraction is comparatively costly),
    // so it defaults off and is persisted with the project (see Format/Project showAudioWaveform).
    bool showAudioWaveform = false;
    // Overlay (z-stacked) vs Lanes (one row per axis). Persisted with the project
    // (see Format/Project timelineLayout).
    TimelineLayout layout = TimelineLayout::Overlay;
};

} // namespace ofs
