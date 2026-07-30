// Copyright (c) 2026 ZROLogic. All rights reserved. Proprietary and confidential.
// Chronos — dsp/Fft.h
//
// A self-contained radix-2 complex FFT plus a real-signal convenience wrapper.
//
// Why ship our own instead of depending on FFTW/pffft/vDSP?
//   * Zero dependencies keeps the core embeddable and portable.
//   * The size is always a power of two (our STFT windows), which the simple
//     iterative radix-2 handles optimally.
//   * It is isolated behind this small interface, so a platform-optimised backend
//     can replace the internals without touching a single call site. That seam is
//     the whole point — correctness now, speed later, no API churn.
//
// Real-time note: prepare()/setSize() allocate the twiddle and scratch tables.
// forward()/inverse() are allocation-free and safe on the audio thread.

#pragma once

#include <memory>
#include <vector>

#include "MathUtils.h"

namespace chronos::dsp
{

class Fft
{
public:
    Fft() = default;
    explicit Fft(int size) { setSize(size); }

    /// Configure for a transform of `size` points. `size` must be a power of two.
    /// Precomputes bit-reversal indices and twiddle factors. Allocates — call at
    /// setup, never on the audio thread. `withRealSupport` also builds a half-size
    /// child transform so forwardReal()/inverseReal() can use the fast real-FFT
    /// path (a real signal only needs an N/2-point complex FFT); it is disabled for
    /// the child itself to stop the recursion.
    void setSize(int size, bool withRealSupport = true);

    [[nodiscard]] int size() const noexcept { return fftSize; }

    /// In-place forward transform (time -> frequency). `data` holds `size()`
    /// interleaved complex samples. No normalisation is applied (matching the
    /// usual convention: the 1/N scaling lives in inverse()).
    void forward(Complex* data) const noexcept;

    /// In-place inverse transform (frequency -> time), scaled by 1/N so that
    /// inverse(forward(x)) == x.
    void inverse(Complex* data) const noexcept;

    /// Convenience: transform a real, windowed frame into a complex spectrum.
    /// `spectrum` must have room for size() bins (we return the full spectrum;
    /// the caller may use only the lower half + Nyquist for a real signal).
    void forwardReal(const Sample* realInput, Complex* spectrum) const noexcept;

    /// Convenience inverse for a spectrum known to be conjugate-symmetric: writes
    /// size() real samples, discarding the (numerically ~0) imaginary part.
    void inverseReal(const Complex* spectrum, Sample* realOutput) const noexcept;

private:
    void transform(Complex* data, bool inverse) const noexcept;

    int fftSize = 0;
    int numBits = 0;
    std::vector<int>     bitReversal;   ///< Precomputed bit-reversed indices.
    std::vector<Complex> twiddles;      ///< Forward twiddle factors per stage.
    mutable std::vector<Complex> scratch; ///< Reused by the real-signal helpers.

    // Fast real-FFT support: a real transform of size N is computed as an N/2-point
    // complex FFT plus an unpack step, halving the work. `half` is the N/2 child;
    // `unpackTwiddles[k] = exp(-2*pi*i*k/N)` for k in [0, N/2].
    std::unique_ptr<Fft> half;
    std::vector<Complex> unpackTwiddles;
    mutable std::vector<Complex> halfScratch; ///< N/2 packed buffer for real paths.
};

} // namespace chronos::dsp
