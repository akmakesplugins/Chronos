// Chronos — test/TestMain.cpp
//
// A self-contained correctness harness. It generates known signals, runs them
// through the engine offline and streaming, and checks the properties that matter:
//   * output duration tracks the time ratio,
//   * a tone's pitch is preserved under time-stretch and moved correctly by pitch
//     shift (measured from the spectrum, not assumed),
//   * energy is roughly conserved (no gross gain error),
//   * output is always finite (no NaN/Inf/denormal blow-ups),
//   * both engines and the formant path run end to end.
//
// It links only against the public library plus the private FFT (used here purely
// as a measurement tool). Exit code is non-zero if any check fails.

#include <chronos/Chronos.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "dsp/Fft.h"   // measurement only

using namespace chronos;

// ---------------------------------------------------------------------------
// Tiny test framework.
// ---------------------------------------------------------------------------
namespace
{
    int gChecks = 0;
    int gFailures = 0;

    void check(bool cond, const std::string& what, const std::string& detail = "")
    {
        ++gChecks;
        if (!cond)
        {
            ++gFailures;
            std::printf("  [FAIL] %s   %s\n", what.c_str(), detail.c_str());
        }
        else
        {
            std::printf("  [ ok ] %s   %s\n", what.c_str(), detail.c_str());
        }
    }

    // ---- Signal generation & measurement ---------------------------------

    AudioBuffer makeSine(int channels, int numSamples, double freq, double sr, float amp = 0.5f)
    {
        AudioBuffer b(channels, numSamples);
        for (int c = 0; c < channels; ++c)
            for (int n = 0; n < numSamples; ++n)
                b.channel(c)[n] = amp * std::sin(2.0 * M_PI * freq * n / sr);
        return b;
    }

    AudioBuffer makeClicks(int channels, int numSamples, int periodSamples, float amp = 0.8f)
    {
        AudioBuffer b(channels, numSamples);
        for (int c = 0; c < channels; ++c)
            for (int n = 0; n < numSamples; ++n)
                b.channel(c)[n] = (n % periodSamples == 0) ? amp : 0.0f;
        return b;
    }

    double rms(const Sample* x, int n)
    {
        double s = 0.0;
        for (int i = 0; i < n; ++i)
            s += static_cast<double>(x[i]) * x[i];
        return std::sqrt(s / std::max(1, n));
    }

    bool allFinite(const AudioBuffer& b)
    {
        for (int c = 0; c < b.numChannels(); ++c)
            for (int n = 0; n < b.numSamples(); ++n)
                if (!std::isfinite(b.channel(c)[n]))
                    return false;
        return true;
    }

    // Dominant frequency of a middle segment, via a Hann-windowed FFT with
    // parabolic peak interpolation for sub-bin accuracy.
    double dominantFrequency(const Sample* x, int n, double sr)
    {
        const int N = 8192;
        if (n < N + 16)
            return 0.0;

        const int start = n / 2 - N / 2;
        std::vector<dsp::Complex> spec(static_cast<size_t>(N));
        std::vector<Sample> frame(static_cast<size_t>(N));
        for (int i = 0; i < N; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / (N - 1));
            frame[static_cast<size_t>(i)] = static_cast<Sample>(x[start + i] * w);
        }
        dsp::Fft fft(N);
        fft.forwardReal(frame.data(), spec.data());

        int peak = 1;
        float peakMag = 0.0f;
        for (int k = 1; k < N / 2; ++k)
        {
            const float m = std::abs(spec[static_cast<size_t>(k)]);
            if (m > peakMag) { peakMag = m; peak = k; }
        }

        // Parabolic interpolation on log-magnitude.
        const float a = std::log(std::abs(spec[static_cast<size_t>(peak - 1)]) + 1e-12f);
        const float b = std::log(std::abs(spec[static_cast<size_t>(peak)])     + 1e-12f);
        const float c = std::log(std::abs(spec[static_cast<size_t>(peak + 1)]) + 1e-12f);
        const float denom = (a - 2 * b + c);
        const float delta = std::abs(denom) > 1e-9f ? 0.5f * (a - c) / denom : 0.0f;
        return (peak + delta) * sr / N;
    }

    double meanRms(const AudioBuffer& b)
    {
        double s = 0.0;
        for (int c = 0; c < b.numChannels(); ++c)
            s += rms(b.channel(c), b.numSamples());
        return s / std::max(1, b.numChannels());
    }
}

// ---------------------------------------------------------------------------
// Test groups.
// ---------------------------------------------------------------------------

static void testTimeStretchLength(double sr)
{
    std::printf("\n== Time-stretch: output length tracks the ratio ==\n");
    const int ch = 2, len = static_cast<int>(1.5 * sr);
    AudioBuffer in = makeSine(ch, len, 440.0, sr);

    for (double ratio : { 0.5, 0.75, 1.0, 1.5, 2.0 })
    {
        StretchParameters p;
        p.timeRatio = ratio;
        AudioBuffer out = processOffline(in, p, sr, Quality::High);

        const double expected = len * ratio;
        const double err = std::abs(out.numSamples() - expected) / expected;
        char d[128];
        std::snprintf(d, sizeof d, "ratio %.2f -> %d samples (expected ~%.0f, err %.1f%%)",
                      ratio, out.numSamples(), expected, err * 100.0);
        check(err < 0.05, "length within 5% of ratio", d);
        check(allFinite(out), "output finite", "");
    }
}

