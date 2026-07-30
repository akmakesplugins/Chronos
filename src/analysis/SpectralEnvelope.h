// Chronos — analysis/SpectralEnvelope.h
//
// Cepstral spectral-envelope estimator. The envelope is the slowly-varying shape
// of the magnitude spectrum — it carries formants (vocal tract / body resonances)
// while the fine ripple carries pitch. Separating the two lets us pitch-shift the
// fine structure while keeping formants put ("formant preservation"), or move the
// formants independently ("formant shift").
//
// Method: take log|X|, inverse-FFT to the cepstrum, keep only the low-quefrency
// coefficients (liftering) — those describe the smooth envelope — then FFT back
// and exponentiate. The lifter cutoff trades envelope detail against smoothness.

#pragma once

#include <vector>

#include "../dsp/Fft.h"

namespace chronos::analysis
{

class SpectralEnvelope
{
public:
    SpectralEnvelope() = default;

    /// `fft` must outlive this object and be sized to `fftSize`. We borrow it
    /// rather than owning a second FFT of the same size.
    void prepare(int fftSize, const dsp::Fft* fft);

    void reset() noexcept;

    /// Lifter cutoff in cepstral bins. Smaller = smoother envelope (fewer, broader
    /// formants); larger = follows finer detail. Typical: fftSize/64 .. fftSize/16.
    void setLifterCutoff(int quefrencyBins) noexcept;

    /// Estimate the envelope from a full complex spectrum (only magnitudes used).
    void compute(const dsp::Complex* spectrum) noexcept;

    /// Linear magnitude of the envelope at a (possibly fractional) FFT bin, with
    /// clamping at the ends. Used to look up env(pitch * bin) for formant work.
    [[nodiscard]] float envelopeAt(float bin) const noexcept;

    [[nodiscard]] const float* magnitudes() const noexcept { return envelope.data(); }
    [[nodiscard]] int numBins() const noexcept { return halfBins; }

private:
    const dsp::Fft* fft = nullptr;
    int fftSize  = 0;
    int halfBins = 0;    ///< fftSize/2 + 1.
    int lifter   = 0;

    std::vector<dsp::Complex> work;      ///< Scratch for the cepstral transforms.
    std::vector<float>        envelope;  ///< Linear envelope magnitude per bin.
};

} // namespace chronos::analysis
