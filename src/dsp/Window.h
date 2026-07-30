// Chronos — dsp/Window.h
//
// Analysis/synthesis window functions for the STFT. The phase vocoder applies a
// window on analysis *and* synthesis (a "dual window" / weighted overlap-add
// scheme), which suppresses the discontinuities that windowing only once would
// leave. The key correctness constraint is COLA — Constant OverLap-Add: with the
// chosen hop the squared window must sum to a constant across frames, otherwise
// the output amplitude modulates at the frame rate.
//
// We normalise the window so that analysis+synthesis+overlap reconstructs unity
// gain for the configured hop, computed once at setup.

#pragma once

#include <vector>

#include "chronos/Types.h"

namespace chronos::dsp
{

enum class WindowType
{
    Hann,       ///< Default. Smooth, COLA-friendly at 50/75% overlap.
    Hamming,
    BlackmanHarris
};

class Window
{
public:
    Window() = default;

    /// Build a window of `size` samples. If `analysisHop` > 0 the window is scaled
    /// so that summing window^2 over overlapping frames at that hop yields ~1.0,
    /// giving unity-gain reconstruction for weighted overlap-add.
    void design(int size, WindowType type, int analysisHop);

    [[nodiscard]] int size() const noexcept { return static_cast<int>(coefficients.size()); }
    [[nodiscard]] const Sample* data() const noexcept { return coefficients.data(); }
    [[nodiscard]] Sample operator[](int i) const noexcept { return coefficients[static_cast<size_t>(i)]; }

    /// Multiply `frame` in place by the window.
    void apply(Sample* frame) const noexcept;

private:
    std::vector<Sample> coefficients;
};

} // namespace chronos::dsp
