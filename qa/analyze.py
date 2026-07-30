#!/usr/bin/env python3
"""Chronos feedback-loop analyzer.

Given an input loop and one or more processed (time-stretched) versions, compute
listening-proxy metrics that expose the ways a stretcher degrades real material,
and render a stacked spectrogram PNG for visual artifact inspection.

Metrics (all reported as deltas vs the input where meaningful):
  env_crest_db     peak/mean of the short-time energy envelope. Smearing lowers it.
  gap_fill_db      level in the 50 ms before each onset relative to the onset peak.
                   Phase-vocoder "reverb" fills inter-hit gaps and raises this.
  onset_rate       onsets per second (should scale as 1/timeRatio; doubling/flams
                   raise it, deleted attacks lower it).
  stereo_corr      mean per-window Pearson correlation of L and R. Inter-channel
                   phase drift decorrelates the channels and lowers this.
  icpd_dev_rad     energy-weighted circular deviation of the inter-channel phase
                   difference per bin over time, minus the input's. Direct
                   "stereo phasing" measure.
  side_mid_db      overall side/mid energy ratio; image widening/narrowing.
  am_mod           amplitude-modulation depth of strong partials in sustain
                   (watery/warbly PV artifact).
  warble_cents     frequency instability of strong partials in sustain.
  hollow_flatness  spectral-flatness change in sustain frames (hollow/metallic).
  hf_ratio_db      energy above 6 kHz relative to input (dullness).
"""

import argparse
import json
import os
import sys

