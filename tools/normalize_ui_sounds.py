#!/usr/bin/env python3
"""Loudness-normalize the UI SFX .ogg files in place so clips from different packs play at a consistent
perceived level.

The cue sounds come from two packs with very different levels — the loud Kenney "UI SFX Set" clicks and
the much quieter "Universal UI Soundpack" notification tones — so without this a success tone is a
whisper next to a region-create click. We bake the correction into the assets (re-encoding once) rather
than applying a runtime gain.

Method: gated RMS. Plain whole-file RMS under-reads these tones because their long decay/silence tails
drag the average down; instead we measure RMS only over windows within GATE_DB of the loudest window
(an ITU-1770-style relative gate), normalize that to TARGET_DBFS, then clamp so the true peak never
exceeds CEILING_DBFS (no clipping) and the boost never exceeds MAX_GAIN_DB (no pumping near-silent
files). A file whose correction is within DEADBAND_DB is left untouched, so re-running is a no-op (no
cumulative re-encode loss).

    python tools/normalize_ui_sounds.py            # dry run: print the gain each file would get
    python tools/normalize_ui_sounds.py --apply    # write the normalized files in place

Requires: numpy, soundfile (libsndfile with Vorbis write support).
"""
import argparse
import glob
import os
import sys

import numpy as np
import soundfile as sf

AUDIO_ROOT = os.path.join(os.path.dirname(__file__), "..", "data", "audio")
TARGET_DBFS = -20.0  # gated-RMS target
CEILING_DBFS = -1.0  # true-peak ceiling
GATE_DB = 20.0       # keep windows within this many dB of the loudest window
MAX_GAIN_DB = 24.0   # cap boost so a near-silent clip can't pump
WIN = 1024           # measurement window (samples)
# Leave a file alone when its correction is within this deadband. Wide enough (a) to skip files already
# close to target and (b) so a re-run is a true no-op: a lossy Vorbis re-encode shifts the gated
# measurement by up to ~1.5 dB, which a tighter band would chase into endless re-encodes.
DEADBAND_DB = 2.0


def db_to_lin(db):
    return float(10.0 ** (db / 20.0))


def gated_rms(mono):
    """RMS over the loud portion only (windows within GATE_DB of the loudest), ignoring decay tails."""
    n = len(mono)
    if n < WIN:
        return float(np.sqrt(np.mean(mono**2)) + 1e-12)
    powers = []
    for i in range(0, n - WIN, WIN // 2):
        powers.append(float(np.mean(mono[i:i + WIN] ** 2)))
    powers = np.asarray(powers) + 1e-12
    gate = powers.max() * db_to_lin(-GATE_DB) ** 2  # power gate
    kept = powers[powers >= gate]
    return float(np.sqrt(kept.mean()))


def compute_gain(y):
    mono = y.mean(axis=1)
    loud = gated_rms(mono)
    peak = float(np.max(np.abs(y))) + 1e-12
    gain = db_to_lin(TARGET_DBFS) / loud           # hit the loudness target...
    gain = min(gain, db_to_lin(CEILING_DBFS) / peak)  # ...but never clip
    gain = min(gain, db_to_lin(MAX_GAIN_DB))          # ...nor over-boost
    return gain


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--apply", action="store_true", help="write the files (default: dry run)")
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(AUDIO_ROOT, "**", "*.ogg"), recursive=True))
    if not files:
        print("no .ogg files found under", AUDIO_ROOT, file=sys.stderr)
        return 1

    changed = 0
    for path in files:
        y, sr = sf.read(path, always_2d=True)
        gain = compute_gain(y)
        gain_db = 20.0 * np.log10(gain)
        rel = os.path.relpath(path, AUDIO_ROOT)
        if abs(gain_db) <= DEADBAND_DB:
            print(f"  ok    {rel:28} {gain_db:+5.1f} dB (already normalized)")
            continue
        changed += 1
        action = "APPLY" if args.apply else "would"
        print(f"  {action} {rel:28} {gain_db:+5.1f} dB")
        if args.apply:
            out = np.clip(y * gain, -1.0, 1.0)
            sf.write(path, out, sr, subtype="VORBIS")

    print(f"\n{changed} file(s) {'normalized' if args.apply else 'would change'} "
          f"(target {TARGET_DBFS:g} dBFS gated, ceiling {CEILING_DBFS:g} dBFS).")
    if not args.apply and changed:
        print("Re-run with --apply to write them.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
