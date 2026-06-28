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
// Default per-cue debounce window (ms): a same-cue burst within this window collapses to one chirp. A
// cue the user triggers in quick succession (undo/redo held down) overrides it with the shorter rapid
// window so its repeats still register instead of being swallowed.
constexpr uint32_t kDefaultDebounceMs = 60;
constexpr uint32_t kRapidDebounceMs = 20;

struct CueSound {
    UiCue cue;
    std::string_view asset;
    uint32_t debounceMs = kDefaultDebounceMs;
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
    // (high). Kept extra short, with the rapid debounce, because undo/redo are held and fire in quick
    // succession, so a longer clip would smear and a longer window would swallow repeats.
    {.cue = UiCue::Undo, .asset = "data/audio/Audio/switch14.ogg", .debounceMs = kRapidDebounceMs}, // 7361 / 805
    {.cue = UiCue::Redo, .asset = "data/audio/Audio/switch13.ogg", .debounceMs = kRapidDebounceMs}, // 7554 / 2362
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
    // The existing undo/redo request events drive the history cues — no separate "applied" signal needed.
    // A no-op press (empty stack) still chirps, which reads as ordinary key feedback.
    eq.on<UndoEvent>([this](const UndoEvent &) { play(UiCue::Undo); });
    eq.on<RedoEvent>([this](const RedoEvent &) { play(UiCue::Redo); });
    eq.on<CreateRegionEvent>([this](const CreateRegionEvent &) { play(UiCue::RegionCreated); });
    eq.on<DeleteRegionEvent>([this](const DeleteRegionEvent &) { play(UiCue::RegionDeleted); });
    eq.on<BakeRegionEvent>([this](const BakeRegionEvent &) { play(UiCue::RegionBaked); });
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
    eq.on<AddScratchAxisEvent>([this](const AddScratchAxisEvent &) { play(UiCue::AxisAdded); });
    eq.on<RemoveAxisEvent>([this](const RemoveAxisEvent &) { play(UiCue::AxisRemoved); });
    // The two visibility toggles get distinct cues. The "Show in Panel" toggle adds/removes the axis from
    // the strip, so it shares the create/remove-scratch-axis cue (AxisAdded/AxisRemoved); the timeline-row
    // eye toggle is just a view change and gets its own pair (AxisShown/AxisHidden).
    eq.on<ToggleAxisVisibilityEvent>(
        [this](const ToggleAxisVisibilityEvent &e) { play(e.visible ? UiCue::AxisShown : UiCue::AxisHidden); });
    eq.on<ToggleAxisPanelVisibilityEvent>(
        [this](const ToggleAxisPanelVisibilityEvent &e) { play(e.inPanel ? UiCue::AxisAdded : UiCue::AxisRemoved); });
    // Any active-axis change chirps. This fires on programmatic selection too (project load, axis
    // add/remove, undo restore, a plugin); the per-cue debounce keeps a burst from machine-gunning.
    eq.on<AxisSelectedEvent>([this](const AxisSelectedEvent &) { play(UiCue::AxisActivated); });
    eq.on<SetAxisGroupingEvent>([this](const SetAxisGroupingEvent &) { play(UiCue::AxisGrouped); });
    // The Axes-menu layout presets bulk-show / bulk-hide the main axes, so they reuse the panel-presence
    // pair: expand to multi-axis = "added", collapse to L0 only = "removed".
    eq.on<ShowMultiAxisEvent>([this](const ShowMultiAxisEvent &) { play(UiCue::AxisAdded); });
    eq.on<ShowL0OnlyEvent>([this](const ShowL0OnlyEvent &) { play(UiCue::AxisRemoved); });

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

    // Decode each distinct asset once (deduped — several levels may map to one file) into its own
    // device-bound stream. SDL mixes all bound streams, so different sounds overlap cleanly.
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
        if (frames < 0 || out == nullptr) {
            OFS_CORE_WARN("UI sounds: failed to decode {}", asset);
            return -1;
        }

        const SDL_AudioSpec srcSpec{.format = SDL_AUDIO_S16, .channels = channels, .freq = rate};
        SDL_AudioStream *stream = SDL_CreateAudioStream(&srcSpec, &devSpec);
        if (stream == nullptr) {
            OFS_CORE_WARN("UI sounds: SDL_CreateAudioStream failed: {}", SDL_GetError());
            std::free(out);
            return -1;
        }
        SDL_BindAudioStream(device_, stream);
        SDL_ResumeAudioStreamDevice(stream);

        Sound sound;
        sound.pcm.assign(out, out + static_cast<size_t>(frames) * static_cast<size_t>(channels));
        sound.stream = stream;
        std::free(out);

        sounds_.push_back(std::move(sound));
        loaded.push_back(asset);
        return static_cast<int>(sounds_.size() - 1);
    };

    for (const auto &m : kCueSounds) {
        cueSound_[static_cast<int>(m.cue)] = loadSound(m.asset);
        cueDebounceMs_[static_cast<int>(m.cue)] = m.debounceMs;
    }
}

UiSoundService::~UiSoundService() {
    for (auto &s : sounds_)
        if (s.stream)
            SDL_DestroyAudioStream(s.stream); // unbinds from the device
    if (device_ != 0) {
        SDL_CloseAudioDevice(device_);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

void UiSoundService::onNotify(const NotifyEvent &e) {
    play(cueForLevel(e.level));
}

void UiSoundService::play(UiCue cue) {
    if (device_ == 0 || !settings_.uiSoundsEnabled || cue == UiCue::kCount)
        return;
    const int idx = cueSound_[static_cast<size_t>(cue)];
    if (idx < 0)
        return;

    // Debounce per *sound*, not per cue: collapse a burst (a flurry of error toasts, a multi-region delete
    // that pushes one DeleteRegionEvent per region) into a single chirp instead of machine-gunning. Keying
    // on the shared stream also stops two distinct cues that resolve to the same asset from clobbering each
    // other — the SDL_ClearAudioStream below would otherwise cut the first off mid-play. The window comes
    // from the cue, so a rapidly-repeated cue can opt into a shorter one via kCueSounds.
    const uint64_t now = SDL_GetTicks();
    uint64_t &last = lastPlayedMs_[static_cast<size_t>(idx)];
    if (last != 0 && now - last < cueDebounceMs_[static_cast<size_t>(cue)])
        return;
    last = now;

    Sound &s = sounds_[static_cast<size_t>(idx)];
    // Subtle per-trigger variation so a repeated cue never sounds mechanically identical. Gain only ever
    // attenuates, so a play never exceeds the user's configured volume; pitch wobbles ±3% around natural.
    // Stateless distributions, constructed once; play() is main-thread only, so the shared rng_ is safe.
    static std::uniform_real_distribution<float> gainJitter(0.85f, 1.0f);
    static std::uniform_real_distribution<float> pitchJitter(0.97f, 1.03f);
    SDL_SetAudioStreamGain(s.stream, std::clamp(settings_.uiSoundVolume, 0.0f, 1.0f) * gainJitter(rng_));
    SDL_SetAudioStreamFrequencyRatio(s.stream, pitchJitter(rng_));
    // Drop anything still queued from a rapid re-trigger so feedback stays prompt, then queue this play.
    SDL_ClearAudioStream(s.stream);
    SDL_PutAudioStreamData(s.stream, s.pcm.data(), static_cast<int>(s.pcm.size() * sizeof(int16_t)));
    SDL_FlushAudioStream(s.stream);
}

} // namespace ofs
