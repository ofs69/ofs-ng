#include "Services/UiSoundService.h"
#include "Core/EventQueue.h"
#include "Core/Events.h"       // NotifyEvent + Axis/Playback/Region events
#include "Core/UpdateEvents.h" // UpdateAvailableEvent
#include "Format/AppSettings.h"
#include "Platform/Headless.h"
#include "UI/Modals.h" // ShowModalEvent + ModalSeverity
#include "Util/Log.h"
#include "Util/Resources.h"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h> // SDL_GetTicks
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>

// The single stb_vorbis entry point we use; its implementation is compiled once into the `stb` static
// lib (lib/stb/stb_vorbis.c). Declared here directly rather than including the .c header-only to keep
// this TU clean. Decodes an in-memory Ogg Vorbis blob to interleaved S16 PCM (caller frees `output`),
// returning the per-channel frame count (or a negative value on failure).
// NOLINTBEGIN(readability-identifier-naming) — matches the third-party C symbol verbatim.
extern "C" int stb_vorbis_decode_memory(const unsigned char *mem, int len, int *channels, int *sample_rate,
                                        short **output);
// NOLINTEND(readability-identifier-naming)

namespace ofs {

namespace {

// UiCue -> SFX asset (archive-relative, read through ofs::res / data.pak). A cue with no entry stays
// silent (e.g. NotifyLevel::Info, which has no cue at all — routine info toasts shouldn't chirp). Two
// packs are drawn on: the snappy Kenney "UI SFX Set" clicks (data/audio/Audio/) for the frequent,
// mechanical cues, and the melodic "Universal UI Soundpack" tones (data/audio/OGG/) for the notification
// cues that warrant a fuller sound. Levels across both are matched by tools/normalize_ui_sounds.py.
//
// Files were chosen by measuring each clip rather than by name (tools/analyze_ui_sounds.py): the clicks
// by brightness (centroid Hz) + pitch — rising for the "positive/up/forward" member of a pair, falling
// for the "negative/down/back" one — and the melodic notification tones by contour (does the tune rise
// or fall), which carries positive-vs-negative far better than absolute brightness. Swap a filename to
// retune; rerun the script's --check to re-verify the orderings.
// Number of pooled playback voices. Each play takes one; a sound overlapping itself or others spreads
// across the pool and rings out instead of being cut. Sized well above the handful of UI sounds that can
// realistically overlap, so a voice is essentially never stolen mid-clip.
constexpr int kVoiceCount = 16;
// Linear fade applied to the head and tail of every decoded clip, so a sound whose waveform doesn't start
// or end at a zero-crossing doesn't click. A few ms is inaudible as a fade but kills the boundary pop.
constexpr float kEdgeFadeMs = 3.0f;
// Perceptual volume taper. Loudness is ~logarithmic, so the linear slider position is squared into a gain
// before it reaches SDL — this spreads the audible range evenly across the slider instead of bunching it
// at the bottom. The default uiSoundVolume (0.5) squares to the 0.25 gain the app shipped with.
constexpr float kVolumeExponent = 2.0f;

struct CueSound {
    UiCue cue;
    std::string_view asset;
};
constexpr CueSound kCueSounds[] = {
    // Notifications: melodic tones ordered by contour — success rises, warning is flat, error falls
    // (centroid slope ≈ +2473 / -207 / -1879).
    {.cue = UiCue::Success, .asset = "data/audio/OGG/Minimalist8.ogg"},  // rising — positive
    {.cue = UiCue::Warning, .asset = "data/audio/OGG/Minimalist13.ogg"}, // flat — neutral attention
    {.cue = UiCue::Error, .asset = "data/audio/OGG/Minimalist10.ogg"},   // falling — negative
    // A modal opening: a soft, neutral chime distinct from the notification trio (Warning/Error modals
    // reuse those tones; this covers the Info case — confirmations, file dialogs, the axis picker).
    {.cue = UiCue::ModalOpened, .asset = "data/audio/OGG/Coffee1.ogg"},
    // History: a mirrored ~30 ms tick pair distinguished by pitch — undo "back" (low), redo "forward"
    // (high). Kept extra short because undo/redo are held and fire in quick succession, so a longer clip
    // would smear.
    {.cue = UiCue::Undo, .asset = "data/audio/Audio/switch14.ogg"}, // 7361 / 805
    {.cue = UiCue::Redo, .asset = "data/audio/Audio/switch13.ogg"}, // 7554 / 2362
    // Regions: created "up" (brighter/higher) vs deleted "down" (darker/lower); baked = decisive commit.
    {.cue = UiCue::RegionCreated, .asset = "data/audio/Audio/switch2.ogg"},  // 5477 /  357
    {.cue = UiCue::RegionDeleted, .asset = "data/audio/Audio/switch24.ogg"}, // 3299 /  113
    {.cue = UiCue::RegionBaked, .asset = "data/audio/Audio/switch7.ogg"},    // 4951 / 1810 — short, solid
    // A newer release exists — a warm, longer chime that reads as a calm notification.
    {.cue = UiCue::UpdateAvailable, .asset = "data/audio/OGG/Coffee2.ogg"},
    // Axis presence — create/destroy a scratch axis and the "Show in Panel" add/remove-from-strip toggle
    // share this pair: added is a bright/high switch tick, removed a soft/low one (centroid 6739 vs 2708).
    {.cue = UiCue::AxisAdded, .asset = "data/audio/Audio/switch12.ogg"}, // 6739 / 2790
    {.cue = UiCue::AxisRemoved, .asset = "data/audio/Audio/click2.ogg"}, // 2708 /   82 — short, soft, low
    // Axis row visibility (the timeline-row eye toggle) — a view change, so two short, bright, distinct
    // clicks (NOT a darker/longer "down" sound): show is higher-pitched, hide a touch lower.
    {.cue = UiCue::AxisShown, .asset = "data/audio/Audio/click5.ogg"},  // 4741 / 1997
    {.cue = UiCue::AxisHidden, .asset = "data/audio/Audio/click4.ogg"}, // 5234 / 1627
    // Axis activation (active-axis change) gets a subtle focus tick; grouping (forming a multi-axis edit
    // group) a fuller, distinct sound.
    {.cue = UiCue::AxisActivated, .asset = "data/audio/Audio/switch28.ogg"}, // mellow focus tick (low-ish
                                                                             // brightness, gentle slope)
    {.cue = UiCue::AxisGrouped, .asset = "data/audio/Audio/switch26.ogg"},   // group formed
};

// NotifyLevel -> cue. Info maps to nothing (silent). Returns kCount for "no cue".
UiCue cueForLevel(NotifyLevel level) {
    switch (level) {
    case NotifyLevel::Success:
        return UiCue::Success;
    case NotifyLevel::Warning:
        return UiCue::Warning;
    case NotifyLevel::Error:
        return UiCue::Error;
    case NotifyLevel::Info:
        break;
    }
    return UiCue::kCount;
}

} // namespace

UiSoundService::UiSoundService(EventQueue &eq, const AppSettings &settings) : settings_(settings) {
    eq.on<NotifyEvent>([this](const NotifyEvent &e) { onNotify(e); });
    // History cues are driven by HistoryNavigatedEvent — emitted by UndoSystem only when an undo/redo
    // actually moved the stack — not by the Undo/RedoEvent *request*, so a no-op press at the end of the
    // history is silent. undo "back" (low), redo "forward" (high).
    eq.on<HistoryNavigatedEvent>([this](const HistoryNavigatedEvent &e) { play(e.redo ? UiCue::Redo : UiCue::Undo); });
    // Regions: ProjectManager emits RegionChangedEvent only on a *completed* create/delete/bake — a request
    // that found no room or no target is silent. Created "up", deleted "down", baked = decisive commit.
    eq.on<RegionChangedEvent>([this](const RegionChangedEvent &e) {
        play(e.kind == RegionChangeKind::Created   ? UiCue::RegionCreated
             : e.kind == RegionChangeKind::Deleted ? UiCue::RegionDeleted
                                                   : UiCue::RegionBaked);
    });
    // Bookmarks and chapters reuse the region create/delete cues — ProjectManager collapses every
    // add/remove call site into this one count-changed signal.
    eq.on<BookmarkChapterCountChangedEvent>([this](const BookmarkChapterCountChangedEvent &e) {
        play(e.added ? UiCue::RegionCreated : UiCue::RegionDeleted);
    });
    eq.on<UpdateAvailableEvent>([this](const UpdateAvailableEvent &) { play(UiCue::UpdateAvailable); });
    // Any modal opening (confirm, custom body, file dialog, axis picker) chirps. A warning/error modal
    // reuses the matching notification tone; an info modal gets the neutral ModalOpened chime. The body
    // path tints from ShowModalEvent.severity; the message/spec path from spec.severity (file dialogs and
    // the axis picker leave both at the default Info).
    eq.on<ShowModalEvent>([this](const ShowModalEvent &e) {
        const ModalSeverity sev = e.body ? e.severity : e.spec.severity;
        play(sev == ModalSeverity::Error     ? UiCue::Error
             : sev == ModalSeverity::Warning ? UiCue::Warning
                                             : UiCue::ModalOpened);
    });
    // Axis presence: ProjectManager emits AxisPresenceChangedEvent only on a *real* flip — scratch
    // add/remove, the "Show in Panel" strip toggle, and the bulk multi-axis / L0-only presets all map to
    // the strip add/remove cue pair; the timeline-row eye toggle is just a view change and gets its own
    // pair (AxisShown/AxisHidden). A no-op request (L0 always in-panel, a preset with nothing to change,
    // all scratch slots full) emits nothing, so it stays silent.
    eq.on<AxisPresenceChangedEvent>([this](const AxisPresenceChangedEvent &e) {
        switch (e.change) {
        case AxisPresence::AddedToStrip:
            play(UiCue::AxisAdded);
            break;
        case AxisPresence::RemovedFromStrip:
            play(UiCue::AxisRemoved);
            break;
        case AxisPresence::Shown:
            play(UiCue::AxisShown);
            break;
        case AxisPresence::Hidden:
            play(UiCue::AxisHidden);
            break;
        }
    });
    // A real active-axis change chirps — ProjectManager emits ActiveAxisChangedEvent when an accepted
    // selection (including programmatic ones: project load, axis add/remove, a plugin) or a group
    // lead-switch actually moves the active axis. Observing this outcome instead of the AxisSelectedEvent
    // request keeps a rejected or no-op selection silent; per-frame coalescing collapses a same-frame burst.
    eq.on<ActiveAxisChangedEvent>([this](const ActiveAxisChangedEvent &) { play(UiCue::AxisActivated); });
    // AxisGroupingChangedEvent fires only when a real multi-axis group forms/changes, not on the
    // SetAxisGroupingEvent request (which also fires to dissolve a group or re-issue the same one).
    eq.on<AxisGroupingChangedEvent>([this](const AxisGroupingChangedEvent &) { play(UiCue::AxisGrouped); });

    // No SelectionChangedEvent cue on purpose: the action selection changes on every timeline click and
    // box-select drag, so cueing it would machine-gun during normal editing.

    // The headless test binary has no audio device; stay a silent no-op there.
    if (kHeadless)
        return;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        OFS_CORE_WARN("UI sounds disabled: SDL audio init failed: {}", SDL_GetError());
        return;
    }
    device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (device_ == 0) {
        OFS_CORE_WARN("UI sounds disabled: no audio device: {}", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return;
    }

    SDL_AudioSpec devSpec{};
    SDL_GetAudioDeviceFormat(device_, &devSpec, nullptr);

    // A fixed pool of device-bound voices, shared across all sounds. SDL mixes all bound streams, so any
    // subset can sound at once; each play sets the chosen voice's source format to its clip's. Created in
    // the device format (src == dst) as a neutral default, overwritten per play.
    voices_.reserve(kVoiceCount);
    for (int i = 0; i < kVoiceCount; ++i) {
        SDL_AudioStream *stream = SDL_CreateAudioStream(&devSpec, &devSpec);
        if (stream == nullptr) {
            OFS_CORE_WARN("UI sounds: SDL_CreateAudioStream failed: {}", SDL_GetError());
            continue;
        }
        SDL_BindAudioStream(device_, stream);
        SDL_ResumeAudioStreamDevice(stream);
        voices_.push_back({.stream = stream});
    }

    // Decode each distinct asset once (deduped — several cues may map to one file) to interleaved S16,
    // keeping its source format and length for playback on any pooled voice.
    std::vector<std::string_view> loaded; // parallel to sounds_, for dedup
    auto loadSound = [&](std::string_view asset) -> int {
        for (size_t i = 0; i < loaded.size(); ++i)
            if (loaded[i] == asset)
                return static_cast<int>(i);

        const auto bytes = ofs::res::read(asset);
        if (!bytes)
            return -1; // res::read already logged the miss

        int channels = 0, rate = 0;
        short *out = nullptr;
        const int frames = stb_vorbis_decode_memory(reinterpret_cast<const unsigned char *>(bytes->data()),
                                                    static_cast<int>(bytes->size()), &channels, &rate, &out);
        if (frames < 0 || out == nullptr || channels <= 0 || rate <= 0) {
            OFS_CORE_WARN("UI sounds: failed to decode {}", asset);
            std::free(out);
            return -1;
        }

        Sound sound;
        sound.spec = {.format = SDL_AUDIO_S16, .channels = channels, .freq = rate};
        sound.durationMs = static_cast<uint32_t>(static_cast<int64_t>(frames) * 1000 / rate);
        sound.pcm.assign(out, out + static_cast<size_t>(frames) * static_cast<size_t>(channels));
        std::free(out);

        // Linear head/tail fade so a clip that doesn't begin/end at a zero-crossing doesn't click. Capped
        // at half the clip so a very short tick still gets a symmetric in/out ramp.
        const int fade = std::min(frames / 2, static_cast<int>(static_cast<float>(rate) * kEdgeFadeMs / 1000.0f));
        for (int f = 0; f < fade; ++f) {
            const float g = static_cast<float>(f + 1) / static_cast<float>(fade + 1);
            for (int c = 0; c < channels; ++c) {
                int16_t &head = sound.pcm[static_cast<size_t>(f) * channels + c];
                int16_t &tail = sound.pcm[static_cast<size_t>(frames - 1 - f) * channels + c];
                head = static_cast<int16_t>(static_cast<float>(head) * g);
                tail = static_cast<int16_t>(static_cast<float>(tail) * g);
            }
        }

        sounds_.push_back(std::move(sound));
        loaded.push_back(asset);
        return static_cast<int>(sounds_.size() - 1);
    };

    for (const auto &m : kCueSounds)
        cueSound_[static_cast<int>(m.cue)] = loadSound(m.asset);
}

UiSoundService::~UiSoundService() {
    for (auto &v : voices_)
        if (v.stream)
            SDL_DestroyAudioStream(v.stream); // unbinds from the device
    if (device_ != 0) {
        SDL_CloseAudioDevice(device_);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

void UiSoundService::onNotify(const NotifyEvent &e) {
    play(cueForLevel(e.level));
}

void UiSoundService::play(UiCue cue) {
    if (device_ == 0 || cue == UiCue::kCount)
        return;
    const int idx = cueSound_[static_cast<size_t>(cue)];
    if (idx < 0)
        return;
    // Record the sound, deduped by index: queued any number of times this frame, it plays once at flush.
    // Keying on the sound (not the cue) collapses two distinct cues that resolve to the same asset too.
    pendingSound_[static_cast<size_t>(idx)] = true;
}

void UiSoundService::update() {
    if (device_ == 0 || voices_.empty())
        return;
    // Settings are read live so a preference edit takes effect immediately; if sounds were just disabled,
    // drop whatever this frame queued rather than letting it backlog.
    const bool enabled = settings_.uiSoundsEnabled;
    for (size_t i = 0; i < pendingSound_.size(); ++i) {
        if (!pendingSound_[i])
            continue;
        pendingSound_[i] = false;
        if (enabled && i < sounds_.size())
            trigger(i);
    }
}

void UiSoundService::trigger(size_t soundIdx) {
    Sound &s = sounds_[soundIdx];

    // Subtle per-trigger variation so a repeated cue never sounds mechanically identical. Both jitters are
    // normal distributions clustered tight around natural, so most plays sit near the original and the
    // occasional one varies gently — smoother than a uniform spread that lingers at the extremes. Gain is
    // one-sided (never above the configured volume); pitch wobbles a hair either way. Constructed once;
    // trigger() is main-thread only, so the shared rng_ is safe.
    static std::normal_distribution<float> gainJitter(0.96f, 0.03f);
    static std::normal_distribution<float> pitchJitter(1.0f, 0.012f);
    const float vol = std::pow(std::clamp(settings_.uiSoundVolume, 0.0f, 1.0f), kVolumeExponent);
    const float gain = vol * std::clamp(gainJitter(rng_), 0.88f, 1.0f);
    const float pitch = std::clamp(pitchJitter(rng_), 0.97f, 1.03f);

    // Take a free voice (clip already finished), else steal the one closest to finishing — that cuts the
    // least remaining audio, and with kVoiceCount voices it essentially never happens for UI feedback.
    const uint64_t now = SDL_GetTicks();
    size_t pick = 0;
    for (size_t v = 0; v < voices_.size(); ++v) {
        if (voices_[v].freeAtMs <= now) {
            pick = v;
            break;
        }
        if (voices_[v].freeAtMs < voices_[pick].freeAtMs)
            pick = v;
    }
    Voice &voice = voices_[pick];

    SDL_ClearAudioStream(voice.stream); // empty for a free voice; flushes the old clip when stealing
    SDL_SetAudioStreamFormat(voice.stream, &s.spec, nullptr);
    SDL_SetAudioStreamGain(voice.stream, gain);
    SDL_SetAudioStreamFrequencyRatio(voice.stream, pitch);
    SDL_PutAudioStreamData(voice.stream, s.pcm.data(), static_cast<int>(s.pcm.size() * sizeof(int16_t)));
    SDL_FlushAudioStream(voice.stream);
    // Pitch scales playback duration (a higher ratio plays faster). Mark when the voice frees, with a small
    // margin so we don't reclaim a stream that's still draining its final samples.
    voice.freeAtMs = now + static_cast<uint64_t>(static_cast<float>(s.durationMs) / pitch) + 20;
}

} // namespace ofs
