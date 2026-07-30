# Architecture

Chronos changes the duration and pitch of audio independently. It is layered so
each piece is testable and replaceable on its own: the public API depends on
nothing but value types, the algorithms hide behind a strategy interface, and the
FFT and resampler are isolated so faster backends can drop in without touching call
sites.

```
include/chronos/           Public API (framework-free)
  Types.h                  Sample type, EngineType, Quality, StreamConfig, unit helpers
  StretchParameters.h      What to do: time / pitch / formant / engine / quality
  AudioBuffer.h            Non-owning planar view (RT path) + owning buffer (offline)
  TimeStretcher.h          The facade — pimpl, stable ABI surface, allocation-free hot path
  Chronos.h                Umbrella include

src/dsp/                   Dependency-free primitives
  Fft                      Self-contained radix-2 complex FFT + real-input helpers
  Window                   Hann / Hamming / Blackman-Harris, COLA-aware
  AudioFifo                Fixed-capacity multichannel ring, bridges block sizes
  SincResampler            Windowed-sinc polyphase, anti-aliased (the pitch stage)
  Resampler                Cubic-Hermite fractional resampler (cheaper fallback)
  MathUtils                Phase wrapping, dB, small numeric helpers

src/analysis/
  TransientDetector        Adaptive spectral-flux onset detection
  SpectralEnvelope         Cepstral-liftered envelope, for formant work

src/engine/
  IStretchEngine           Strategy interface: feed / available / read
  PhaseVocoderEngine       Flagship — stereo-coherent, transient-aware, formant-correcting
  WsolaEngine              Time-domain waveform-similarity overlap-add
  SinusoidalEngine         McAulay-Quatieri partial tracking + oscillator bank
```

---

## The central design decision

**Engines only stretch time.** Pitch is realised by the facade as *stretch by
`timeRatio × pitchRatio`, then resample by `pitchRatio`*:

```
input --> [engine: stretch by timeRatio*pitchRatio] --> FIFO --> [resampler: pitchRatio] --> output
```

This keeps every engine simple and uniform, and makes the two operations
independently testable. Formant correction is the exception that proves the rule:
it happens *inside* the engine, because it needs the spectrum, and the engine is
told the pitch ratio via `setPitchContext()` so it can pre-compensate for the
resampling that follows.

---

## The three engines

### PhaseVocoderEngine

The default, and where most of the quality work lives. A textbook phase vocoder
sounds watery and smeared; four refinements fix that.

**Identity phase locking** (Laroche & Dolson, 1999). Advancing every bin
independently lets bins belonging to one sinusoid drift apart — the classic
"phasiness". Instead, spectral peaks are located, only peaks advance by their true
frequency, and surrounding bins lock rigidly to their peak. Region boundaries fall
at spectral *valleys* rather than index midpoints, so the lock boundary sits where
there is least energy to mis-assign.

**Stereo-coherent phase advance.** Phase is propagated once per frame on a mono
(complex-sum) reference; each channel then re-applies its own analysis-phase offset
at synthesis:

```
phase_c[k] = S[k] + (aph_c[k] - aphMono[k])
```

so `out_L[k] - out_R[k] == in_L[k] - in_R[k]` on every frame, by construction.
Advancing channels independently — the textbook approach — lets tiny L/R
differences in the frequency estimates integrate into an unbounded drift, heard as
the stereo image swimming and combing on sustained material.

**Transient preservation.** The hard part of attacks is that every analysis frame
whose window merely *overlaps* a coming onset already contains it, and re-rendering
that frame spreads its energy across the frame's whole output span — backward in
time, as pre-echo. A phase reset on the onset frame cannot undo damage already
present in earlier frames. So instead:

1. onsets are found sample-accurately in the time domain (first-difference energy,
   64-sample attack window vs 768-sample context — differencing rejects the
   low-frequency wobble of a kick body that fools a plain energy ratio);
2. the coming attack is gated out of the frames that would smear it;
3. the hop schedule is re-timed so the attack lands at exactly its stretched
   position, with one debt invariant keeping the long-term ratio exact:
   `debtOut += hopIn * E - hopOut`;
