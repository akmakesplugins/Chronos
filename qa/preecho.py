#!/usr/bin/env python3
"""Measure attack quality directly: pre-echo level and HF rise time.

Pre-echo is the phase vocoder's signature drum artifact — energy smeared BACKWARD
before the hit, heard as a soft "whoosh" ahead of every attack. It is invisible to
whole-file metrics, so it gets its own measurement, taken relative to each detected
onset:

  preecho_db   mean HF (>2 kHz) energy in [-60, -12] ms before the attack, relative
               to the attack peak. Lower is sharper. The input itself sets the floor.
  rise_ms      time for the HF envelope to go from -20 dB to -1 dB of the attack
               peak. Lower is snappier.

Usage: preecho.py --input IN.wav --version NAME WAV [--version NAME WAV ...]
"""

import argparse

import numpy as np
import soundfile as sf
from scipy.signal import butter, sosfiltfilt


def hf_envelope(x, sr, cutoff=2000.0):
    sos = butter(4, cutoff / (sr / 2), btype="highpass", output="sos")
    hf = sosfiltfilt(sos, x)
    win = max(8, int(0.001 * sr))                 # 1 ms RMS window
    n = len(hf) // win
    env = np.sqrt(np.mean(hf[: n * win].reshape(n, win) ** 2, axis=1) + 1e-20)
    return env, sr / win                          # envelope + its rate


def find_onsets(env, erate, min_gap_s=0.08):
    """Onsets from the envelope's own rising edges, above a global threshold."""
    thr = np.percentile(env, 90) * 0.5
    peaks = []
    last = -1e9
    for i in range(2, len(env) - 2):
        if env[i] < thr:
            continue
        if env[i] >= env[i - 1] and env[i] > env[i + 1] and env[i] > 3.0 * env[i - 2]:
            t = i / erate
            if t - last >= min_gap_s:
                peaks.append(i)
                last = t
    return peaks


def measure(path, cutoff=2000.0):
    x, sr = sf.read(path, always_2d=True)
    mono = x.mean(axis=1)
    env, erate = hf_envelope(mono, sr, cutoff)
    onsets = find_onsets(env, erate)

    pre_lo, pre_hi = int(0.060 * erate), int(0.012 * erate)
    pres, rises = [], []
    for o in onsets:
        if o - pre_lo < 0 or o + int(0.05 * erate) >= len(env):
            continue
        peak = env[o:o + int(0.02 * erate)].max() + 1e-20
        pre = env[o - pre_lo:o - pre_hi]
        if len(pre) == 0:
            continue
        pres.append(20 * np.log10(pre.mean() / peak))

        # Rise: walk back from the peak to -20 dB, forward to -1 dB.
        lo_t = hi_t = None
        for j in range(o, max(o - pre_lo, 0), -1):
            if env[j] <= peak * 10 ** (-20 / 20):
                lo_t = j
                break
        for j in range(lo_t if lo_t else o, min(o + int(0.05 * erate), len(env))):
            if env[j] >= peak * 10 ** (-1 / 20):
                hi_t = j
                break
        if lo_t is not None and hi_t is not None and hi_t > lo_t:
            rises.append((hi_t - lo_t) / erate * 1000.0)

    return {
        "onsets": len(onsets),
        "preecho_db": float(np.median(pres)) if pres else float("nan"),
        "rise_ms": float(np.median(rises)) if rises else float("nan"),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True)
    ap.add_argument("--version", action="append", nargs=2, metavar=("NAME", "WAV"), default=[])
    args = ap.parse_args()

    m = measure(args.input)
    print(f"{'input':14s} preecho={m['preecho_db']:7.1f} dB  rise={m['rise_ms']:6.2f} ms  ({m['onsets']} onsets)")
    for name, path in args.version:
        m = measure(path)
        print(f"{name:14s} preecho={m['preecho_db']:7.1f} dB  rise={m['rise_ms']:6.2f} ms  ({m['onsets']} onsets)")


if __name__ == "__main__":
    main()
