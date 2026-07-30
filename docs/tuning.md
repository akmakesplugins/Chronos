# Tuning reference

Every tunable constant in the engine, what it does, and what happens if you move
it. Values are the shipped defaults. Most were set from measurement — the figures
quoted are from the corpus in [quality.md](quality.md).

---

## Quality tiers

`Quality` selects frame size and overlap. Larger frames give better frequency
resolution and worse time resolution — the fundamental trade.

**PhaseVocoderEngine** (`specFor`, `PhaseVocoderEngine.cpp`):

| Quality | Frame | Overlap | Phase locking |
|---|---|---|---|
| `Draft` | 1024 | 4 | off (classic per-bin) |
| `Good` | 2048 | 4 | on |
| `High` | 2048 | 8 | on |
| `Best` | 4096 | 8 | on |

**SinusoidalEngine** additionally scales its track budget: 48 / 96 / 128 / 192.

**WsolaEngine deliberately does not grow** with quality — 1024 samples at Good and
above (512 at Draft). A larger segment would smear the transients WSOLA exists to
protect, and the similarity search is O(frameSize²).

`maxTimeStretch` for the phase vocoder is `overlapFactor × 0.95`: the synthesis hop
must stay below the frame size or frames stop overlapping. At `Best` that allows
~7.6×.

---

## Transient preservation

`PhaseVocoderEngine.h`:

| Constant | Default | Effect |
|---|---|---|
| `kAttackWin` | 64 samples (~1.5 ms) | Energy window for the attack side of the detector. Shorter = twitchier. |
| `kContextWin` | 768 samples (~17 ms) | The "before" context. Must be long enough to average over a kick cycle. |
| `kOnsetRatio` | 5.0 | Attack/context first-difference energy step needed to fire. Lower = more onsets, more verbatim splices on tonal material. |
| `refractorySamples` | 40 ms | Minimum gap between onsets. Rolls and flams faster than this are handled by the first onset only. |
| `unityFrames` | 12 ms worth | Frames held at unity rate through the attack. Longer = sharper attack, more time-debt to repay. |
| `splicePreRoll` | 3 ms | Verbatim audio captured *before* the onset, giving the crossfade somewhere to start. |
| `spliceLen` | preRoll + 25 ms | Total verbatim insert. |
| `spliceHold` | preRoll + 12 ms | How long the splice plays at full weight before crossfading back. |
| `spliceFadeIn` | 1.5 ms | Fade onto the splice. |
| `kNormEpsSq` | 2.5e-3 | Tikhonov term in the OLA normaliser. Larger = safer around gated regions, slightly more level loss. |
| gate compensation cap | `4/normFull` (~12 dB) | Bounds how far a gated region may be boosted. **Raising this is tempting and wrong** — at 16 the corpus peak went to 1.95 FS for 0.7 dB of HF. |

The detector uses **first-difference** energy on purpose. A plain short/long energy
ratio fires on the low-frequency cycle wobble of a kick body; differencing rejects
it. On one corpus loop that changed false onsets every ~1400 samples into true hits
every ~10400.

**Time-debt bookkeeping.** All hop scheduling funnels through one invariant:

```cpp
debtOut += hopIn * E - hopOut;
```

The long-term ratio is exact whenever the debt returns to zero. Pre-payment by the
gated approach frames is what makes an attack land at exactly `E × p`. If you add a
scheduling case, keep it behind this invariant.

---

## Noise-bin handling

`PhaseVocoderEngine.h` — the HF-dullness cure:

| Constant | Default | Effect |
|---|---|---|
| `kRandChunk` | 32 bins | Bins sharing one random rotation. A constant rotation across a narrow band preserves that band's temporal envelope, keeping hits sharp. Smaller = more decorrelation but softer transients. |
| `kMainLobeBins` | 6 | Distance from a peak beyond which a bin is not part of its main lobe. |
| `kPeakProminence` | 4.0 | A peak must exceed 4× its region's off-lobe mean to count as a real partial. Below that the whole region is treated as noise. |
| `kHannM2`, `kHannM4` | 0.375, 35/128 | Hann window moments. Recompute if you change the window: `m2 = mean(wa·ws)`, `m4 = mean((wa·ws)²)`. |
| compensation cap | 1.0 … 6.0 | Only binds below ~0.11× ratio. |

