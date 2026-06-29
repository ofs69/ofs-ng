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
struct HistoryNavigatedEvent;
struct RegionChangedEvent;
struct BookmarkChapterCountChangedEvent;
struct UpdateAvailableEvent;
struct AxisPresenceChangedEvent;
struct AxisSelectedEvent;
struct ShowModalEvent;
struct AxisGroupingChangedEvent;

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
// are no UI or per-widget hooks.
//
// The cues key off *outcome* events — ones the owning service emits only after the change actually lands
// (HistoryNavigatedEvent, RegionChangedEvent, AxisPresenceChangedEvent, BookmarkChapterCountChangedEvent,
// …) — never off the *request* event that triggered the attempt. A request can no-op (undo at the end of
// the stack, a region with no room, a preset with nothing to change), and cueing the request would chirp
// when nothing happened. Observing the outcome keeps this a pure observer: the no-op classification lives
// in the one service that performs the mutation, not duplicated here. Extend coverage by adding a UiCue,
// an entry in kCueSounds, and a subscription to the owning service's outcome event in the constructor.
//
// Cues requested during a frame's event drain are coalesced by sound and flushed once per frame by
// update() — so a same-frame burst (a multi-region delete, a cue pushed from two code paths) collapses to
// one chirp, deterministically and without a time-based debounce. Playback runs through a small pool of
// voices: a sound takes a free voice and rings out instead of being cut, so repeats overlap cleanly. Each
// play applies subtle per-trigger gain/pitch variation so a repeat never sounds mechanically identical.
//
// Behavior unit; owns only its SDL audio device, decoded PCM, and voice pool. Enable state and volume live
// in AppSettings (read live on each play — the volume slider is squared into a perceptual gain), so a
// preference edit takes effect immediately. If the audio device can't be opened (or this is the headless
// test binary), the service degrades to a silent no-op.
class UiSoundService {
  public:
    UiSoundService(EventQueue &eq, const AppSettings &settings);
    ~UiSoundService();

    UiSoundService(const UiSoundService &) = delete;
    UiSoundService &operator=(const UiSoundService &) = delete;

    // Emit the cues requested during this frame's event drain — one play per distinct sound, so a
    // same-frame burst (a multi-region delete, a cue pushed from two code paths in one drain) collapses to
    // a single chirp with no time-based debounce. Call once per frame, right after EventQueue::drain().
    void update();

  private:
    void onNotify(const NotifyEvent &e);
    void play(UiCue cue);          // mark the cue's sound to play at this frame's flush
    void trigger(size_t soundIdx); // emit one sound on a pooled voice (called from update())

    // A decoded SFX: interleaved S16 PCM plus the source format and clip length needed to play it on any
    // pooled voice and to schedule that voice's release.
    struct Sound {
        std::vector<int16_t> pcm;
        SDL_AudioSpec spec{};    // source format (sample rate / channels) of this clip
        uint32_t durationMs = 0; // clip length at natural pitch
    };

    // A playback voice — one device-bound stream out of a small fixed pool. Each play takes a free voice
    // (or steals the one closest to finishing), so a sound can overlap itself or another and rings out
    // naturally instead of being truncated. `freeAtMs` is SDL_GetTicks() when the current clip ends
    // (0 = idle); the source format is set per play, since voices are shared across all sounds.
    struct Voice {
        SDL_AudioStream *stream = nullptr;
        uint64_t freeAtMs = 0;
    };

    const AppSettings &settings_;
    SDL_AudioDeviceID device_ = 0;
    std::vector<Sound> sounds_;
    std::vector<Voice> voices_;
    // Index into sounds_ for each UiCue, or -1 for "no sound" (the default until a cue's asset loads).
    std::array<int, static_cast<size_t>(UiCue::kCount)> cueSound_ = [] {
        std::array<int, static_cast<size_t>(UiCue::kCount)> a{};
        a.fill(-1);
        return a;
    }();
    // Sounds requested during the current frame's drain, deduped by sounds_ index (which is ≤ kCount), so
    // the same sound queued N times this frame plays once. Flushed and cleared by update(). Main-thread only.
    std::array<bool, static_cast<size_t>(UiCue::kCount)> pendingSound_{};
    // Drives the per-trigger gain/pitch variation. Stepped only from the main thread, so unsynced.
    std::mt19937 rng_{std::random_device{}()};
};

} // namespace ofs
