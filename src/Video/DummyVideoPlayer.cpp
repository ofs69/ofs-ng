#include "DummyVideoPlayer.h"
#include "Core/Events.h"
#include <algorithm>

namespace ofs {

DummyVideoPlayer::DummyVideoPlayer(EventQueue &eventQueue) : eventQueue(eventQueue) {}

bool DummyVideoPlayer::init() {
    eventQueue.on<ChangeDummyDurationEvent>([this](const ChangeDummyDurationEvent &e) {
        active = true;
        duration = e.durationSeconds;
        position = 0.0;
        paused = true;
        speed = 1.0f;
        eventQueue.push(DurationChangedEvent{duration});
        eventQueue.push(PlayStateChangedEvent{false});
    });

    eventQueue.on<LoadVideoEvent>([this](const LoadVideoEvent &) { active = false; });

    eventQueue.on<CloseVideoEvent>([this](const CloseVideoEvent &) { active = false; });

    eventQueue.on<SeekEvent>([this](const SeekEvent &e) {
        if (!active)
            return;
        setPosition(e.time);
    });

    eventQueue.on<PlayPauseEvent>([this](const PlayPauseEvent &) {
        if (!active)
            return;
        paused = !paused;
        eventQueue.push(PlayStateChangedEvent{!paused});
    });

    eventQueue.on<PlaybackSpeedEvent>([this](const PlaybackSpeedEvent &e) {
        if (!active)
            return;
        setPlaybackSpeed(e.speed);
        eventQueue.push(SpeedChangedEvent{speed});
    });

    eventQueue.on<VolumeChangedEvent>([this](const VolumeChangedEvent &e) {
        if (!active)
            return;
        setVolume(e.volume);
    });

    return true;
}

void DummyVideoPlayer::update(float dt) {
    if (!active || paused)
        return;
    position = std::min(position + static_cast<double>(dt) * speed, duration);
    if (position >= duration) {
        paused = true;
        eventQueue.push(PlayStateChangedEvent{false});
    }
}

} // namespace ofs
