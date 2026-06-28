#pragma once

#include "Core/NotifyLevel.h"
#include <SDL3/SDL_audio.h>
#include <array>
#include <cstdint>
#include <random>
#include <vector>

namespace ofs {

struct AppSettings;
class EventQueue;
struct NotifyEvent;
struct UndoEvent;
struct RedoEvent;
struct CreateRegionEvent;
struct DeleteRegionEvent;
struct BakeRegionEvent;
struct BookmarkChapterCountChangedEvent;
struct UpdateAvailableEvent;
struct AddScratchAxisEvent;
struct RemoveAxisEvent;
struct ToggleAxisVisibilityEvent;
struct ToggleAxisPanelVisibilityEvent;
struct AxisSelectedEvent;
struct ShowModalEvent;
struct SetAxisGroupingEvent;
struct ShowMultiAxisEvent;
struct ShowL0OnlyEvent;

// A distinct feedback cue. Each maps to one SFX asset (see the table in the .cpp); several cues may
// share a file. Order is irrelevant except that kCount must stay last.
enum class UiCue {
    Success,
    Warning,
    Error,
    ModalOpened,
    Undo,
    Redo,
    RegionCreated,
    RegionDeleted,
    RegionBaked,
    UpdateAvailable,
    AxisAdded,
    AxisRemoved,
    AxisShown,
    AxisHidden,
    AxisActivated,
    AxisGrouped,
    kCount
};

// Plays short UI feedback sound effects (packed under data/audio/ — Kenney "UI SFX Set" clicks plus
// "Universal UI Soundpack" tones) in response
// to meaningful events on the EventQueue — toasts (success/warning/error), history (undo/redo), region
// and bookmark/chapter edits, axis changes, and so on. The mapping is purely event-driven: there
// are no UI or per-widget hooks, so a window only has to push the event it already pushes. Extend
// coverage by adding a UiCue, an entry in kCueSounds, and a subscription in the constructor.
//
// Each play applies subtle per-trigger gain/pitch variation so a repeated cue doesn't sound mechanically
// identical, and a short per-cue debounce drops same-cue bursts (e.g. a flurry of error toasts or a
// multi-region delete) so feedback never machine-guns.
//
// Behavior unit; owns only its SDL audio device + decoded PCM. Enable state and volume live in
// AppSettings (read live on each play), so a preference edit takes effect immediately. If the audio
// device can't be opened (or this is the headless test binary), the service degrades to a silent no-op.
class UiSoundService {
  public:
    UiSoundService(EventQueue &eq, const AppSettings &settings);
    ~UiSoundService();

    UiSoundService(const UiSoundService &) = delete;
    UiSoundService &operator=(const UiSoundService &) = delete;

  private:
    void onNotify(const NotifyEvent &e);
    void play(UiCue cue);

    // A decoded SFX bound to its own persistent stream. SDL mixes all bound streams, so distinct sounds
    // overlap cleanly; re-triggering the same sound clears its stream first to avoid queue build-up.
    struct Sound {
        std::vector<int16_t> pcm; // interleaved S16
        SDL_AudioStream *stream = nullptr;
    };

    const AppSettings &settings_;
    SDL_AudioDeviceID device_ = 0;
    std::vector<Sound> sounds_;
    // Index into sounds_ for each UiCue, or -1 for "no sound" (the default until a cue's asset loads).
    std::array<int, static_cast<size_t>(UiCue::kCount)> cueSound_ = [] {
        std::array<int, static_cast<size_t>(UiCue::kCount)> a{};
        a.fill(-1);
        return a;
    }();
    // Per-cue debounce window (ms), copied from kCueSounds. A repeat of the same cue within its window is
    // dropped so a same-frame / rapid burst collapses to one chirp; a cue triggered in quick succession
    // (undo/redo) declares a shorter window so its repeats still register. Only read when the cue has a
    // sound, which is exactly when the table populated it. Main-thread only.
    std::array<uint64_t, static_cast<size_t>(UiCue::kCount)> cueDebounceMs_{};
    // SDL_GetTicks() of each *sound's* last play, indexed by sounds_ index (0 = never). Keyed by sound,
    // not cue, so cues sharing one asset share a debounce gate and never clobber each other's stream. Sized
    // by kCount as a safe upper bound (sounds_ is deduped, so its size is ≤ kCount). Main-thread only.
    std::array<uint64_t, static_cast<size_t>(UiCue::kCount)> lastPlayedMs_{};
    // Drives the per-trigger gain/pitch jitter. Played only from main-thread event handlers, so unsynced.
    std::mt19937 rng_{std::random_device{}()};
};

} // namespace ofs