The gain is `m₂·sqrt(N/(S·m₄))` — the exact inverse of the `Σw⁴/(Σw²)²` power loss
that fully incoherent overlap-add produces. It predicted the measured loss to 0.1 dB
at two different ratios, which is why it is applied as a closed form rather than
fitted.

Randomisation is judged by the **local** hop ratio (`hopOut/hopIn`), not the global
`timeStretch`. The transient schedule runs unity-rate frames where the overlap-add
genuinely *is* coherent; decorrelating those throws away the attack sharpness the
schedule just bought.

---

## Peak locking

`locatePeaks()`:

- Peak threshold: `maxMag × 1e-3` (−60 dB relative floor).
- A bin must dominate **two** neighbours on each side. A lax 3-point test admits
  the interference ripples that beat between two real tones; once locked, those
  inject phase at a garbage frequency.
- Region boundaries fall at the spectral **valley** between adjacent peaks, not the
  index midpoint, so the rigid lock boundary sits where there is least energy.

---

## WSOLA

`WsolaEngine`:

| Constant | Default | Effect |
|---|---|---|
| `synthHop` | frameSize/2 | 50% overlap. |
| `tolerance` | frameSize/4 | Similarity search range. Wider = better matches, more CPU. |
| search `stride` | 2 | Coarse-to-fine: strided pass then ±stride refinement. |
| onset ratio | 3.0 | Energy jump to treat a position as an attack and centre it in the window. |
| onset window | 64 samples | Energy window for that test. |
| `minStretch` / `maxStretch` | 0.25 / 4.0 | Range. |

The normaliser blends amplitude and power by the **achieved** similarity ρ:
`1/sqrt(ρ(Σw)² + (1−ρ)Σw²)`. ρ comes from `findBestOffset` normalised by the
template energy. ρ defaults to 1 (plain amplitude normalisation) when no search ran,
so onset-locked segments keep the old behaviour.

---

## The `Auto` router

`TimeStretcher.cpp`:

| Constant | Default | Effect |
|---|---|---|
| `kProbeSeconds` | 0.5 | Audible input analysed before latching. **This is real added latency.** 0.25 s misclassifies bright melodic material as percussive; 0.35 s misses fast breaks. |
| `kPendingSeconds` | 1.0 | Hard cap on held input, so a near-silent stream cannot buffer forever. |
| `kMinProbeSeconds` | 0.25 | Below this, fall back to the phase vocoder rather than guess. |
| `kProbeFft` / `kMedian` | 1024 / 17 | Probe STFT size and median filter length (~99 ms in time, ~730 Hz in frequency). |
| silence floor | −60 dBFS | Leading silence is held but not analysed, so a loop starting with a gap still gets a full half-second of real audio. |
| WSOLA gate | `hprDb < 4.5 && fluxCrest > 6.0 && corr > 0.9` | All three must agree. On the corpus, drums sat at hprDb ≤ 3.39 against tonal ≥ 3.58, and drum L/R correlation ≥ 0.957 against wide material ≤ 0.86. |
| Sinusoidal gate | `hprDb > 20 && fluxCrest < 3` | Deliberately almost unreachable for real recordings — the engine has no noise residual, so real material loses its air through it. |

Thresholds were tuned on one 44.1 kHz EDM pack and validated on ~40 further files
from it with zero false WSOLA routes. **Validate against acoustic and live material
before relying on them.** Failures are conservative — anything uncertain falls
through to the phase vocoder — as long as the three-way AND is kept.

---

## Things deliberately *not* configurable

- **Allocation points.** `prepare()` allocates; nothing else does. Adding a runtime
  resize would break the real-time contract.
- **The 4/normFull compensation cap** — see above; measured, not guessed.
- **The debt invariant** — exactness of the long-term stretch ratio depends on it.
