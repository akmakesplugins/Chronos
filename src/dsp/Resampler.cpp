// Copyright (c) 2026 ZROLogic. All rights reserved. Proprietary and confidential.
// Chronos — dsp/Resampler.cpp

#include "Resampler.h"

#include <cmath>

namespace chronos::dsp
{

namespace
{
    /// 4-point, 3rd-order Hermite (Catmull-Rom) interpolation. `t` in [0,1) is the
    /// fractional position between x0 and x1. Reaches one sample back (xm1) and two
    /// forward (x1, x2), which fixes the streaming history requirement at exactly
    /// one past sample.
    inline Sample hermite(Sample xm1, Sample x0, Sample x1, Sample x2, float t) noexcept
    {
        const float c0 = x0;
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + c0;
    }
}

void Resampler::prepare(int numChannels)
{
    channelCount = numChannels;
    history.assign(static_cast<size_t>(numChannels), Sample{0});
    reset();
}

void Resampler::reset() noexcept
{
    readPos = 0.0;
    primed  = false;
    for (auto& h : history)
        h = Sample{0};
}

int Resampler::outputAvailable(int numInput) const noexcept
{
    // The loop can emit while floor(readPos) + 2 < numInput. Conservatively:
    const double reach = static_cast<double>(numInput) - 2.0 - readPos;
    if (reach <= 0.0)
        return 0;
    return static_cast<int>(std::floor(reach / step));
}

Resampler::Progress Resampler::process(const Sample* const* input, int numInput,
                                       Sample* const* output, int numOutput,
                                       int numChannels) noexcept
{
    int produced = 0;

    while (produced < numOutput)
    {
        const int i = static_cast<int>(std::floor(readPos));

        // Need input indices i-1 .. i+2. Stop when the two look-ahead samples run
        // past the end of the supplied block; the caller will feed more next call.
        if (i + 2 >= numInput)
            break;

        const float t = static_cast<float>(readPos - i);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const Sample* in = input[ch];
            const Sample xm1 = (i - 1 < 0) ? history[static_cast<size_t>(ch)] : in[i - 1];
            const Sample x0  = in[i];
            const Sample x1  = in[i + 1];
            const Sample x2  = in[i + 2];
            output[ch][produced] = hermite(xm1, x0, x1, x2, t);
        }

        readPos += step;
        ++produced;
    }

    // Retire input we will never read again. The next output needs index
    // floor(readPos) - 1, so everything below that is safe to discard. We rebase
    // readPos onto the retained input and stash the sample just before it as the
    // new history (x[-1]) for the next call.
    int consumed = static_cast<int>(std::floor(readPos)) - 1;
    if (consumed < 0)
        consumed = 0;
    if (consumed > numInput)
        consumed = numInput;

    if (consumed > 0)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            history[static_cast<size_t>(ch)] = input[ch][consumed - 1];
        readPos -= consumed;
    }

    primed = true;
    return { consumed, produced };
}

} // namespace chronos::dsp
