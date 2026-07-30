// Chronos — StretchParameters.h
//
// The complete, plain-old-data description of *what* the engine should do to the
// audio. It is deliberately decoupled from *how* (StreamConfig / EngineType live
// separately) so a host can automate these values per block without touching the
// stream setup. All fields are safe to change every block; the facade smooths the
// continuous ones internally to avoid zipper noise.

#pragma once

#include "Types.h"

namespace chronos
{

struct StretchParameters
{
    /// Output-duration multiplier. 1.0 = unchanged, 2.0 = twice as long (half
    /// speed), 0.5 = half as long (double speed). Independent of pitch.
    double timeRatio = 1.0;

    /// Pitch multiplier as a frequency ratio. 1.0 = unchanged, 2.0 = one octave
    /// up, 0.5 = one octave down. Use pitchSemitones() to set musically.
    double pitchRatio = 1.0;

    /// When true, the spectral envelope (which carries vocal/instrument formant
    /// character) is held constant while the fine harmonic structure is shifted.
    /// This is what keeps a pitched-up voice from sounding like a chipmunk.
    bool preserveFormants = false;

    /// Independent formant shift applied *on top of* preservation, as a frequency
    /// ratio. 1.0 = leave formants where they are. Only meaningful when the engine
    /// estimates the envelope (preserveFormants or formantRatio != 1).
    double formantRatio = 1.0;

    /// Detect transients (attacks/onsets) and keep them sharp by resetting phase
    /// and momentarily suspending stretching across them. Costs a little CPU;
    /// hugely improves drums, plucks, and consonants.
    bool preserveTransients = true;

    /// Algorithm selection and fidelity/CPU trade-off.
    EngineType engine  = EngineType::Auto;
    Quality    quality = Quality::Good;

    // -- Musical convenience setters -------------------------------------------

    /// Set pitch by a signed interval in semitones (fractional allowed).
    void setPitchSemitones(double semitones) noexcept
    {
        pitchRatio = semitonesToRatio(semitones);
    }

    /// Set formant shift by a signed interval in semitones.
    void setFormantSemitones(double semitones) noexcept
    {
        formantRatio = semitonesToRatio(semitones);
    }

    /// The time-stretch factor the engine must apply when time and pitch are
    /// realised as "stretch then resample". The engine stretches by this amount
    /// (pitch-preserving); the downstream resampler then reads `pitchRatio` input
    /// samples per output sample, which both restores the target duration and
    /// shifts the pitch. Derivation:
    ///   finalLen = engineLen / pitchRatio = inputLen * E / pitchRatio
    ///   want finalLen = inputLen * timeRatio  =>  E = timeRatio * pitchRatio
    [[nodiscard]] double engineStretch() const noexcept
    {
        return timeRatio * pitchRatio;
    }

    [[nodiscard]] bool isValid() const noexcept
    {
        return timeRatio > 0.0 && pitchRatio > 0.0 && formantRatio > 0.0;
    }
};

} // namespace chronos
