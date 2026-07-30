// Chronos — dsp/AudioFifo.h
//
// A fixed-capacity, multichannel sample FIFO (ring buffer). It is used within a
// single thread to bridge two different block sizes: the engine produces/consumes
// audio in its own natural hop size, while the host pushes and pulls arbitrary
// block lengths. Because producer and consumer are the same (audio) thread, no
// atomics are needed — but capacity is fixed and all storage is allocated in
// prepare(), so read()/write() are allocation-free and real-time safe.

#pragma once

#include <vector>

#include "chronos/Types.h"

namespace chronos::dsp
{

class AudioFifo
{
public:
    AudioFifo() = default;

    /// Allocate storage for `channels` channels and `capacity` samples each.
    void prepare(int channels, int capacity);

    void reset() noexcept;

    [[nodiscard]] int numChannels() const noexcept { return channelCount; }
    [[nodiscard]] int numReady()  const noexcept { return count; }
    [[nodiscard]] int freeSpace() const noexcept { return capacity - count; }
    [[nodiscard]] int capacitySamples() const noexcept { return capacity; }

    /// Append `numSamples` from planar `src[channel][offset..]`. The caller must
    /// ensure freeSpace() >= numSamples (checked with an assert in debug).
    void write(const Sample* const* src, int srcOffset, int numSamples) noexcept;

    /// Append `numSamples` of silence (used to prime latency / flush tails).
    void writeSilence(int numSamples) noexcept;

    /// Pop up to `numSamples` into planar `dst[channel][dstOffset..]`. Returns the
    /// number actually read (min of numSamples and numReady()).
    int read(Sample* const* dst, int dstOffset, int numSamples) noexcept;

    /// Copy up to `numSamples` into `dst` *without* consuming them (used to read a
    /// full STFT frame while only advancing by the hop). Returns the number copied.
    int peek(Sample* const* dst, int dstOffset, int numSamples) const noexcept;

    /// Discard up to `numSamples` without copying. Returns the number discarded.
    int discard(int numSamples) noexcept;

private:
    std::vector<Sample>  storage;   ///< [channel][capacity], contiguous.
    std::vector<Sample*> channels;  ///< Base pointers per channel.
    int channelCount = 0;
    int capacity     = 0;
    int writePos     = 0;
    int readPos      = 0;
    int count        = 0;           ///< Samples currently stored.
};

} // namespace chronos::dsp
