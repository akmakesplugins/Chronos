// Copyright (c) 2026 ZROLogic. All rights reserved. Proprietary and confidential.
// Chronos — dsp/Resampler.h
//
// A streaming fractional resampler. Pitch shifting in Chronos is realised as
// "time-stretch by 1/pitch, then resample by pitch": the phase vocoder changes
// duration while preserving pitch, and this resampler then rescales the time axis
// to restore duration and move the pitch. Decoupling the two keeps each stage
// simple and independently testable.
//
// The kernel is 4-point cubic Hermite interpolation: cheap, C1-continuous, and
// clearly better than linear for audio. It is isolated behind this class so a
// windowed-sinc / polyphase kernel can replace it for the "Best" tier without
// touching callers. All channels are resampled together so they share one
// fractional read position; per-channel history (one sample — the kernel only
// reaches one sample back) is carried across calls for click-free streaming.

#pragma once

#include <vector>

#include "chronos/Types.h"

namespace chronos::dsp
{

class Resampler
{
public:
    Resampler() = default;

    /// Allocate per-channel history. Call at setup.
    void prepare(int numChannels);

    void reset() noexcept;

    /// Number of input samples consumed per output sample. > 1 pitches up (reads
    /// faster than it writes); < 1 stretches. Safe to change per block; the change
    /// takes effect at the next output sample, so ratio automation is glitch-free.
    void setRatio(double inputPerOutput) noexcept { step = inputPerOutput; }

    [[nodiscard]] double ratio() const noexcept { return step; }

    /// Conservative estimate of how many output samples `numInput` buffered input
    /// samples can yield at the current ratio and phase. Used by the caller to
    /// decide how much input to have ready.
    [[nodiscard]] int outputAvailable(int numInput) const noexcept;

    struct Progress { int inputConsumed; int outputProduced; };

    /// Resample all channels together. Reads planar `input[ch][0..numInput)` and
    /// writes up to `numOutput` samples into planar `output[ch][0..)`. Stops early
    /// if it runs out of input. Returns how many input samples may now be retired
    /// (discarded from the caller's FIFO) and how many outputs were produced.
    Progress process(const Sample* const* input, int numInput,
                     Sample* const* output, int numOutput, int numChannels) noexcept;

private:
    double step    = 1.0;  ///< Input advanced per output sample.
    double readPos = 0.0;  ///< Fractional read index into the current input block.

    std::vector<Sample> history;  ///< One previous input sample per channel (x[-1]).
    int channelCount = 0;
    bool primed = false;
};

} // namespace chronos::dsp
