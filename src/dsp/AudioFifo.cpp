// Copyright (c) 2026 ZROLogic. All rights reserved. Proprietary and confidential.
// Chronos — dsp/AudioFifo.cpp

#include "AudioFifo.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace chronos::dsp
{

void AudioFifo::prepare(int channels, int cap)
{
    channelCount = channels;
    capacity     = cap;
    storage.assign(static_cast<size_t>(channels) * static_cast<size_t>(cap), Sample{0});
    this->channels.resize(static_cast<size_t>(channels));
    for (int ch = 0; ch < channels; ++ch)
        this->channels[static_cast<size_t>(ch)] = storage.data() + static_cast<size_t>(ch) * static_cast<size_t>(cap);
    reset();
}

void AudioFifo::reset() noexcept
{
    writePos = readPos = count = 0;
}

void AudioFifo::write(const Sample* const* src, int srcOffset, int numSamples) noexcept
{
    assert(numSamples <= freeSpace() && "AudioFifo overflow");

    // The write may wrap around the ring, so split into up to two contiguous runs.
    int remaining = numSamples;
    int pos = writePos;
    int srcPos = srcOffset;
    while (remaining > 0)
    {
        const int run = std::min(remaining, capacity - pos);
        for (int ch = 0; ch < channelCount; ++ch)
            std::memcpy(channels[static_cast<size_t>(ch)] + pos,
                        src[ch] + srcPos,
                        static_cast<size_t>(run) * sizeof(Sample));
        pos = (pos + run) % capacity;
        srcPos += run;
        remaining -= run;
    }
    writePos = pos;
    count += numSamples;
}

void AudioFifo::writeSilence(int numSamples) noexcept
{
    assert(numSamples <= freeSpace() && "AudioFifo overflow");
    int remaining = numSamples;
    int pos = writePos;
    while (remaining > 0)
    {
        const int run = std::min(remaining, capacity - pos);
        for (int ch = 0; ch < channelCount; ++ch)
            std::memset(channels[static_cast<size_t>(ch)] + pos, 0, static_cast<size_t>(run) * sizeof(Sample));
        pos = (pos + run) % capacity;
        remaining -= run;
    }
    writePos = pos;
    count += numSamples;
}

int AudioFifo::read(Sample* const* dst, int dstOffset, int numSamples) noexcept
{
    const int toRead = std::min(numSamples, count);
    int remaining = toRead;
    int pos = readPos;
    int dstPos = dstOffset;
    while (remaining > 0)
    {
        const int run = std::min(remaining, capacity - pos);
        for (int ch = 0; ch < channelCount; ++ch)
            std::memcpy(dst[ch] + dstPos,
                        channels[static_cast<size_t>(ch)] + pos,
                        static_cast<size_t>(run) * sizeof(Sample));
        pos = (pos + run) % capacity;
        dstPos += run;
        remaining -= run;
    }
    readPos = pos;
    count -= toRead;
    return toRead;
}

int AudioFifo::peek(Sample* const* dst, int dstOffset, int numSamples) const noexcept
{
    const int toRead = std::min(numSamples, count);
    int remaining = toRead;
    int pos = readPos;
    int dstPos = dstOffset;
    while (remaining > 0)
    {
        const int run = std::min(remaining, capacity - pos);
        for (int ch = 0; ch < channelCount; ++ch)
            std::memcpy(dst[ch] + dstPos,
                        channels[static_cast<size_t>(ch)] + pos,
                        static_cast<size_t>(run) * sizeof(Sample));
        pos = (pos + run) % capacity;
        dstPos += run;
        remaining -= run;
    }
    return toRead;
}

int AudioFifo::discard(int numSamples) noexcept
{
    const int toDiscard = std::min(numSamples, count);
    readPos = (readPos + toDiscard) % capacity;
    count -= toDiscard;
    return toDiscard;
}

} // namespace chronos::dsp