import numpy as np
import soundfile as sf

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def stft(x, n_fft=2048, hop=512):
    win = np.hanning(n_fft)
    n = (len(x) - n_fft) // hop + 1
    if n < 2:
        return np.zeros((n_fft // 2 + 1, 0), dtype=np.complex64)
    idx = np.arange(n_fft)[None, :] + hop * np.arange(n)[:, None]
    frames = x[idx] * win
    return np.fft.rfft(frames, axis=1).T.astype(np.complex64)


def envelope(x, win=256, hop=128):
    n = (len(x) - win) // hop + 1
    if n < 2:
        return np.zeros(1)
    idx = np.arange(win)[None, :] + hop * np.arange(n)[:, None]
    return np.sqrt(np.mean(x[idx] ** 2, axis=1) + 1e-20)


def onset_envelope_and_peaks(x, sr, n_fft=1024, hop=256):
    S = np.abs(stft(x, n_fft, hop))
    if S.shape[1] < 4:
        return np.zeros(1), np.array([], dtype=int), hop
    flux = np.sum(np.maximum(S[:, 1:] - S[:, :-1], 0.0), axis=0)
    flux = flux / (np.median(flux) + 1e-12)
    peaks = []
    thresh = 3.0
    last = -int(0.05 * sr / hop)
    for i in range(1, len(flux) - 1):
        if flux[i] > thresh and flux[i] >= flux[i - 1] and flux[i] >= flux[i + 1]:
            if i - last >= int(0.05 * sr / hop):
                peaks.append(i)
                last = i
    return flux, np.array(peaks, dtype=int), hop


def metrics_for(x, sr):
    """x: (n, ch) float array."""
    mono = x.mean(axis=1)
    out = {}

    env = envelope(mono)
    out["env_crest_db"] = float(20 * np.log10((env.max() + 1e-12) / (env.mean() + 1e-12)))

    flux, onsets, ohop = onset_envelope_and_peaks(mono, sr)
    dur = len(mono) / sr
    out["onset_rate"] = float(len(onsets) / max(dur, 1e-9))

    # Gap fill: envelope level 20-60 ms before each onset vs the onset peak.
    ehop = 128
    fills = []
    for o in onsets:
        t = o * ohop // ehop
        pre_lo = t - int(0.060 * sr / ehop)
        pre_hi = t - int(0.020 * sr / ehop)
        post_hi = t + int(0.030 * sr / ehop)
        if pre_lo < 0 or post_hi >= len(env):
            continue
        peak = env[t:post_hi].max() + 1e-12
        pre = np.median(env[pre_lo:pre_hi]) + 1e-12
        fills.append(20 * np.log10(pre / peak))
    out["gap_fill_db"] = float(np.median(fills)) if fills else None

    # Stereo metrics.
    if x.shape[1] >= 2:
        L, R = x[:, 0], x[:, 1]
        w = int(0.1 * sr)
        corrs = []
        for s in range(0, len(L) - w, w):
            l, r = L[s:s + w], R[s:s + w]
            el_, er = np.sqrt(np.sum(l * l)), np.sqrt(np.sum(r * r))
            if el_ > 1e-6 and er > 1e-6:
                corrs.append(np.sum(l * r) / (el_ * er))
        out["stereo_corr"] = float(np.mean(corrs)) if corrs else None

        SL, SR = stft(L), stft(R)
        if SL.shape[1] > 4:
            mag = np.abs(SL) * np.abs(SR)
            icpd = np.angle(SL * np.conj(SR))
            zsum = np.sum(mag * np.exp(1j * icpd), axis=1)
            msum = np.sum(mag, axis=1) + 1e-12
            rbar = np.abs(zsum) / msum                     # per-bin concentration
            wgt = msum / msum.sum()
            out["icpd_dev_rad"] = float(np.sum(wgt * np.sqrt(-2 * np.log(np.maximum(rbar, 1e-6)))))
        else:
            out["icpd_dev_rad"] = None

        mid, side = (L + R) * 0.5, (L - R) * 0.5
        out["side_mid_db"] = float(10 * np.log10((np.sum(side ** 2) + 1e-12) / (np.sum(mid ** 2) + 1e-12)))
    else:
        out["stereo_corr"] = None
        out["icpd_dev_rad"] = None
        out["side_mid_db"] = None

    # Sustain-region tonal stability: track the strongest peak per band per frame.
    S = stft(mono)
    n_fft, hop = 2048, 512
    if S.shape[1] > 8:
        mag = np.abs(S)
        frame_flux = np.sum(np.maximum(np.diff(mag, axis=1), 0), axis=0)
        sustain = np.where(frame_flux < np.median(frame_flux))[0] + 1
        bands = [(60, 400), (400, 1800), (1800, 6000)]
        warbles, ams = [], []
        for lo, hi in bands:
            klo, khi = int(lo * n_fft / sr), int(hi * n_fft / sr)
            if khi - klo < 4 or len(sustain) < 8:
                continue
            sub = mag[klo:khi, :]
            kpk = np.argmax(sub, axis=0)
            pkmag = sub[kpk, np.arange(sub.shape[1])]
            # Parabolic interpolation for sub-bin frequency.
            k = np.clip(kpk, 1, sub.shape[0] - 2)
            a = np.log(sub[k - 1, np.arange(sub.shape[1])] + 1e-12)
            b = np.log(sub[k, np.arange(sub.shape[1])] + 1e-12)
            c = np.log(sub[k + 1, np.arange(sub.shape[1])] + 1e-12)
            denom = a - 2 * b + c
            frac = np.where(np.abs(denom) > 1e-9, 0.5 * (a - c) / (denom + 1e-12), 0.0)
            freq = (klo + k + np.clip(frac, -0.5, 0.5)) * sr / n_fft
            fsel = freq[sustain]
            msel = pkmag[sustain]
            strong = msel > np.median(msel)
            if strong.sum() < 8:
                continue
            fs, ms = fsel[strong], msel[strong]
            med = np.median(fs)
            if med > 0:
                cents = 1200 * np.log2(np.maximum(fs, 1e-3) / med)
                warbles.append(np.std(np.clip(cents, -200, 200)))
            ams.append(np.std(ms) / (np.mean(ms) + 1e-12))
        out["warble_cents"] = float(np.median(warbles)) if warbles else None
        out["am_mod"] = float(np.median(ams)) if ams else None

        flat_frames = []
        for t in sustain[:400]:
            p = mag[8:, t] ** 2 + 1e-20
            flat_frames.append(np.exp(np.mean(np.log(p))) / np.mean(p))
        out["hollow_flatness"] = float(np.median(flat_frames)) if flat_frames else None
    else:
        out["warble_cents"] = None
        out["am_mod"] = None
        out["hollow_flatness"] = None

    spec = np.abs(np.fft.rfft(mono))
    freqs = np.fft.rfftfreq(len(mono), 1 / sr)
    hf = np.sum(spec[freqs > 6000] ** 2)
    total = np.sum(spec ** 2) + 1e-20
    out["hf_frac_db"] = float(10 * np.log10(hf / total + 1e-12))
    out["rms_db"] = float(20 * np.log10(np.sqrt(np.mean(mono ** 2)) + 1e-12))
    return out


def spectrogram_db(x, sr, n_fft=2048, hop=512):
    S = np.abs(stft(x, n_fft, hop)) + 1e-9
    return 20 * np.log10(S), sr / 2


def render(input_path, versions, out_png, zoom_onset=True):
    xin, sr = sf.read(input_path, always_2d=True)
    rows = [("input", xin)]
    for name, path in versions:
        xv, srv = sf.read(path, always_2d=True)
        rows.append((name, xv))

    n = len(rows)
    fig, axes = plt.subplots(n, 2, figsize=(16, 3.2 * n), constrained_layout=True)
    if n == 1:
        axes = axes[None, :]
    for i, (name, x) in enumerate(rows):
        mono = x.mean(axis=1)
        db, fmax = spectrogram_db(mono, sr)
        vmax = db.max()
        ax = axes[i, 0]
        ax.imshow(db, origin="lower", aspect="auto",
                  extent=[0, len(mono) / sr, 0, fmax / 1000],
                  vmin=vmax - 90, vmax=vmax, cmap="magma")
        ax.set_ylim(0, 16)
        ax.set_ylabel(f"{name}\nkHz")

        # Zoom: 300 ms around the strongest onset (transient smear inspection).
        flux, onsets, ohop = onset_envelope_and_peaks(mono, sr)
        ax2 = axes[i, 1]
        if len(onsets) > 0:
            strongest = onsets[np.argmax(flux[onsets])]
            c = strongest * ohop / sr
            lo, hi = max(0, c - 0.10), min(len(mono) / sr, c + 0.20)
            slo, shi = int(lo * sr), int(hi * sr)
            seg = mono[slo:shi]
            db2, fmax2 = spectrogram_db(seg, sr, n_fft=512, hop=64)
            if db2.shape[1] > 0:
                v2 = db2.max()
                ax2.imshow(db2, origin="lower", aspect="auto",
                           extent=[lo, hi, 0, fmax2 / 1000],
                           vmin=v2 - 80, vmax=v2, cmap="magma")
                ax2.set_ylim(0, 16)
        ax2.set_ylabel("onset zoom")
    axes[-1, 0].set_xlabel("s")
    axes[-1, 1].set_xlabel("s")
    fig.savefig(out_png, dpi=90)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True)
    ap.add_argument("--version", action="append", nargs=2, metavar=("NAME", "WAV"),
                    default=[], help="processed version to analyze")
    ap.add_argument("--time-ratio", type=float, default=1.0)
    ap.add_argument("--json-out", default=None)
    ap.add_argument("--png-out", default=None)
    args = ap.parse_args()

    xin, sr = sf.read(args.input, always_2d=True)
    result = {"input": metrics_for(xin, sr), "versions": {}, "time_ratio": args.time_ratio}

    for name, path in args.version:
        xv, srv = sf.read(path, always_2d=True)
        m = metrics_for(xv, srv)
        ref = result["input"]
        deltas = {}
        for key in ("env_crest_db", "gap_fill_db", "stereo_corr", "icpd_dev_rad",
                    "side_mid_db", "am_mod", "warble_cents", "hollow_flatness",
                    "hf_frac_db", "rms_db"):
            if m.get(key) is not None and ref.get(key) is not None:
                deltas["d_" + key] = m[key] - ref[key]
        if m.get("onset_rate") is not None and ref.get("onset_rate"):
            expected = ref["onset_rate"] / args.time_ratio
            deltas["onset_rate_err"] = (m["onset_rate"] - expected) / max(expected, 1e-9)
        m["deltas"] = deltas
        result["versions"][name] = m

    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump(result, f, indent=1)
    if args.png_out:
        render(args.input, args.version, args.png_out)

    for name, m in result["versions"].items():
        d = m["deltas"]
        parts = [f"{k}={v:+.2f}" for k, v in sorted(d.items()) if v is not None]
        print(f"{name}: " + " ".join(parts))


if __name__ == "__main__":
    main()