static void testPitchPreservedUnderStretch(double sr)
{
    std::printf("\n== Time-stretch preserves pitch (freq unchanged) ==\n");
    const int ch = 1, len = static_cast<int>(1.5 * sr);
    const double f0 = 440.0;
    AudioBuffer in = makeSine(ch, len, f0, sr);

    for (double ratio : { 0.5, 1.5, 2.0 })
    {
        StretchParameters p;
        p.timeRatio = ratio;
        AudioBuffer out = processOffline(in, p, sr, Quality::High);
        const double f = dominantFrequency(out.channel(0), out.numSamples(), sr);
        const double cents = 1200.0 * std::log2(f / f0);
        char d[128];
        std::snprintf(d, sizeof d, "ratio %.2f: dominant %.1f Hz (%.0f cents off %.0f Hz)", ratio, f, cents, f0);
        check(std::abs(cents) < 25.0, "pitch within 25 cents of original", d);
    }
}

static void testPitchShift(double sr)
{
    std::printf("\n== Pitch shift moves frequency, keeps length ==\n");
    const int ch = 1, len = static_cast<int>(1.5 * sr);
    const double f0 = 440.0;
    AudioBuffer in = makeSine(ch, len, f0, sr);

    struct Case { double semis; };
    for (double semis : { -12.0, -5.0, 7.0, 12.0 })
    {
        StretchParameters p;
        p.timeRatio = 1.0;
        p.setPitchSemitones(semis);
        AudioBuffer out = processOffline(in, p, sr, Quality::High);

        const double f = dominantFrequency(out.channel(0), out.numSamples(), sr);
        const double expected = f0 * std::pow(2.0, semis / 12.0);
        const double cents = 1200.0 * std::log2(f / expected);

        char d[160];
        std::snprintf(d, sizeof d, "%+.0f st: %.1f Hz (expected %.1f Hz, %.0f cents off); len %d vs %d",
                      semis, f, expected, cents, out.numSamples(), len);
        check(std::abs(cents) < 30.0, "shifted pitch within 30 cents", d);
        const double lenErr = std::abs(out.numSamples() - len) / static_cast<double>(len);
        check(lenErr < 0.05, "length unchanged within 5%", d);
        check(allFinite(out), "output finite", "");
    }
}

static void testCombinedAndEnergy(double sr)
{
    std::printf("\n== Combined time+pitch, energy roughly conserved ==\n");
    const int ch = 2, len = static_cast<int>(1.5 * sr);
    AudioBuffer in = makeSine(ch, len, 330.0, sr);
    const double inRms = meanRms(in);

    StretchParameters p;
    p.timeRatio = 1.5;
    p.setPitchSemitones(4.0);
    AudioBuffer out = processOffline(in, p, sr, Quality::High);

    const double outRms = meanRms(out);
    const double ratio = outRms / inRms;
    char d[128];
    std::snprintf(d, sizeof d, "in RMS %.3f, out RMS %.3f (ratio %.2f)", inRms, outRms, ratio);
    check(ratio > 0.6 && ratio < 1.5, "RMS preserved within ~[0.6,1.5]", d);
    check(allFinite(out), "output finite", "");
}

static void testUnityReconstruction(double sr)
{
    std::printf("\n== Unity (1x, no shift) reconstructs the tone ==\n");
    const int ch = 1, len = static_cast<int>(1.5 * sr);
    const double f0 = 523.25; // C5
    AudioBuffer in = makeSine(ch, len, f0, sr);
    const double inRms = meanRms(in);

    StretchParameters p; // all defaults => 1x, no shift
    AudioBuffer out = processOffline(in, p, sr, Quality::High);

    const double f = dominantFrequency(out.channel(0), out.numSamples(), sr);
    const double outRms = meanRms(out);
    char d[128];
    std::snprintf(d, sizeof d, "freq %.1f Hz (want %.1f); RMS %.3f vs %.3f", f, f0, outRms, inRms);
    check(std::abs(1200.0 * std::log2(f / f0)) < 15.0, "freq preserved within 15 cents", d);
    check(outRms / inRms > 0.7 && outRms / inRms < 1.3, "RMS preserved within ~[0.7,1.3]", d);
}

