# Changelog

All notable changes to Chronos.

## 1.0.0 — 2026-07-29

First packaged release. The engine reaches or beats zplane élastique Pro on six of
eight objective measures across a 44-case commercial-loop corpus; see
[docs/quality.md](docs/quality.md) for the full table and the honest gaps.

### Added
- **Stereo-coherent phase advance** in the phase vocoder. Phase is propagated once
  on a mono complex-sum reference and each channel re-applies its own analysis-phase
  offset, so the output's inter-channel phase difference equals the input's on every
  frame. Fixes the image swimming/combing on sustained material.
  Inter-channel drift 0.063 → 0.010 rad (élastique: 0.010).
- **Transient preservation**: sample-accurate time-domain onset detection, a
  pre-echo gate, an onset-aligned hop schedule with an exact time-debt invariant,
  and verbatim attack splicing. Pre-echo −20.0 → −45.4 dB (élastique: −44.1);
  HF rise time 17.0 → 2.0 ms, matching the source.
- **Noise-aware reconstruction**: bins that identity locking would decorrelate are
  decorrelated deliberately and power-compensated by the closed-form incoherent-WOLA
  deficit. HF retention 0.012 → 0.811 dB (élastique: 0.680).
- **Content-adaptive `Auto`** rebuilt around a 0.5 s probe using harmonic/percussive
  ratio, flux crest and L/R correlation, accumulated incrementally so no analysis
  spike hits the audio thread. Routing is now correct on all 12 corpus loops; the
  previous single-frame probe was inverted, sending every drum loop to the phase
  vocoder and piano/guitar chords to WSOLA.
- **Coherence-blended WSOLA normaliser** — `1/sqrt(ρ(Σw)² + (1−ρ)Σw²)` using the
  achieved similarity, recovering ~0.3–0.5 dB of level.
- **JUCE adapters**: `ChronosJuceStretcher` (live signal) and `ChronosSampleSource`
  (sample playback with loop/ping-pong/reverse/crossfade; drop-in for an
  élastique-based source).
- **QA harness** (`qa/`): render CLIs, objective metrics, spectrograms, corpus
  scoreboards and A/B listening-pack generation.
- Documentation: architecture, JUCE integration (with a full Sonic Atlas call map),
  measured quality, and a tuning reference for every constant.

### Fixed
- Auto's probe hold overflowed the bridge FIFO and **silently dropped audio**
  (output ran ~9% short). Held input is now replayed in order as the caller consumes
  output, and is never discarded.
- Noise-bin randomisation judged coherence by the global ratio rather than the
  frame's local hop ratio, decorrelating unity-rate frames that are genuinely
  coherent.
- WAV reader rejected files whose `data` chunk declares more bytes than the file
  holds; truncated exports are now read up to what is present.

### Removed
- Dead `RTPGHI` declarations (`RtpghiNode`, `integrateRtpghi`, `rtpghiEnabled`) that
  were never implemented.

### Known limitations
- Pitch warble (11.7 vs 8.97 cents) and absolute level (0.605 vs 0.176 dB) still
  favour élastique; both are concentrated in the WSOLA path.
- `Auto` latches once per stream and does not re-route on changing material.
- Formant calibration is not matched to élastique's envelope curve (the *off* state
  is identical; non-zero settings are Chronos's own).
- Listening tests at scale have not been carried out.
