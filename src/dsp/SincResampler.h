// Chronos — dsp/SincResampler.h
//
// A streaming polyphase windowed-sinc fractional resampler — the high-quality
// pitch stage. It improves on the cubic-Hermite Resampler in two ways that matter
// for pitch shifting:
//
//   * A long windowed-sinc kernel (2L taps) is far closer to the ideal
//     interpolator than a 4-point cubic, so upward pitch shifts of HF-rich material
//     stay clean instead of gaining interpolation noise.
//   * A built-in anti-alias / anti-image low-pass: the kernel cutoff is set to
//     min(1, 1/ratio) in Nyquist units. On pitch-UP (decimation) this is the
//     anti-alias filter that stops HF folding back; on pitch-DOWN / unity it
//     suppresses interpolation images. The cubic had neither.
//
// Real-time safety: a small BANK of kernel tables (one per cutoff bucket) is built
// in prepare(); setRatio() only selects a table (O(1), no compute, no allocation).
// Per-channel history holds the 2L look-back samples the wide kernel needs, carried
// across calls for click-free streaming. Same process() contract as Resampler so it
// is a drop-in in the facade.

#pragma once

#include <array>
#include <vector>

#include "chronos/Types.h"

namespace chronos::dsp
{

class SincResampler
{
public:
    SincResampler() = default;

    /// Allocate history and build the kernel table bank. `halfLength` = L (kernel
    /// is 2L taps); `subPhases` = P fractional positions. Defaults give 32 taps at
    /// 512 sub-phases — transparent to well past -90 dB.
    void prepare(int numChannels, int halfLength = 16, int subPhases = 512);

    void reset() noexcept;

    /// Input samples consumed per output sample. >1 pitches up (and engages the
    /// anti-alias low-pass); <=1 uses the full-band kernel. Selects a table bucket;
    /// safe on the audio thread.
    void setRatio(double inputPerOutput) noexcept;

    [[nodiscard]] double ratio() const noexcept { return step; }

    [[nodiscard]] int outputAvailable(int numInput) const noexcept;

    struct Progress { int inputConsumed; int outputProduced; };

    /// Resample all channels together (shared fractional position, per-channel
    /// history). Reads planar input[ch][0..numInput), writes up to numOutput into
    /// output[ch][0..). Returns samples retired from the input and produced.
    Progress process(const Sample* const* input, int numInput,
                     Sample* const* output, int numOutput, int numChannels) noexcept;

private:
    [[nodiscard]] Sample tap(const Sample* in, int numInput, int ch, int idx) const noexcept;

    int L = 16;            ///< Kernel half-length (taps = 2L).
    int P = 512;           ///< Sub-phase count.
    int numBuckets = 0;    ///< Number of cutoff tables.
    int activeBucket = 0;  ///< Currently selected table.

    double step    = 1.0;
    double readPos = 0.0;

    // table[bucket] is a flat (P+1) x (2L) array of coefficients.
    std::vector<std::vector<float>> tables;
    std::vector<float> bucketCutoff;   ///< Cutoff (Nyquist units) per bucket.

    // Per-channel look-back history: the H = 2L samples immediately before the
    // current input block (virtual indices -H..-1).
    std::vector<std::vector<Sample>> history;   ///< [ch][H]
    std::vector<Sample> histTmp;                ///< [H] scratch to rebuild history without aliasing
    int H = 0;
    int channelCount = 0;
};

} // namespace chronos::dsp