static void testWsolaEngine(double sr)
{
    std::printf("\n== WSOLA engine: percussive stretch, length + finiteness ==\n");
    const int ch = 2, len = static_cast<int>(1.5 * sr);
    AudioBuffer in = makeClicks(ch, len, static_cast<int>(0.20 * sr)); // 5 clicks/sec

    // Expansion (1.5x) and heavy compression (0.4x, where analysisHop > frameSize).
    for (double ratio : { 1.5, 0.4 })
    {
        StretchParameters p;
        p.timeRatio = ratio;
        p.engine = EngineType::Wsola;
        AudioBuffer out = processOffline(in, p, sr, Quality::Good);

        const double expected = len * ratio;
        const double err = std::abs(out.numSamples() - expected) / expected;
        char d[128];
        std::snprintf(d, sizeof d, "ratio %.2f: %d samples (expected ~%.0f, err %.1f%%)",
                      ratio, out.numSamples(), expected, err * 100.0);
        check(err < 0.10, "WSOLA length within 10% of ratio", d);
        check(allFinite(out), "WSOLA output finite", d);
        check(meanRms(out) > 1e-4, "WSOLA output non-silent", d);
    }
}

static void testFormantPath(double sr)
{
    std::printf("\n== Formant-preserving pitch shift runs end to end ==\n");
    const int ch = 1, len = static_cast<int>(1.5 * sr);
    AudioBuffer in = makeSine(ch, len, 220.0, sr);

    StretchParameters p;
    p.setPitchSemitones(7.0);
    p.preserveFormants = true;
    AudioBuffer out = processOffline(in, p, sr, Quality::High);

    const double f = dominantFrequency(out.channel(0), out.numSamples(), sr);
    const double expected = 220.0 * std::pow(2.0, 7.0 / 12.0);
    char d[128];
    std::snprintf(d, sizeof d, "freq %.1f Hz (expected %.1f); len %d", f, expected, out.numSamples());
    check(std::abs(1200.0 * std::log2(f / expected)) < 40.0, "formant path keeps correct pitch", d);
    check(allFinite(out), "formant output finite", "");
}

static void testStreamingPath(double sr)
{
    std::printf("\n== Streaming (block-by-block) path ==\n");
    const int ch = 2, len = static_cast<int>(1.5 * sr);
    const double f0 = 660.0;
    AudioBuffer in = makeSine(ch, len, f0, sr);

    StreamConfig cfg; cfg.sampleRate = sr; cfg.numChannels = ch; cfg.maxBlockSize = 512;
    TimeStretcher ts;
    ts.prepare(cfg);
    StretchParameters p; p.timeRatio = 2.0; p.quality = Quality::High;
    ts.setParameters(p);

    AudioBuffer out(ch, len * 3);
    std::vector<Sample*> outPtrs(static_cast<size_t>(ch));

    int inPos = 0, outPos = 0;
    std::vector<Sample*> inPtrs(static_cast<size_t>(ch));
    const int block = 512;
    while (inPos < len)
    {
        const int n = std::min(block, len - inPos);
        for (int c = 0; c < ch; ++c) inPtrs[static_cast<size_t>(c)] = in.channel(c) + inPos;
        ts.pushInput(AudioBlockView(inPtrs.data(), ch, n));
        inPos += n;

        for (int c = 0; c < ch; ++c) outPtrs[static_cast<size_t>(c)] = out.channel(c) + outPos;
        outPos += ts.receiveOutput(AudioBlockView(outPtrs.data(), ch, out.numSamples() - outPos));
    }
    ts.flush();
    for (int c = 0; c < ch; ++c) outPtrs[static_cast<size_t>(c)] = out.channel(c) + outPos;
    outPos += ts.receiveOutput(AudioBlockView(outPtrs.data(), ch, out.numSamples() - outPos));

    const double expected = len * 2.0;
    const double err = std::abs(outPos - expected) / expected;
    char d[128];
    std::snprintf(d, sizeof d, "produced %d (expected ~%.0f, err %.1f%%); engine=%s",
                  outPos, expected, err * 100.0, ts.activeEngineName());
    check(err < 0.05, "streaming length within 5%", d);

    AudioBuffer trimmed(ch, outPos);
    for (int c = 0; c < ch; ++c)
        std::copy(out.channel(c), out.channel(c) + outPos, trimmed.channel(c));
    const double f = dominantFrequency(trimmed.channel(0), outPos, sr);
    std::snprintf(d, sizeof d, "streaming dominant %.1f Hz (want %.1f)", f, f0);
    check(std::abs(1200.0 * std::log2(f / f0)) < 25.0, "streaming preserves pitch", d);
    check(allFinite(trimmed), "streaming output finite", "");
}

int main()
{
    const double sr = 44100.0;
    std::printf("Chronos test harness — sample rate %.0f Hz\n", sr);

    testTimeStretchLength(sr);
    testPitchPreservedUnderStretch(sr);
    testPitchShift(sr);
    testCombinedAndEnergy(sr);
    testUnityReconstruction(sr);
    testWsolaEngine(sr);
    testFormantPath(sr);
    testStreamingPath(sr);

    std::printf("\n----------------------------------------\n");
    std::printf("%d checks, %d failures\n", gChecks, gFailures);
    std::printf("%s\n", gFailures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return gFailures == 0 ? 0 : 1;
}
