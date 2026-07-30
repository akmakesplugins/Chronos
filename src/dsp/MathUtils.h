// Copyright (c) 2026 ZROLogic. All rights reserved. Proprietary and confidential.
// Chronos — dsp/MathUtils.h
//
// Small, header-only numeric helpers used across the DSP core. Kept tiny and
// branch-light because several of these run per-bin, per-frame in hot loops.

#pragma once

#include <cmath>
#include <complex>

#include "chronos/Types.h"

namespace chronos::dsp
{

constexpr double kPi     = 3.14159265358979323846;
constexpr double kTwoPi  = 2.0 * kPi;

using Complex = std::complex<float>;

/// Wrap a phase (radians) into the principal range (-pi, pi]. This is the single
/// most important operation in a phase vocoder: instantaneous frequency is only
/// defined modulo 2*pi, so every phase difference must be re-wrapped here.
[[nodiscard]] inline float wrapPhase(float phase) noexcept
{
    // Fast, allocation-free wrap. round() maps to the nearest multiple of 2*pi.
    constexpr float twoPi = static_cast<float>(kTwoPi);
    return phase - twoPi * std::round(phase / twoPi);
}

/// True power-of-two check (used to validate FFT sizes at setup).
[[nodiscard]] inline bool isPowerOfTwo(int n) noexcept
{
    return n > 0 && (n & (n - 1)) == 0;
}

/// Integer base-2 logarithm of a power-of-two (its number of trailing zero bits).
[[nodiscard]] inline int log2Int(int n) noexcept
{
    int bits = 0;
    while (n > 1) { n >>= 1; ++bits; }
    return bits;
}

/// Linear interpolation.
[[nodiscard]] inline float lerp(float a, float b, float t) noexcept
{
    return a + (b - a) * t;
}

/// Convert a linear amplitude to decibels, floored so silence maps to a finite
/// value instead of -inf (handy for transient thresholds and metering).
[[nodiscard]] inline float amplitudeToDb(float amp, float floorDb = -120.0f) noexcept
{
    return amp > 1.0e-9f ? 20.0f * std::log10(amp) : floorDb;
}

} // namespace chronos::dsp
