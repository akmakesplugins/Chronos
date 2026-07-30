// Copyright (c) 2026 ZROLogic. All rights reserved. Proprietary and confidential.
// Chronos — AudioBuffer.h
//
// Two small buffer abstractions:
//
//   AudioBlockView  — a non-owning, planar (channel-pointer) view. This is the
//                     currency of the real-time API: the host already owns the
//                     memory (e.g. a juce::AudioBuffer's array-of-write-pointers),
//                     and Chronos never copies or allocates to accept it. Layout
//                     is [channel][sample], matching JUCE and élastique.
//
//   AudioBuffer     — a minimal owning, planar buffer used for offline work and
//                     internal scratch. Allocation is explicit and confined to
//                     setup; the real-time path only ever *views* into it.
//
// Keeping these framework-agnostic means the DSP core has no JUCE dependency; the
// juce adapter simply wraps a juce::AudioBuffer as an AudioBlockView.

#pragma once

#include "Types.h"

#include <cstring>
#include <vector>

namespace chronos
{

/// Non-owning view over planar audio. Cheap to copy (it's just pointers + sizes).
class AudioBlockView
{
public:
    AudioBlockView() = default;

    AudioBlockView(Sample* const* channels, int numChannels, int numSamples) noexcept
        : data(channels), channelCount(numChannels), sampleCount(numSamples)
    {
    }

    [[nodiscard]] int numChannels() const noexcept { return channelCount; }
    [[nodiscard]] int numSamples()  const noexcept { return sampleCount; }
    [[nodiscard]] bool isEmpty()    const noexcept { return sampleCount == 0 || channelCount == 0; }

    // A view is a non-owning handle: its own const-ness protects the handle's
    // fields, not the (always mutable) audio it points at — like a span. So the
    // channel accessors are const and return writable pointers.
    [[nodiscard]] Sample* channel(int ch) const noexcept { return data[ch]; }

    /// The underlying planar channel-pointer array, for APIs taking `float**`.
    [[nodiscard]] Sample* const* channelArray() const noexcept { return data; }

private:
    Sample* const* data = nullptr;
    int channelCount = 0;
    int sampleCount  = 0;
};

/// Minimal owning planar buffer. Not real-time safe to resize; do it in setup.
class AudioBuffer
{
public:
    AudioBuffer() = default;

    AudioBuffer(int numChannels, int numSamples) { resize(numChannels, numSamples); }

    void resize(int numChannels, int numSamples)
    {
        channelCount = numChannels;
        sampleCount  = numSamples;
        storage.assign(static_cast<size_t>(numChannels) * static_cast<size_t>(numSamples), Sample{0});
        pointers.resize(static_cast<size_t>(numChannels));
        for (int ch = 0; ch < numChannels; ++ch)
            pointers[static_cast<size_t>(ch)] = storage.data() + static_cast<size_t>(ch) * static_cast<size_t>(numSamples);
    }

    void clear() noexcept
    {
        std::memset(storage.data(), 0, storage.size() * sizeof(Sample));
    }

    [[nodiscard]] int numChannels() const noexcept { return channelCount; }
    [[nodiscard]] int numSamples()  const noexcept { return sampleCount; }

    [[nodiscard]] Sample*       channel(int ch)       noexcept { return pointers[static_cast<size_t>(ch)]; }
    [[nodiscard]] const Sample* channel(int ch) const noexcept { return pointers[static_cast<size_t>(ch)]; }

    /// A view over the whole buffer (or a leading sub-length), for passing into
    /// the streaming API without exposing internals.
    [[nodiscard]] AudioBlockView view() noexcept
    {
        return AudioBlockView(pointers.data(), channelCount, sampleCount);
    }

    [[nodiscard]] AudioBlockView view(int length) noexcept
    {
        return AudioBlockView(pointers.data(), channelCount, length);
    }

private:
    std::vector<Sample>  storage;   ///< Contiguous [ch0 samples...][ch1 samples...].
    std::vector<Sample*> pointers;  ///< Channel base pointers into `storage`.
    int channelCount = 0;
    int sampleCount  = 0;
};

} // namespace chronos
