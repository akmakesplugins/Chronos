// Copyright (c) 2026 ZROLogic. All rights reserved. Proprietary and confidential.
// Chronos — a modular, real-time audio time-stretching and pitch-shifting engine.
//
// Types.h — fundamental value types and stream configuration shared across the
// whole public API. Intentionally dependency-free: only <cstdint>/<cmath> so the
// core library can be embedded anywhere (JUCE, bare CMake, a CLI tool) without
// pulling in a framework.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace chronos
{

/// The engine works internally in 32-bit float. This alias documents intent and
/// keeps a single seam should a double-precision build ever be wanted.
using Sample = float;

/// Which time-stretch algorithm to run. `Auto` lets the facade pick per-content
/// (see TimeStretcher), trading a little analysis latency for better results.
enum class EngineType
{
    Auto,           ///< Choose the engine from signal content.
    PhaseVocoder,   ///< Frequency-domain, phase-locked. Robust for dense/complex material.
    Wsola,          ///< Time-domain overlap-add. Cheap, sharp transients, great for percussive/mono.
    Sinusoidal      ///< MQ partial tracking + oscillator bank. Best purity on sparse/expressive tonal.
};

/// Quality preset. Higher quality uses larger FFTs / more overlap / more of the
/// expensive refinements (phase locking, transient handling). It never changes
/// the *meaning* of the parameters — only the fidelity/CPU trade-off.
enum class Quality
{
    Draft,      ///< Smallest windows, minimal refinement. Preview / many voices.
    Good,       ///< Balanced default for real-time instruments.
    High,       ///< Larger windows, full phase locking + transient handling.
    Best        ///< Maximum window/overlap. Intended for offline rendering.
};

/// Everything the engine needs to preallocate before the first `process()` call.
/// Passing this to `prepare()` is the *only* point at which allocation happens;
/// after that the streaming path is real-time safe.
struct StreamConfig
{
    double sampleRate   = 44100.0;  ///< Output/device sample rate in Hz.
    int    numChannels  = 2;        ///< Channels processed with phase coherence.
    int    maxBlockSize = 512;      ///< Largest block the host will ever request.

    [[nodiscard]] bool isValid() const noexcept
    {
        return sampleRate > 0.0 && numChannels > 0 && maxBlockSize > 0;
    }
};

// ---------------------------------------------------------------------------
// Small, allocation-free unit helpers. These make call sites read in musical
// terms (semitones, cents) instead of raw frequency ratios.
// ---------------------------------------------------------------------------

/// Convert a pitch interval in semitones to a linear frequency ratio.
/// 12 semitones -> 2.0 (one octave up), -12 -> 0.5.
[[nodiscard]] inline double semitonesToRatio(double semitones) noexcept
{
    return std::pow(2.0, semitones / 12.0);
}

/// Inverse of semitonesToRatio(). ratio 2.0 -> 12 semitones.
[[nodiscard]] inline double ratioToSemitones(double ratio) noexcept
{
    return 12.0 * std::log2(ratio);
}

/// Clamp helper (kept local so the header stays free of <algorithm> in hot paths).
template <typename T>
[[nodiscard]] inline T clamp(T value, T lo, T hi) noexcept
{
    return value < lo ? lo : (value > hi ? hi : value);
}

} // namespace chronos
