#pragma once

#include "Core/OverlaySettings.h"
#include "Localization/Translator.h"
#include "Util/FrameAllocator.h"

#include <imgui.h>

#include <cmath>

namespace ofs::ui {

// Common frame-rate presets for the Frame overlay; the parallel name array is what the preset combo shows.
inline constexpr float kCommonFpss[] = {23.976f, 24.0f, 25.0f, 29.97f, 30.0f, 48.0f, 50.0f, 59.94f, 60.0f};
inline constexpr const char *kCommonFpsNames[] = {"23.976", "24", "25", "29.97", "30", "48", "50", "59.94", "60"};

// Index of the preset matching `fps` (within 0.001), or -1 for a custom value (combo shows blank).
inline int fpsPresetIndex(float fps) {
    for (int i = 0; i < IM_ARRAYSIZE(kCommonFpss); ++i)
        if (std::abs(fps - kCommonFpss[i]) < 0.001f)
            return i;
    return -1;
}

// kTempoSubdivisionNames with index 0 ("1/1 (measure)") localized into the frame arena — the only entry
// carrying a translatable word; the rest are numeric fractions, left as-is. Main-thread render only.
inline const char **localizedTempoSnapNames() {
    auto **names = ofs::FrameAllocator::instance().allocArray<const char *>(kTempoSubdivisionCount);
    for (int i = 0; i < kTempoSubdivisionCount; ++i)
        names[i] = kTempoSubdivisionNames[i];
    names[0] = fmtScratch("1/1 ({})", Str::TlMeasure.sv());
    return names;
}

// Frame/Tempo scripting-overlay controls — the single source for editing OverlayState, shared by the
// timeline's right-click Overlay submenu, its gear settings modal, and the Project Settings overlay
// section, so the three entry points can't drift. `row(label)` emits each control's label and positions
// the next item; the sites lay rows out differently (inline text vs a form row), and `typeLabel` names
// the overlay-type combo per site. Returns true when any control edited `ov`; the caller decides
// whether/where to show the read-only video-fps readout, since form-table placement differs.
template <typename RowFn> bool renderOverlayControls(OverlayState &ov, TrKey typeLabel, RowFn &&row) {
    bool changed = false;

    row(typeLabel);
    const char *overlayNames[] = {Str::TlOverlayFrame.id("ov_frame"), Str::TlOverlayTempo.id("ov_tempo")};
    int currentOverlay = static_cast<int>(ov.overlay);
    if (ImGui::Combo("###ov_type", &currentOverlay, overlayNames, IM_ARRAYSIZE(overlayNames))) {
        ov.overlay = static_cast<ScriptingOverlay>(currentOverlay);
        changed = true;
    }

    if (ov.overlay == ScriptingOverlay::Frame) {
        int currentFpsIdx = fpsPresetIndex(ov.frameFps);
        row(Str::TlScriptFps);
        if (ImGui::Combo("###ov_fps_preset", &currentFpsIdx, kCommonFpsNames, IM_ARRAYSIZE(kCommonFpsNames))) {
            ov.frameFps = kCommonFpss[currentFpsIdx];
            changed = true;
        }

        row(Str::TlCustomFps);
        changed |= ImGui::DragFloat("###ov_custom_fps", &ov.frameFps, 0.1f, 1.0f, 240.0f);
    } else if (ov.overlay == ScriptingOverlay::Tempo) {
        row(Str::TlBpm);
        changed |= ImGui::DragFloat("###ov_bpm", &ov.tempoBpm, 0.1f, 10.0f, 500.0f);

        row(Str::TlOffsetSeconds);
        changed |= ImGui::DragFloat("###ov_tempo_offset", &ov.tempoOffsetSeconds, 0.001f, -10.0f, 10.0f, "%.3f",
                                    ImGuiSliderFlags_AlwaysClamp);

        row(Str::TlSnap);
        changed |=
            ImGui::Combo("###ov_tempo_snap", &ov.tempoMeasureIndex, localizedTempoSnapNames(), kTempoSubdivisionCount);
    }
    return changed;
}

} // namespace ofs::ui
