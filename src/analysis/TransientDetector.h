// Copyright (c) 2026 ZROLogic. All rights reserved. Proprietary and confidential.
// Chronos — analysis/TransientDetector.h
//
// Spectral-flux onset detector. It consumes the magnitude spectra that the phase
// vocoder already computes (so there is no second FFT) and flags frames that
// contain a transient onset. The phase vocoder reacts by resetting phases across
// that frame, which keeps attacks crisp instead of smearing them — the single
// biggest audible win for drums, plucked strings, and speech consonants.
//
// The measure is half-wave-rectified spectral flux (energy that *appeared* since
// the previous frame), compared against an adaptive threshold derived from a
// running average of recent flux. A refractory period prevents a single attack
// from firing on several consecutive frames.

#pragma once

#include <vector>

#include "chronos/Types.h"

namespace chronos::analysis
{

class TransientDetector
{
public:
    TransientDetector() = default;

    void prepare(int numBins, double sampleRate, int hopSize);
    void reset() noexcept;

    /// 0 = only the sharpest attacks fire; 1 = very sensitive. Maps to how far the
    /// flux must exceed its running average.
    void setSensitivity(float s) noexcept { sensitivity = clamp(s, 0.0f, 1.0f); }

    /// Feed one frame's magnitude spectrum (numBins = fftSize/2 + 1). Returns true
    /// if this frame is judged to be a transient onset.
    [[nodiscard]] bool process(const Sample* magnitudes, int numBins) noexcept;

    /// The flux value from the most recent process() call (diagnostics / metering).
    [[nodiscard]] float lastFlux() const noexcept { return currentFlux; }

private:
    std::vector<Sample> previousMags;
    float runningMean = 0.0f;   ///< One-pole average of recent flux.
    float currentFlux = 0.0f;
    float sensitivity = 0.5f;
    int   refractory  = 0;      ///< Frames remaining before we may fire again.
    int   refractoryFrames = 0; ///< Minimum spacing between onsets (in frames).
    bool  seededMean = false;
};

} // namespace chronos::analysis
