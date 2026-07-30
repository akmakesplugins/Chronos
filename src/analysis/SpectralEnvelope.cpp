// Copyright (c) 2026 ZROLogic. All rights reserved. Proprietary and confidential.
// Chronos — analysis/SpectralEnvelope.cpp

#include "SpectralEnvelope.h"

#include <cmath>

#include "../dsp/MathUtils.h"

namespace chronos::analysis
{

void SpectralEnvelope::prepare(int size, const dsp::Fft* sharedFft)
{
    fft      = sharedFft;
    fftSize  = size;
    halfBins = size / 2 + 1;
    work.assign(static_cast<size_t>(size), dsp::Complex{});
    envelope.assign(static_cast<size_t>(halfBins), 1.0f);

    // A sensible default cutoff: keeps broad formant structure, discards pitch.
    setLifterCutoff(size / 32);
}

void SpectralEnvelope::reset() noexcept
{
    for (auto& e : envelope)
        e = 1.0f;
}

void SpectralEnvelope::setLifterCutoff(int quefrencyBins) noexcept
{
    lifter = clamp(quefrencyBins, 2, fftSize / 4);
}

void SpectralEnvelope::compute(const dsp::Complex* spectrum) noexcept
{
    // 1) Log-magnitude spectrum, made conjugate-symmetric so the cepstrum is real.
    //    A small floor avoids log(0).
    for (int k = 0; k < fftSize; ++k)
    {
        const int mirror = (k <= fftSize / 2) ? k : (fftSize - k);
        const float mag = std::abs(spectrum[mirror]);
        work[static_cast<size_t>(k)] = dsp::Complex(std::log(mag + 1.0e-9f), 0.0f);
    }

    // 2) To the cepstrum (inverse FFT of the log spectrum).
    fft->inverse(work.data());

    // 3) Lifter: keep low quefrencies (the envelope), zero the rest (the pitch
    //    ripple and its harmonics). The cepstrum is symmetric, so we window both
    //    ends and double the interior — this is the standard real-cepstrum lifter.
    for (int q = 0; q < fftSize; ++q)
    {
        float gain;
        if (q == 0)
            gain = 1.0f;
        else if (q < lifter)
            gain = 2.0f;
        else if (q == fftSize / 2)
            gain = 1.0f;
        else
            gain = 0.0f;
        work[static_cast<size_t>(q)] *= gain;
    }

    // 4) Back to a (log) spectrum and exponentiate to linear magnitude.
    fft->forward(work.data());
    for (int k = 0; k < halfBins; ++k)
        envelope[static_cast<size_t>(k)] = std::exp(work[static_cast<size_t>(k)].real());
}

float SpectralEnvelope::envelopeAt(float bin) const noexcept
{
    if (bin <= 0.0f)
        return envelope[0];
    if (bin >= static_cast<float>(halfBins - 1))
        return envelope[static_cast<size_t>(halfBins - 1)];

    const int i = static_cast<int>(bin);
    const float t = bin - static_cast<float>(i);
    return dsp::lerp(envelope[static_cast<size_t>(i)], envelope[static_cast<size_t>(i + 1)], t);
}

} // namespace chronos::analysis
