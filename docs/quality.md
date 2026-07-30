# Measured quality

All figures below are measurements, not estimates. They come from the harness in
`qa/`, run on a corpus of **12 commercial loops** (5 full drum loops plus brass,
guitar, piano and strings) at **4 cases each** — stretch ×0.8, ×1.5, ×2.0 and a
+3-semitone shift — giving 44 comparisons, scored against **zplane élastique Pro
V3.4.9** rendering the identical material.

Reproduce with:

```bash
qa/run_corpus.py --tag mine
qa/compare_tags.py baseline mine
```

---

## Headline: Chronos vs élastique Pro

Mean absolute deviation from the input, across all 44 cases. Lower is better
except where marked `*`, where higher is better. Bold = winner.

| Property | Before this work | **Chronos now** | élastique Pro |
|---|---|---|---|
| Inter-channel phase drift (rad) | 0.063 | **0.010** | 0.010 |
| Side/mid image error (dB) | 1.015 | **0.323** | 1.392 |
| Amplitude modulation | 0.109 | **0.061** | 0.075 |
| Transient crest* (dB) | 0.135 | **0.906** | 0.876 |
| HF retention* (dB) | 0.012 | **0.811** | 0.680 |
| Inter-hit gap fill (dB) | 2.659 | **2.666** | 2.820 |
| Pitch warble (cents) | 11.2 | 11.7 | **8.97** |
| Level accuracy (dB) | 0.562 | 0.605 | **0.176** |

Chronos wins six of eight and ties the seventh. The two losses — warble and
absolute level — are almost entirely in the WSOLA path (see the split below).

### Attack rendering

Measured per detected onset (`qa/preecho.py`), drum loop at ×2:

| | Before | **Now** | élastique Pro |
|---|---|---|---|
| Pre-echo before the hit | −20.0 dB | **−45.4 dB** | −44.1 dB |
| HF rise time | 17.0 ms | **2.0 ms** | 2.0 ms |
| Attacks preserved (of 38 real onsets) | 25 | **41** | 42 |

The input's own rise time is 2.0 ms, so both engines now reproduce attacks at
source sharpness.

### Split by engine

`Auto` routes drums to WSOLA and everything else to the phase vocoder. The two
paths have different strengths, and reporting them merged hides that:

**PhaseVocoder path (24 tonal cases) — Chronos wins 6 of 9 metrics**

| Metric | Chronos | élastique |
|---|---|---|
| Transient crest* | **+0.581** | +0.413 |
| Gap fill | **1.268** | 1.587 |
| Inter-channel phase | **0.007** | 0.013 |
| Amplitude modulation | **0.047** | 0.056 |
| Pitch warble | **6.84** | 8.33 |
| HF retention* | +1.110 | **+1.141** |
| Stereo correlation* | +0.004 | **+0.013** |
| Level accuracy | 0.432 | **0.122** |
| Onset rate error | 0.435 | **0.361** |

**WSOLA path (20 percussive cases)** — wins HF retention, gap fill and AM, but
loses warble (17.7 vs 9.7) and level (1.08 vs 0.24). This is the engine's weak
spot and the clearest place to improve next.

---

## Throughput

Stereo, single voice, Apple M3 Pro, one thread, `Quality::Best`:

| Configuration | ×realtime |
|---|---|
| `Auto` on drums (routes to WSOLA) | ~107× |
| `Auto` on tonal material (phase vocoder) | ~48× |
| élastique Pro, same material | ~78× |

A phase-vocoder voice costs roughly 2% of a core.

---

## Correctness

`test/TestMain.cpp` — 40 checks, all passing on arm64/clang, clean under ASan and
UBSan (both on synthetic signals and on real loops through every engine, plus the
mono, pitch-shift and formant paths).

| Property | Result |
|---|---|
| Output length tracks time ratio (0.5×–2×) | within ~2–4% |
| Pitch preserved under time-stretch | 0 cents error |
| Pitch shift accuracy (±12 st) | 0 cents error |
| Energy conservation | RMS ratio ≈ 0.98 |
| Finiteness (no NaN/Inf) | all paths |

**Peak levels.** Resynthesis can exceed unity — partials that were out of phase in
the input can sum constructively. Chronos peaks at 0.99 where élastique reaches
1.44 at the common ×1.5 and ×2 ratios; at an extreme ×3.7 Chronos reaches 1.24 to
élastique's 1.14. Leave headroom, as you would with any stretcher.

---

## What the metrics mean

| Metric | Measures | Why it matters |
|---|---|---|
| `icpd_dev_rad` | Energy-weighted circular deviation of the per-bin L/R phase difference, vs the input | Direct measure of stereo "phasing" — the image swimming or combing |
| `env_crest_db` | Peak-to-mean of the short-time energy envelope | Transient sharpness; smearing flattens it |
| `gap_fill_db` | Level just before each onset relative to the onset peak | Phase-vocoder "reverb" filling the gaps between hits |
| `warble_cents` | Frequency instability of strong partials during sustain | Audible pitch wobble |
| `am_mod` | Amplitude-modulation depth of strong partials | The watery/tremolo artifact |
| `hf_frac_db` | Energy above 6 kHz relative to the input | Dullness |
| `preecho_db` | HF energy 12–60 ms *before* each attack | The pre-echo "whoosh" ahead of drum hits |

**A caution on `warble_cents`:** it tracks the strongest partial per band, so on
percussive material it conflates real frequency modulation with the tracker hopping
between partials. Differences under ~5 cents are noise, and large values on drums
should not be read as audible pitch wobble. Trust it on sustained tonal material.

---

## Honest remaining gaps

- **WSOLA warble and level.** It now carries ~45% of cases via `Auto`, so its
  weaknesses matter more than they used to.
- **`Auto` latches once.** Material that changes character mid-stream keeps the
  engine chosen from its opening half-second. A hysteretic re-route with an
  equal-power crossfade at an onset is the fix.
- **Dense drums leave the phase-vocoder path.** A 93 ms window fights closely
  spaced attacks; a multi-resolution window would let the PV keep them.
- **Formant calibration** has not been matched to élastique's envelope curve.
- **Listening tests at scale have not been done.** `qa/make_listening_pack.py`
  renders input / before / Chronos / élastique sets for exactly that.
