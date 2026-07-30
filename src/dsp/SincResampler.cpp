// Chronos — dsp/SincResampler.cpp

#include "SincResampler.h"

#include <algorithm>
#include <cmath>

#include "MathUtils.h"

namespace chronos::dsp
{

namespace
{
    inline double sinc(double x) noexcept
    {
        if (std::abs(x) < 1.0e-9) return 1.0;
        const double px = kPi * x;
        return std::sin(px) / px;
    }

    // Zeroth-order modified Bessel function I0, for the Kaiser window.
    double besselI0(double x) noexcept
    {
        double sum = 1.0, term = 1.0;
        const double halfX = x * 0.5;
        for (int k = 1; k < 32; ++k)
        {
            term *= (halfX / k) * (halfX / k);
            sum += term;
            if (term < 1.0e-12 * sum) break;
        }
        return sum;
    }
}

void SincResampler::prepare(int numChannels, int halfLength, int subPhases)
{
    L = halfLength;
    P = subPhases;
    H = 2 * L;
    channelCount = numChannels;

    // Cutoff buckets: 1.0 (full band, for downshift/unity) plus a set below 1.0 for
    // upshift anti-aliasing. setRatio() picks the largest cutoff <= min(1, 1/ratio).
    bucketCutoff = { 1.0f, 0.92f, 0.84f, 0.76f, 0.68f, 0.60f, 0.52f, 0.44f, 0.36f, 0.28f };
    numBuckets = static_cast<int>(bucketCutoff.size());

    // Kaiser window, beta ~ 9.5 gives roughly -90 dB stopband over 2L=32 taps.
    const double beta = 9.5;
    const double denomI0 = besselI0(beta);

    tables.assign(static_cast<size_t>(numBuckets), std::vector<float>(static_cast<size_t>((P + 1) * H)));
    for (int b = 0; b < numBuckets; ++b)
    {
        const double cutoff = bucketCutoff[static_cast<size_t>(b)];
        auto& tab = tables[static_cast<size_t>(b)];
        for (int p = 0; p <= P; ++p)
        {
            const double frac = static_cast<double>(p) / P;   // 0..1 position between center taps
            for (int j = 0; j < H; ++j)
            {
                // Tap j covers input offset (j - (L-1)) - frac relative to the read
                // position; the ideal reconstruction filter is cutoff*sinc(cutoff*t)
                // windowed by a Kaiser of half-width L.
                const double t = (j - (L - 1)) - frac;
                const double win = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - (t / L) * (t / L)))) / denomI0;
                const double h = cutoff * sinc(cutoff * t) * win;
                tab[static_cast<size_t>(p * H + j)] = static_cast<float>(h);
            }
        }

        // Normalise each sub-phase to unity DC gain (removes ripple-induced level
        // drift so a steady tone keeps its amplitude across the fractional grid).
        for (int p = 0; p <= P; ++p)
        {
            double s = 0.0;
            for (int j = 0; j < H; ++j) s += tab[static_cast<size_t>(p * H + j)];
            if (std::abs(s) > 1.0e-9)
            {
                const float g = static_cast<float>(1.0 / s);
                for (int j = 0; j < H; ++j) tab[static_cast<size_t>(p * H + j)] *= g;
            }
        }
    }

    history.assign(static_cast<size_t>(numChannels), std::vector<Sample>(static_cast<size_t>(H), Sample{0}));
    histTmp.assign(static_cast<size_t>(H), Sample{0});
    reset();
}

void SincResampler::reset() noexcept
{
    readPos = 0.0;
    for (auto& h : history)
        std::fill(h.begin(), h.end(), Sample{0});
}

void SincResampler::setRatio(double inputPerOutput) noexcept
{
    step = inputPerOutput;
    const double targetCutoff = inputPerOutput > 1.0 ? 1.0 / inputPerOutput : 1.0;
    // Pick the largest bucket cutoff that does not exceed the target (so we never
    // pass aliasing energy); fall back to the smallest if the ratio is extreme.
    int chosen = numBuckets - 1;
    for (int b = 0; b < numBuckets; ++b)
        if (bucketCutoff[static_cast<size_t>(b)] <= targetCutoff + 1.0e-6f) { chosen = b; break; }
    activeBucket = chosen;
}

int SincResampler::outputAvailable(int numInput) const noexcept
{
    const double reach = static_cast<double>(numInput) - static_cast<double>(L) - readPos;
    if (reach <= 0.0) return 0;
    return static_cast<int>(std::floor(reach / step));
}

Sample SincResampler::tap(const Sample* in, int numInput, int ch, int idx) const noexcept
{
    if (idx >= 0)
        return idx < numInput ? in[idx] : Sample{0};
    // Negative index -> look-back history (idx in [-H, -1]).
    const int h = H + idx;
    return (h >= 0) ? history[static_cast<size_t>(ch)][static_cast<size_t>(h)] : Sample{0};
}

SincResampler::Progress SincResampler::process(const Sample* const* input, int numInput,
                                               Sample* const* output, int numOutput,
                                               int numChannels) noexcept
{
    const float* tab = tables[static_cast<size_t>(activeBucket)].data();
    int produced = 0;

    while (produced < numOutput)
    {
        const int i = static_cast<int>(std::floor(readPos));
        // The kernel touches input indices i-(L-1) .. i+L. Stop when the forward
        // reach exceeds the supplied block; more input arrives next call.
        if (i + L >= numInput)
            break;

        const double fracPos = readPos - i;
        int p = static_cast<int>(std::lround(fracPos * P));
        if (p > P) p = P;
        const float* coeff = tab + static_cast<size_t>(p) * static_cast<size_t>(H);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const Sample* in = input[ch];
            float acc = 0.0f;
            const int base = i - (L - 1);
            for (int j = 0; j < H; ++j)
                acc += coeff[j] * tap(in, numInput, ch, base + j);
            output[ch][produced] = acc;
        }

        readPos += step;
        ++produced;
    }

    // Retire input we no longer need. The next kernel's lowest index is
    // floor(readPos) - (L-1), so we keep the L-1 preceding samples as look-back and
    // rebase. Stash the trailing H samples up to the retire boundary as history.
    int consumed = static_cast<int>(std::floor(readPos)) - (L - 1);
    if (consumed < 0) consumed = 0;
    if (consumed > numInput) consumed = numInput;

    if (consumed > 0)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            // Build into a temp first: for small `consumed` some source indices are
            // still in the OLD history, which we must not overwrite mid-loop.
            for (int k = 0; k < H; ++k)
                histTmp[static_cast<size_t>(k)] = tap(input[ch], numInput, ch, consumed - H + k);
            std::copy(histTmp.begin(), histTmp.end(), history[static_cast<size_t>(ch)].begin());
        }
        readPos -= consumed;
    }

    return { consumed, produced };
}

} // namespace chronos::dsp