4. the first few milliseconds are spliced in verbatim, because a window taper
   cannot carry a full-level attack at a frame edge.

**Noise-aware reconstruction.** Identity locking assumes every bin in a region
belongs to one sinusoid. Where that is false — hiss above the last real partial,
or regions whose "peak" is just the tallest noise sample — locking pins a bin to a
distant peak, giving a per-frame phase error of `(ω_k − ω_p)(Hs − Ha)`: hundreds of
radians. Successive frames then overlap-add incoherently and the band loses
`Σw⁴/(Σw²)²` of its power. Rather than pretend those bins are coherent, they are
decorrelated deliberately (a fresh rotation per 32-bin chunk, shared across
channels so the stereo image survives) and the now-known loss is added back with
a compensation gain of `m₂·sqrt(N/(S·m₄))`.

### WsolaEngine

Time-domain waveform-similarity overlap-add: cheap, phasiness-free by construction,
and the right choice for percussive material. Segments are placed by normalised
cross-correlation against a continuation template, searched coarse-to-fine. On a
detected attack the segment is instead placed so the onset lands at the window
*centre* — gain 1, neighbours ~0 — so the transient is copied verbatim rather than
softened under a taper.

Overlapping segments only add in amplitude when they actually match, and the
similarity search never matches exactly. The residual mismatch makes the overlap
add partly in *power*, which is why plain `1/Σw` normalisation leaves WSOLA about a
decibel quiet. The normaliser blends by the similarity actually achieved:
`1/sqrt(ρ(Σw)² + (1−ρ)Σw²)` — ρ=1 gives the coherent case, ρ=0 the incoherent one.

### SinusoidalEngine

McAulay-Quatieri partial tracking driving a quadratic-phase oscillator bank. Because
pitch is preserved, the oscillator increment equals the analysed frequency, so
stretching only changes how many samples are rendered — there is no overlap-add,
window, or normalisation in the sine path at all. Excellent on sparse polyphony.

It has **no noise residual**, so real recordings lose their air through it. `Auto`
gates it out except for near-pure tonal content.

---

## Content-adaptive `Auto`

`Auto` buffers the first half-second, measures three features, and latches one
engine for the stream:

| Feature | What it separates |
|---|---|
| `hprDb` | Harmonic vs percussive energy above 200 Hz, from median-filtering the probe spectrogram along time (harmonic) and frequency (percussive). |
| `fluxCrest` | Peak-to-mean spectral flux: sparse hard hits vs sustain. |
| `corr` | L/R correlation — wide or ambient material is not a dry kit. |

WSOLA is chosen only when all three agree (`hprDb < 4.5 && fluxCrest > 6 && corr > 0.9`);
anything else goes to the phase vocoder. Features are accumulated incrementally as
input arrives, so no analysis spike ever lands on the audio thread.

A single frame cannot do this job: a loop that opens on a kick looks like one
decaying sine, and a bright strummed chord looks percussive. Half a second is the
measured minimum that classifies reliably.

**The probe hold is real latency** and is reported by `latencySamples()`. For
sampler use, pick the engine explicitly instead.

---

## Real-time discipline

- All allocation is confined to `prepare()`. Even the phase-locking peak list is
  pre-reserved so `push_back` cannot allocate.
- The streaming path never allocates, locks, or throws.
- **Normalised weighted overlap-add**: a parallel window-energy accumulator makes
  reconstruction unity-gain for *any* synthesis hop, including a time-varying one —
  which is what makes both ratio automation and the transient hop schedule possible.
  Normalisation is Tikhonov-regularised (`norm/(norm² + ε²)`) so that where window
  coverage collapses the output fades to silence instead of amplifying whatever
  little is there.
- The FFT is ours on purpose: zero dependencies keeps the core embeddable, and it
  is isolated behind `dsp::Fft` so a vectorised backend (vDSP, pffft, FFTW) is a
  drop-in.

---

## Extending it

Adding an algorithm means implementing `IStretchEngine` (`prepare`, `reset`,
`setTimeStretch`, `setPitchContext`, `feed`, `available`, `read`, `latencySamples`,
`name`) and adding a case to `EngineType`. The facade's pitch, formant, smoothing,
and streaming plumbing comes for free.
