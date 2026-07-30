// Chronos — engine/WsolaEngine.cpp

#include "WsolaEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../dsp/MathUtils.h"

namespace chronos::engine
{

void WsolaEngine::prepare(const EngineConfig& cfg)
{
    config = cfg;

    // WSOLA is the percussive/transient engine; small segments keep attacks sharp
    // AND keep the O(frameSize^2) similarity search cheap, so we deliberately do NOT
    // grow the frame at higher quality tiers (large frames would smear transients).
    switch (cfg.quality)
    {
        case Quality::Draft: frameSize = 512;  break;
        case Quality::Good:  frameSize = 1024; break;
        case Quality::High:  frameSize = 1024; break;
        case Quality::Best:  frameSize = 1024; break;
    }
    synthHop  = frameSize / 2;
    overlap   = frameSize - synthHop;
    tolerance = frameSize / 4;

    window.design(frameSize, dsp::WindowType::Hann, 0);

    const int ch = cfg.numChannels;

    // Worst-case nominal advance per segment is Hs/minStretch; the look-ahead the
    // similarity search needs on top is tolerance + frameSize.
    const int haMax = static_cast<int>(std::ceil(synthHop / minStretch));
    workLen = tolerance + frameSize + tolerance + 4;
    inputFifo.prepare(ch, cfg.maxBlockSize + haMax + workLen + 8);

    const int outCap = static_cast<int>(std::ceil(cfg.maxBlockSize * maxStretch)) + frameSize * 2 + 8;
    outputFifo.prepare(ch, outCap);

    workWindow.assign(static_cast<size_t>(ch), std::vector<Sample>(static_cast<size_t>(workLen), Sample{0}));
    workPtrs.resize(static_cast<size_t>(ch));
    for (int c = 0; c < ch; ++c)
        workPtrs[static_cast<size_t>(c)] = workWindow[static_cast<size_t>(c)].data();

    templateSeg.assign(static_cast<size_t>(overlap), Sample{0});
    onsetPrefix.assign(static_cast<size_t>(workLen + 1), 0.0);

    olaLength = frameSize + synthHop + 8;
    olaBuffer.assign(static_cast<size_t>(ch), std::vector<Sample>(static_cast<size_t>(olaLength), Sample{0}));
    normBuffer.assign(static_cast<size_t>(olaLength), Sample{0});
    normSqBuffer.assign(static_cast<size_t>(olaLength), Sample{0});

    emitScratch.assign(static_cast<size_t>(ch), std::vector<Sample>(static_cast<size_t>(synthHop), Sample{0}));
    emitPtrs.resize(static_cast<size_t>(ch));
    for (int c = 0; c < ch; ++c)
        emitPtrs[static_cast<size_t>(c)] = emitScratch[static_cast<size_t>(c)].data();

    reset();
}

void WsolaEngine::reset()
{
    inputFifo.reset();
    outputFifo.reset();
    for (auto& b : olaBuffer)
        std::fill(b.begin(), b.end(), Sample{0});
    std::fill(normBuffer.begin(), normBuffer.end(), Sample{0});
    std::fill(normSqBuffer.begin(), normSqBuffer.end(), Sample{0});
    std::fill(templateSeg.begin(), templateSeg.end(), Sample{0});
    // Start the nominal position one tolerance in so the first search can look back.
    nominalPos = tolerance;
    hasTemplate = false;
    templateEnergy = 0.0f;
    lastCorr = 1.0f;
    onsetHold = 0;
}

void WsolaEngine::setTimeStretch(double ratio)
{
    timeStretch = clamp(ratio, minStretch, maxStretch);
    analysisHop = synthHop / timeStretch;   // Ha = Hs / alpha
}

void WsolaEngine::setPitchContext(double, const StretchParameters&)
{
    // WSOLA is time-domain and does not model formants; the facade avoids selecting
    // it when formant processing is requested. Nothing to configure here.
}

int WsolaEngine::feed(const Sample* const* input, int offset, int numFrames)
{
    const int accepted = std::min(numFrames, inputFifo.freeSpace());
    if (accepted > 0)
        inputFifo.write(input, offset, accepted);
    processSegments();
    return accepted;
}

int WsolaEngine::read(Sample* const* output, int offset, int numFrames)
{
    return outputFifo.read(output, offset, numFrames);
}

void WsolaEngine::processSegments()
{
    const int ch = config.numChannels;

    while (true)
    {
        const int start = static_cast<int>(std::lround(nominalPos));

        // Two requirements:
        //  * enough input to search + extract: indices up to start+tolerance+frameSize;
        //  * enough input to then retire ~analysisHop samples (large under heavy
        //    compression, where analysisHop can exceed the frame) and still leave a
        //    full search window for the next segment.
        const int need = start + tolerance + frameSize;
        const int discardAhead = static_cast<int>(std::ceil(analysisHop)) + 2;
        if (inputFifo.numReady() < need + discardAhead || outputFifo.freeSpace() < synthHop)
            break;

        // Peek the working span once for all channels.
        const int span = std::min(need, workLen);
        inputFifo.peek(workPtrs.data(), 0, span);

        // Onset-locked placement: if a sharp attack sits within the timing tolerance,
        // choose the extraction offset so the attack lands at the segment's Hann
        // CENTRE (gain 1, neighbours ~0) — copied verbatim and sample-sharp — instead
        // of under a window taper where it would be softened. A segment placed at
        // `chosen` centres input index `chosen + frameSize/2`, so we look for onsets
        // in [start-tol, start+tol] + frameSize/2 and keep `chosen` within tolerance.
        int chosen;
        const int onLo = std::max(frameSize / 4, start - tolerance + frameSize / 2);
        const int onHi = std::min(span - frameSize / 4, start + tolerance + frameSize / 2);
        const int onsetIdx = (onsetHold == 0) ? findOnset(onLo, onHi) : -1;
        if (onsetIdx >= 0)
        {
            chosen = clamp(onsetIdx - frameSize / 2,
                           std::max(0, start - tolerance),
                           std::min(span - frameSize, start + tolerance));
            onsetHold = 1;   // don't re-lock the same attack on the very next segment
        }
        else
        {
            const int delta = hasTemplate ? findBestOffset(start) : 0;
            chosen = clamp(start + delta, 0, span - frameSize);
        }
        if (onsetHold > 0)
            --onsetHold;

        placeSegment(chosen);

        // Save the natural-continuation template (mono average) for the next search:
        // the samples that *follow* this segment by one synthesis hop.
        const int tstart = chosen + synthHop;
        float tEnergy = 0.0f;
        for (int n = 0; n < overlap; ++n)
        {
            float acc = 0.0f;
            for (int c = 0; c < ch; ++c)
                acc += workWindow[static_cast<size_t>(c)][static_cast<size_t>(tstart + n)];
            const float v = acc / static_cast<float>(ch);
            templateSeg[static_cast<size_t>(n)] = v;
            tEnergy += v * v;
        }
        templateEnergy = tEnergy;
        hasTemplate = true;

        // Advance nominal input position and retire consumed input, keeping one
        // tolerance of history so the next search can look back.
        nominalPos += analysisHop;
        int discard = static_cast<int>(std::floor(nominalPos)) - tolerance;
        if (discard > 0)
        {
            inputFifo.discard(discard);
            nominalPos -= discard;
        }
    }
}

int WsolaEngine::findBestOffset(int nominalStart)
{
    const int ch = config.numChannels;

    // Normalised cross-correlation between a candidate segment's overlap region and
    // the saved continuation template (mono average, so L/R stay coherent). The
    // score surface is smooth, so we search coarse-to-fine: a strided coarse pass
    // then a ±stride fine pass around the best — ~stride× fewer O(overlap) scores
    // than an exhaustive scan, at no audible cost.
    auto score = [&](int delta) -> float {
        const int s = nominalStart + delta;
        if (s < 0) return -1.0e30f;
        float dot = 0.0f, energy = 0.0f;
        for (int n = 0; n < overlap; ++n)
        {
            float cand = 0.0f;
            for (int c = 0; c < ch; ++c)
                cand += workWindow[static_cast<size_t>(c)][static_cast<size_t>(s + n)];
            cand /= static_cast<float>(ch);
            dot    += cand * templateSeg[static_cast<size_t>(n)];
            energy += cand * cand;
        }
        return dot / std::sqrt(energy + 1.0e-9f);
    };

    constexpr int stride = 2;
    float bestScore = -1.0e30f;
    int   bestDelta = 0;
    for (int delta = -tolerance; delta <= tolerance; delta += stride)
    {
        const float sc = score(delta);
        if (sc > bestScore) { bestScore = sc; bestDelta = delta; }
    }
    const int lo = std::max(-tolerance, bestDelta - stride + 1);
    const int hi = std::min(tolerance, bestDelta + stride - 1);
    for (int delta = lo; delta <= hi; ++delta)
    {
        const float sc = score(delta);
        if (sc > bestScore) { bestScore = sc; bestDelta = delta; }
    }

    // How well the chosen segment actually matches, as a correlation in [0,1]. The
    // overlap-add normalisation depends on it: two identical segments add in
    // amplitude, two unrelated ones only in power (see placeSegment).
    lastCorr = clamp(bestScore / (std::sqrt(templateEnergy) + 1.0e-9f), 0.0f, 1.0f);
    return bestDelta;
}

int WsolaEngine::findOnset(int lo, int hi)
{
    // Strongest energy jump (short leading vs trailing window) over [lo, hi), using a
    // prefix-sum of the mono energy so each candidate is O(1) — the whole scan is
    // O(hi-lo), not O((hi-lo)*w). Reuses the already-peeked workWindow.
    const int ch = config.numChannels;
    const int w = 64;              // ~1.5 ms energy windows
    if (hi <= lo + w || lo < w) return -1;

    const int a = lo - w;          // prefix covers [a, hi+w)
    const int end = hi + w;
    onsetPrefix[0] = 0.0;
    for (int k = a; k < end; ++k)
    {
        float s = 0.0f;
        for (int c = 0; c < ch; ++c) s += workWindow[static_cast<size_t>(c)][static_cast<size_t>(k)];
        onsetPrefix[static_cast<size_t>(k - a + 1)] = onsetPrefix[static_cast<size_t>(k - a)] + static_cast<double>(s) * s;
    }

    int best = -1;
    double bestRatio = 3.0;        // require a >3x energy jump to count as an onset
    for (int i = lo; i < hi; ++i)
    {
        const int r = i - a;
        const double before = onsetPrefix[static_cast<size_t>(r)]     - onsetPrefix[static_cast<size_t>(r - w)] + 1e-9;
        const double after  = onsetPrefix[static_cast<size_t>(r + w)] - onsetPrefix[static_cast<size_t>(r)]     + 1e-9;
        const double ratio = after / before;
        if (ratio > bestRatio) { bestRatio = ratio; best = i; }
    }
    return best;
}

void WsolaEngine::placeSegment(int chosenStart)
{
    const int ch = config.numChannels;
    const Sample* win = window.data();

    for (int c = 0; c < ch; ++c)
    {
        const Sample* src = workWindow[static_cast<size_t>(c)].data() + chosenStart;
        Sample* ola = olaBuffer[static_cast<size_t>(c)].data();
        for (int n = 0; n < frameSize; ++n)
            ola[n] += src[n] * win[n];
    }
    for (int n = 0; n < frameSize; ++n)
    {
        normBuffer[static_cast<size_t>(n)]   += win[n];
        normSqBuffer[static_cast<size_t>(n)] += win[n] * win[n];
    }

    // Emit one synthesis hop of normalised output and slide the accumulator.
    //
    // Overlapping segments only add in amplitude when they actually match. The
    // similarity search maximises that match but never reaches it exactly, and the
    // residual mismatch makes the overlap add partly in POWER instead — which is
    // why plain 1/sum(w) normalisation leaves WSOLA about a dB quiet on dense
    // material. Blend the two normalisers by the similarity actually achieved:
    // rho = 1 gives sum(w) (coherent), rho = 0 gives sqrt(sum(w^2)) (incoherent).
    const float rho = lastCorr;
    for (int n = 0; n < synthHop; ++n)
    {
        const float norm   = normBuffer[static_cast<size_t>(n)];
        const float normSq = normSqBuffer[static_cast<size_t>(n)];
        const float blended = std::sqrt(rho * norm * norm + (1.0f - rho) * normSq);
        const float gain = blended > 1.0e-6f ? 1.0f / blended : 0.0f;
        for (int c = 0; c < ch; ++c)
            emitScratch[static_cast<size_t>(c)][static_cast<size_t>(n)] =
                olaBuffer[static_cast<size_t>(c)][static_cast<size_t>(n)] * gain;
    }
    outputFifo.write(emitPtrs.data(), 0, synthHop);

    const int keep = olaLength - synthHop;
    for (int c = 0; c < ch; ++c)
    {
        Sample* ola = olaBuffer[static_cast<size_t>(c)].data();
        std::memmove(ola, ola + synthHop, static_cast<size_t>(keep) * sizeof(Sample));
        std::memset(ola + keep, 0, static_cast<size_t>(synthHop) * sizeof(Sample));
    }
    std::memmove(normBuffer.data(), normBuffer.data() + synthHop, static_cast<size_t>(keep) * sizeof(Sample));
    std::memset(normBuffer.data() + keep, 0, static_cast<size_t>(synthHop) * sizeof(Sample));
    std::memmove(normSqBuffer.data(), normSqBuffer.data() + synthHop, static_cast<size_t>(keep) * sizeof(Sample));
    std::memset(normSqBuffer.data() + keep, 0, static_cast<size_t>(synthHop) * sizeof(Sample));
}

} // namespace chronos::engine
