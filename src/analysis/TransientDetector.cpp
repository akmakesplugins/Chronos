// Copyright (c) 2026 ZROLogic. All rights reserved. Proprietary and confidential.
// Chronos — analysis/TransientDetector.cpp

#include "TransientDetector.h"

#include <cmath>

namespace chronos::analysis
{

void TransientDetector::prepare(int numBins, double sampleRate, int hopSize)
{
    previousMags.assign(static_cast<size_t>(numBins), Sample{0});

    // Refractory period ~30 ms: long enough that one attack fires once, short
    // enough to catch fast drum rolls.
    const double framesPerSecond = sampleRate / static_cast<double>(hopSize);
    refractoryFrames = static_cast<int>(std::round(0.030 * framesPerSecond));
    if (refractoryFrames < 1)
        refractoryFrames = 1;

    reset();
}

void TransientDetector::reset() noexcept
{
    for (auto& m : previousMags)
        m = Sample{0};
    runningMean = 0.0f;
    currentFlux = 0.0f;
    refractory  = 0;
    seededMean  = false;
}

bool TransientDetector::process(const Sample* magnitudes, int numBins) noexcept
{
    // Half-wave-rectified spectral flux: only rising bins count, so a decaying
    // note is not mistaken for an onset. We also sum the total magnitude so the
    // flux can be judged relative to the frame's energy (see the broadband gate).
    float flux = 0.0f;
    float total = 0.0f;
    for (int k = 0; k < numBins; ++k)
    {
        const float diff = magnitudes[k] - previousMags[static_cast<size_t>(k)];
        if (diff > 0.0f)
            flux += diff;
        total += magnitudes[k];
        previousMags[static_cast<size_t>(k)] = magnitudes[k];
    }
    currentFlux = flux;

    // Adaptive threshold: flux must exceed the recent average by a margin that the
    // sensitivity control tightens or loosens. margin in ~[1.15 .. 3.0].
    const float margin = 3.0f - 1.85f * sensitivity;

    // Broadband-onset gate. A real onset dumps energy across many bins at once, so
    // its flux is a large fraction of the frame's total magnitude. A steady tone
    // never does that — but the spectral-leakage image of the sinusoid beats against
    // the analysis hop, wobbling the magnitudes a little every frame. That wobble is
    // a minuscule fraction of the total (<1% for a pure tone, ~10% for a rich chord)
    // yet it can still spike above the adaptive running-mean threshold and fire a
    // spurious onset. On tonal material each false onset triggers a full phase reset
    // (sph = aph), which is nearly harmless at light synthesis overlap but injects
    // strong sidebands once frames overlap heavily (small synthesis hop, i.e. time
    // compression). Requiring the flux to be a meaningful fraction of the spectrum
    // rejects the wobble while still passing genuine transients (whose fraction is
    // far higher). 0.20 sits comfortably between the two populations.
    constexpr float kOnsetFluxFraction = 0.20f;
    const float onsetFloor = kOnsetFluxFraction * total;

    bool isOnset = false;
    if (refractory > 0)
    {
        --refractory;
    }
    else if (seededMean && flux > runningMean * margin && flux > onsetFloor && flux > 1.0e-6f)
    {
        isOnset = true;
        refractory = refractoryFrames;
    }

    // Update the running average (one-pole). We seed it on the first frame so the
    // very first attack after a reset is not swallowed by a zero baseline.
    constexpr float smoothing = 0.90f;
    if (!seededMean)
    {
        runningMean = flux;
        seededMean = true;
    }
    else
    {
        runningMean = smoothing * runningMean + (1.0f - smoothing) * flux;
    }

    return isOnset;
}

} // namespace chronos::analysis
