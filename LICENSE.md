# License

Copyright © 2026 Pitch Innovations. All rights reserved.

Chronos is proprietary, internal software of Pitch Innovations. It may not be
copied, distributed, sublicensed, or disclosed outside Pitch Innovations without
prior written permission.

## Third-party code

None. The engine is dependency-free C++17 — the FFT, windows, resamplers, and
analysis code are all first-party. The algorithms implemented are standard,
well-documented DSP from the published literature (the phase vocoder;
Laroche & Dolson identity phase locking; WSOLA; McAulay-Quatieri sinusoidal
modelling; Fitzgerald median-filter harmonic/percussive separation; cepstral
spectral envelopes). No third-party source is vendored.

The optional JUCE adapters in `adapters/juce/` include JUCE headers and are subject
to your own JUCE license. They are not required to build or use the core library.

The QA harness in `qa/` uses NumPy, SciPy, soundfile and Matplotlib at development
time only; none of them are build or runtime dependencies of the engine.
