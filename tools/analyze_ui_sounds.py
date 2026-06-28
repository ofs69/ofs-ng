#!/usr/bin/env python3
"""Measure the UI SFX clips so cue->sound picks are driven by audio content, not filenames.

For every .ogg under data/audio/Audio/ this prints duration, loudness, brightness (spectral
centroid), tonalness (spectral flatness), attack time, and a rough pitch, plus sorted views. The
mapping in src/Services/UiSoundService.cpp (kCueSounds) was chosen from these numbers so each cue's
character matches its meaning and paired cues stay consistently ordered (brightness/pitch rise for the
"positive/up/forward" member, fall for the "negative/down/back" one).

    python tools/analyze_ui_sounds.py [--check]

--check re-verifies the relational orderings the current mapping relies on and exits non-zero if any
is violated (useful after swapping a filename).

Requires: numpy, soundfile, librosa.
"""
import argparse
import glob
import os
import sys

import librosa
import numpy as np
import soundfile as sf

AUDIO_ROOT = os.path.join(os.path.dirname(__file__), "..", "data", "audio")

# The cue -> file (basename, resolved under any data/audio subdir) mapping mirrored from kCueSounds,
# with the relational invariants each pair must keep. Each invariant is (cueA, cueB, feature): featureA
# must be > featureB. The notification trio is melodic (the "Universal UI Soundpack" tones), so it is
# ordered by centroid_slope — the rise/fall contour, which carries "positive vs negative" for a tune far
# better than absolute brightness — plus pitch; the click/switch pairs stay ordered by brightness+pitch.
MAPPING = {
    "Success": "Minimalist8", "Warning": "Minimalist13", "Error": "Minimalist10",
    "ModalOpened": "Coffee1",
    "Undo": "switch14", "Redo": "switch13",
    "RegionCreated": "switch2", "RegionDeleted": "switch24", "RegionBaked": "switch7",
    "UpdateAvailable": "Coffee2",
    "AxisAdded": "switch12", "AxisRemoved": "click2",
    "AxisShown": "click5", "AxisHidden": "click4",
    "AxisActivated": "switch28", "AxisGrouped": "switch26",
}
INVARIANTS = [
    # Notification trio ordered by contour: success rises, warning is flat, error falls.
    ("Success", "Warning", "centroid_slope"), ("Warning", "Error", "centroid_slope"),
    ("Redo", "Undo", "centroid"), ("Redo", "Undo", "pitch"),
    ("RegionCreated", "RegionDeleted", "centroid"), ("RegionCreated", "RegionDeleted", "pitch"),
    ("AxisAdded", "AxisRemoved", "centroid"), ("AxisAdded", "AxisRemoved", "pitch"),
    # Eye show/hide are both short bright clicks (the user rejected a darker/longer "down" hide), so only
    # the subtle pitch drop distinguishes them — no centroid ordering.
    ("AxisShown", "AxisHidden", "pitch"),
]


def resolve(name):
    hits = glob.glob(os.path.join(AUDIO_ROOT, "**", name + ".ogg"), recursive=True)
    if not hits:
        raise FileNotFoundError(f"{name}.ogg not found under {AUDIO_ROOT}")
    return hits[0]


def features(path):
    y, sr = sf.read(path, always_2d=True)
    y = y.mean(axis=1).astype(np.float32)
    rms = float(np.sqrt(np.mean(y**2))) + 1e-12
    cent_t = librosa.feature.spectral_centroid(y=y, sr=sr)[0]
    rms_t = librosa.feature.rms(y=y)[0]
    # Contour: slope of the centroid over the loud portion (energy-gated to ignore decay tails).
    e = rms_t / (rms_t.max() + 1e-9)
    c = cent_t[e > 0.15] if (e > 0.15).sum() > 3 else cent_t
    centroid_slope = float(np.polyfit(np.linspace(0, 1, len(c)), c, 1)[0]) if len(c) >= 3 else 0.0
    f0 = librosa.yin(y, fmin=80, fmax=4000, sr=sr)
    f0 = f0[np.isfinite(f0)]
    return dict(
        dur_ms=1000.0 * len(y) / sr,
        rms_db=20 * np.log10(rms),
        centroid=float(np.mean(cent_t)),
        centroid_slope=centroid_slope,
        flatness=float(np.mean(librosa.feature.spectral_flatness(y=y))),
        attack_ms=1000.0 * int(np.argmax(np.abs(y))) / sr,
        pitch=float(np.median(f0)) if len(f0) else 0.0,
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true", help="verify the mapping's relational orderings")
    args = ap.parse_args()

    if args.check:
        feats = {cue: features(resolve(fn)) for cue, fn in MAPPING.items()}
        ok = True
        for a, b, key in INVARIANTS:
            if not feats[a][key] > feats[b][key]:
                ok = False
                print(f"FAIL {a}.{key} ({feats[a][key]:.0f}) !> {b}.{key} ({feats[b][key]:.0f})")
        print("all orderings hold" if ok else "ordering check failed")
        return 0 if ok else 1

    rows = []
    for p in sorted(glob.glob(os.path.join(AUDIO_ROOT, "**", "*.ogg"), recursive=True)):
        rows.append((os.path.relpath(p, AUDIO_ROOT), features(p)))

    hdr = f"{'file':22} {'dur_ms':>7} {'rms_dB':>7} {'cent_Hz':>8} {'cSlope':>7} {'atk_ms':>7} {'pitch_Hz':>8}"
    print(hdr)
    for name, f in rows:
        print(f"{name:22} {f['dur_ms']:7.0f} {f['rms_db']:7.1f} {f['centroid']:8.0f} "
              f"{f['centroid_slope']:7.0f} {f['attack_ms']:7.1f} {f['pitch']:8.0f}")

    for title, key in [("brightness (centroid)", "centroid"), ("duration", "dur_ms")]:
        print(f"\n== sorted by {title}, low->high ==")
        for name, f in sorted(rows, key=lambda r: r[1][key]):
            print(f"  {name:16} {f[key]:8.0f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
