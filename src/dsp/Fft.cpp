// Copyright (c) 2026 ZROLogic. All rights reserved. Proprietary and confidential.
// Chronos — dsp/Fft.cpp

#include "Fft.h"

#include <cassert>

namespace chronos::dsp
{

void Fft::setSize(int size, bool withRealSupport)
{
    assert(isPowerOfTwo(size) && "Fft size must be a power of two");

    fftSize = size;
    numBits = log2Int(size);

    // --- Bit-reversal permutation table --------------------------------------
    // A decimation-in-time FFT reads its input in bit-reversed order. Precompute
    // the permutation once so the hot path is a simple index swap.
    bitReversal.resize(static_cast<size_t>(size));
    for (int i = 0; i < size; ++i)
    {
        int reversed = 0;
        for (int bit = 0; bit < numBits; ++bit)
            if (i & (1 << bit))
                reversed |= 1 << (numBits - 1 - bit);
        bitReversal[static_cast<size_t>(i)] = reversed;
    }

    // --- Twiddle factors ------------------------------------------------------
    // W_N^k = exp(-2*pi*i*k/N) for k in [0, N/2). Stored for the forward
    // transform; the inverse conjugates on the fly.
    twiddles.resize(static_cast<size_t>(size / 2));
    for (int k = 0; k < size / 2; ++k)
    {
        const double angle = -kTwoPi * k / size;
        twiddles[static_cast<size_t>(k)] =
            Complex(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
    }

    scratch.assign(static_cast<size_t>(size), Complex{});

    // Real-FFT machinery: an N/2 child transform plus the unpack twiddles.
    if (withRealSupport && size >= 2)
    {
        const int m = size / 2;
        half = std::make_unique<Fft>();
        half->setSize(m, /*withRealSupport*/ false);
        halfScratch.assign(static_cast<size_t>(m), Complex{});
        unpackTwiddles.resize(static_cast<size_t>(m + 1));
        for (int k = 0; k <= m; ++k)
        {
            const double angle = -kTwoPi * k / size;
            unpackTwiddles[static_cast<size_t>(k)] =
                Complex(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
        }
    }
    else
    {
        half.reset();
        unpackTwiddles.clear();
        halfScratch.clear();
    }
}

void Fft::transform(Complex* data, bool inverse) const noexcept
{
    // 1) Reorder into bit-reversed positions (in place via swaps).
    for (int i = 0; i < fftSize; ++i)
    {
        const int j = bitReversal[static_cast<size_t>(i)];
        if (j > i)
            std::swap(data[i], data[j]);
    }

    // 2) Iterative Cooley–Tukey butterflies, stage by stage. `halfSize` doubles
    //    each stage (1, 2, 4, ... N/2); the twiddle stride shrinks accordingly.
    for (int halfSize = 1; halfSize < fftSize; halfSize <<= 1)
    {
        const int fullSize = halfSize << 1;
        const int twiddleStep = (fftSize / fullSize);

        for (int start = 0; start < fftSize; start += fullSize)
        {
            for (int k = 0; k < halfSize; ++k)
            {
                Complex w = twiddles[static_cast<size_t>(k * twiddleStep)];
                if (inverse)
                    w = std::conj(w);

                const Complex even = data[start + k];
                const Complex odd  = w * data[start + k + halfSize];

                data[start + k]            = even + odd;
                data[start + k + halfSize] = even - odd;
            }
        }
    }

    // 3) Inverse transform is scaled by 1/N to invert forward() exactly.
    if (inverse)
    {
        const float norm = 1.0f / static_cast<float>(fftSize);
        for (int i = 0; i < fftSize; ++i)
            data[i] *= norm;
    }
}

void Fft::forward(Complex* data) const noexcept { transform(data, false); }
void Fft::inverse(Complex* data) const noexcept { transform(data, true); }

void Fft::forwardReal(const Sample* realInput, Complex* spectrum) const noexcept
{
    // Fall back to the plain complex transform if the real path is unavailable.
    if (!half)
    {
        for (int i = 0; i < fftSize; ++i)
            spectrum[i] = Complex(realInput[i], 0.0f);
        transform(spectrum, false);
        return;
    }

    const int m = fftSize / 2;

    // Pack the N real samples into N/2 complex (even -> real, odd -> imag) and take
    // one N/2-point complex FFT.
    Complex* z = halfScratch.data();
    for (int n = 0; n < m; ++n)
        z[n] = Complex(realInput[2 * n], realInput[2 * n + 1]);
    half->transform(z, false);

    // Unpack into the real spectrum bins 0..N/2. Callers only read this half; the
    // upper half is its conjugate mirror and is never touched.
    for (int k = 0; k <= m; ++k)
    {
        const int kk = k % m;               // z index (k==m wraps to 0)
        const int mk = (m - k) % m;
        const Complex zk  = z[kk];
        const Complex zmk = std::conj(z[mk]);
        const Complex fe = 0.5f * (zk + zmk);                 // even-sample DFT
        const Complex fo = Complex(0.0f, -0.5f) * (zk - zmk); // odd-sample DFT
        spectrum[k] = fe + unpackTwiddles[static_cast<size_t>(k)] * fo;
    }
}

void Fft::inverseReal(const Complex* spectrum, Sample* realOutput) const noexcept
{
    if (!half)
    {
        for (int i = 0; i < fftSize; ++i)
            scratch[static_cast<size_t>(i)] = spectrum[i];
        transform(scratch.data(), true);
        for (int i = 0; i < fftSize; ++i)
            realOutput[i] = scratch[static_cast<size_t>(i)].real();
        return;
    }

    const int m = fftSize / 2;

    // Invert the unpack, rebuild the N/2 complex sequence, and take one inverse
    // N/2-point FFT. Reads only bins 0..N/2 (conjugate symmetry is implied).
    Complex* z = halfScratch.data();
    for (int k = 0; k < m; ++k)
    {
        const Complex xk  = spectrum[k];
        const Complex xmk = std::conj(spectrum[m - k]);
        const Complex fe = 0.5f * (xk + xmk);
        const Complex fo = 0.5f * (xk - xmk) * std::conj(unpackTwiddles[static_cast<size_t>(k)]);
        z[k] = fe + Complex(0.0f, 1.0f) * fo;
    }
    half->transform(z, true);   // scaled by 1/(N/2)

    for (int n = 0; n < m; ++n)
    {
        realOutput[2 * n]     = z[n].real();
        realOutput[2 * n + 1] = z[n].imag();
    }
}

} // namespace chronos::dsp
