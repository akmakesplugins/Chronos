// Chronos — engine/SinusoidalEngine.cpp

#include "SinusoidalEngine.h"

#include <algorithm>
#include <cmath>

#include "../dsp/MathUtils.h"

namespace chronos::engine
{

using dsp::Complex;
using dsp::kTwoPi;

namespace
{
    struct QualitySpec { int frameSize; int overlap; int tracks; };
    QualitySpec specFor(Quality q) noexcept
    {
        switch (q)
        {
            case Quality::Draft: return { 1024, 4,  48 };
            case Quality::Good:  return { 2048, 4,  96 };
            case Quality::High:  return { 2048, 8, 128 };
            case Quality::Best:  return { 4096, 8, 192 };
        }
        return { 2048, 4, 96 };
    }
}

void SinusoidalEngine::prepare(const EngineConfig& cfg)
{
    config = cfg;
    numCh = cfg.numChannels;

    const QualitySpec spec = specFor(cfg.quality);
    frameSize     = spec.frameSize;
    overlapFactor = spec.overlap;
    analysisHop   = frameSize / overlapFactor;
    halfBins      = frameSize / 2 + 1;
    maxTracks     = spec.tracks;
    maxPeaks      = spec.tracks;

    // Synthesis is contiguous (no overlap constraint), so we can stretch far beyond
    // the overlap factor. Size buffers for the full advertised range.
    maxTimeStretch = 20.0;
    maxSynthHop = static_cast<int>(std::ceil(analysisHop * maxTimeStretch)) + 1;

    fft.setSize(frameSize);
    analysisWindow.design(frameSize, dsp::WindowType::Hann, 0);   // raw window (like PV)

    // Oscillator amplitude from a windowed main-lobe peak: |X[k0]| ≈ (A/2)·Σw, so
    // A ≈ 2·|X| / Σw. Measured from the coefficients to stay window-agnostic.
    double windowSum = 0.0;
    for (int n = 0; n < frameSize; ++n) windowSum += analysisWindow[n];
    sinAmpScale = static_cast<float>(2.0 / windowSum);

    transientDetector.prepare(halfBins, cfg.sampleRate, analysisHop);
    envelopes.resize(static_cast<size_t>(numCh));
    for (auto& e : envelopes) e.prepare(frameSize, &fft);

    inputFifo.prepare(numCh, frameSize + cfg.maxBlockSize + analysisHop + 8);
    outputFifo.prepare(numCh, static_cast<int>(std::ceil(cfg.maxBlockSize * maxTimeStretch)) + maxSynthHop + 8);

    spectrum.assign(static_cast<size_t>(numCh), std::vector<Complex>(static_cast<size_t>(frameSize)));
    magnitude.assign(static_cast<size_t>(numCh), std::vector<float>(static_cast<size_t>(halfBins)));
    detectMag.assign(static_cast<size_t>(halfBins), 0.0f);

    peakFreq.assign(static_cast<size_t>(maxPeaks), 0.0f);
    peakBin.assign(static_cast<size_t>(maxPeaks), 0);
    peakAmpCh.assign(static_cast<size_t>(maxPeaks * numCh), 0.0f);
    peakPhaseCh.assign(static_cast<size_t>(maxPeaks * numCh), 0.0f);
    peakOrder.assign(static_cast<size_t>(maxPeaks), 0);
    peakMatched.assign(static_cast<size_t>(maxPeaks), -1);

    tracks.assign(static_cast<size_t>(maxTracks), Track{ false, 0.0f });
    trackPhase.assign(static_cast<size_t>(maxTracks * numCh), 0.0);
    trackAmp.assign(static_cast<size_t>(maxTracks * numCh), 0.0f);
    trackMatch.assign(static_cast<size_t>(maxTracks), -1);
    activeIdx.assign(static_cast<size_t>(maxTracks), 0);
    freeList.assign(static_cast<size_t>(maxTracks), 0);

    sineTable.assign(static_cast<size_t>(kSineTableSize + 1), 0.0f);
    for (int i = 0; i <= kSineTableSize; ++i)
        sineTable[static_cast<size_t>(i)] = static_cast<float>(std::sin(kTwoPi * i / kSineTableSize));

    sineOut.assign(static_cast<size_t>(numCh), std::vector<Sample>(static_cast<size_t>(maxSynthHop), Sample{0}));
    frameScratch.assign(static_cast<size_t>(numCh), std::vector<Sample>(static_cast<size_t>(frameSize), Sample{0}));
    sineOutPtrs.resize(static_cast<size_t>(numCh));
    framePtrs.resize(static_cast<size_t>(numCh));
    for (int c = 0; c < numCh; ++c)
    {
        sineOutPtrs[static_cast<size_t>(c)] = sineOut[static_cast<size_t>(c)].data();
        framePtrs[static_cast<size_t>(c)]   = frameScratch[static_cast<size_t>(c)].data();
    }

    reset();
}

void SinusoidalEngine::reset()
{
    inputFifo.reset();
    outputFifo.reset();
    transientDetector.reset();
    for (auto& e : envelopes) e.reset();

    for (auto& t : tracks) t = Track{ false, 0.0f };
    std::fill(trackPhase.begin(), trackPhase.end(), 0.0);
    std::fill(trackAmp.begin(), trackAmp.end(), 0.0f);
    for (int i = 0; i < maxTracks; ++i) freeList[static_cast<size_t>(i)] = i;
    freeTop = maxTracks;
    numActive = 0;
    hopAccum = 0.0;
    primed = false;
}

void SinusoidalEngine::setTimeStretch(double ratio)
{
    timeStretch = clamp(ratio, 0.05, maxTimeStretch);
}

void SinusoidalEngine::setPitchContext(double pitch, const StretchParameters& params)
{
    pitchRatio    = pitch;
    formantActive = params.preserveFormants || std::abs(params.formantRatio - 1.0) > 1.0e-6;
    formantScale  = (params.preserveFormants ? 1.0 : pitch) * params.formantRatio;
    if (formantScale < 1.0e-6) formantScale = 1.0e-6;
}

int SinusoidalEngine::feed(const Sample* const* input, int offset, int numFrames)
{
    const int accepted = std::min(numFrames, inputFifo.freeSpace());
    if (accepted > 0) inputFifo.write(input, offset, accepted);
    processFrames();
    return accepted;
}

int SinusoidalEngine::read(Sample* const* output, int offset, int numFrames)
{
    return outputFifo.read(output, offset, numFrames);
}

void SinusoidalEngine::processFrames()
{
    while (inputFifo.numReady() >= frameSize && outputFifo.freeSpace() >= maxSynthHop)
    {
        inputFifo.peek(framePtrs.data(), 0, frameSize);
        processOneFrame();
        inputFifo.discard(analysisHop);
    }
}

float SinusoidalEngine::sineLookup(double phase) const noexcept
{
    // phase assumed already wrapped to [0, 2*pi).
    const double x = phase * (kSineTableSize / kTwoPi);
    const int i = static_cast<int>(x);
    const float frac = static_cast<float>(x - i);
    const float a = sineTable[static_cast<size_t>(i)];
    const float b = sineTable[static_cast<size_t>(i + 1)];
    return a + frac * (b - a);
}

void SinusoidalEngine::processOneFrame()
{
    // Integer synthesis hop with fractional carry (keeps output duration exact and
    // avoids per-frame phase glitches).
    hopAccum += analysisHop * timeStretch;
    int hop = static_cast<int>(std::floor(hopAccum));
    hopAccum -= hop;
    if (hop > maxSynthHop) hop = maxSynthHop;

    // Analysis.
    std::fill(detectMag.begin(), detectMag.end(), 0.0f);
    for (int c = 0; c < numCh; ++c)
    {
        Sample* frame = frameScratch[static_cast<size_t>(c)].data();
        analysisWindow.apply(frame);
        fft.forwardReal(frame, spectrum[static_cast<size_t>(c)].data());
        auto& mag = magnitude[static_cast<size_t>(c)];
        const Complex* spec = spectrum[static_cast<size_t>(c)].data();
        for (int k = 0; k < halfBins; ++k)
        {
            mag[static_cast<size_t>(k)] = std::abs(spec[k]);
            detectMag[static_cast<size_t>(k)] += mag[static_cast<size_t>(k)];
        }
        if (formantActive) envelopes[static_cast<size_t>(c)].compute(spec);
    }

    (void) transientDetector.process(detectMag.data(), halfBins);

    pickPeaks();
    matchTracks();
    renderPartials(hop);

    if (hop > 0)
        outputFifo.write(sineOutPtrs.data(), 0, hop);

    primed = true;
}

void SinusoidalEngine::pickPeaks()
{
    numPeaks = 0;

    float maxDetect = 0.0f;
    for (int k = 0; k < halfBins; ++k) maxDetect = std::max(maxDetect, detectMag[static_cast<size_t>(k)]);
    const float floor = maxDetect * kFloorRel + 1.0e-9f;

    for (int k = 2; k < halfBins - 2; ++k)
    {
        const float m = detectMag[static_cast<size_t>(k)];
        if (m <= floor) continue;
        if (m <= detectMag[static_cast<size_t>(k - 1)] || m < detectMag[static_cast<size_t>(k + 1)]) continue;
        if (m <= detectMag[static_cast<size_t>(k - 2)] || m < detectMag[static_cast<size_t>(k + 2)]) continue;

        // Parabolic interpolation on log-magnitude of the SHARED (summed) spectrum.
        const float a = std::log(detectMag[static_cast<size_t>(k - 1)] + 1.0e-12f);
        const float b = std::log(m + 1.0e-12f);
        const float cc = std::log(detectMag[static_cast<size_t>(k + 1)] + 1.0e-12f);
        const float denom = a - 2.0f * b + cc;
        float delta = std::abs(denom) > 1.0e-12f ? 0.5f * (a - cc) / denom : 0.0f;
        delta = clamp(delta, -0.5f, 0.5f);

        // Peaks are discovered in ascending-frequency order; appending keeps the
        // array sorted (the matcher relies on this). If we hit capacity we stop —
        // dropping the highest, generally quieter partials, which is acceptable and
        // avoids any reordering.
        if (numPeaks >= maxPeaks)
            break;

        const int idx = numPeaks++;
        peakBin[static_cast<size_t>(idx)] = k;
        peakFreq[static_cast<size_t>(idx)] = static_cast<float>(kTwoPi * (k + delta) / frameSize);

        for (int c = 0; c < numCh; ++c)
        {
            const auto& mg = magnitude[static_cast<size_t>(c)];
            const float la = std::log(mg[static_cast<size_t>(k - 1)] + 1.0e-12f);
            const float lb = std::log(mg[static_cast<size_t>(k)] + 1.0e-12f);
            const float lc = std::log(mg[static_cast<size_t>(k + 1)] + 1.0e-12f);
            const float peakLog = lb - 0.25f * (la - lc) * delta;   // parabola-corrected peak
            peakAmpCh[static_cast<size_t>(idx * numCh + c)] = std::exp(peakLog) * sinAmpScale;
            peakPhaseCh[static_cast<size_t>(idx * numCh + c)] = std::arg(spectrum[static_cast<size_t>(c)][k]);
        }
    }
}

void SinusoidalEngine::matchTracks()
{
    // Collect active tracks and sort by frequency ascending.
    numActive = 0;
    for (int t = 0; t < maxTracks; ++t)
        if (tracks[static_cast<size_t>(t)].active)
            activeIdx[static_cast<size_t>(numActive++)] = t;
    std::sort(activeIdx.begin(), activeIdx.begin() + numActive,
              [this](int x, int y) { return tracks[static_cast<size_t>(x)].freq < tracks[static_cast<size_t>(y)].freq; });

    for (int p = 0; p < numPeaks; ++p) peakMatched[static_cast<size_t>(p)] = -1;
    for (int a = 0; a < numActive; ++a) trackMatch[static_cast<size_t>(activeIdx[static_cast<size_t>(a)])] = -1;

    const float tol = std::max(kMatchBins * static_cast<float>(kTwoPi) / frameSize, 0.03f);

    // McAulay–Quatieri matching: for each track (ascending), claim the nearest
    // unclaimed peak within tolerance, unless a later track is a closer owner.
    for (int a = 0; a < numActive; ++a)
    {
        const int t = activeIdx[static_cast<size_t>(a)];
        const float f = tracks[static_cast<size_t>(t)].freq;

        int best = -1; float bestD = tol;
        for (int p = 0; p < numPeaks; ++p)
        {
            if (peakMatched[static_cast<size_t>(p)] != -1) continue;
            const float d = std::abs(peakFreq[static_cast<size_t>(p)] - f);
            if (d <= bestD) { bestD = d; best = p; }
        }
        if (best < 0) continue;   // dies

        // Conflict: does the next track own `best` more strongly?
        if (a + 1 < numActive)
        {
            const float fNext = tracks[static_cast<size_t>(activeIdx[static_cast<size_t>(a + 1)])].freq;
            if (std::abs(peakFreq[static_cast<size_t>(best)] - fNext) < bestD)
            {
                // Try the next-lower unclaimed peak within tolerance instead.
                int alt = -1; float altD = tol;
                for (int p = 0; p < best; ++p)
                {
                    if (peakMatched[static_cast<size_t>(p)] != -1) continue;
                    const float d = std::abs(peakFreq[static_cast<size_t>(p)] - f);
                    if (d <= altD) { altD = d; alt = p; }
                }
                best = alt;
            }
        }
        if (best >= 0)
        {
            trackMatch[static_cast<size_t>(t)] = best;
            peakMatched[static_cast<size_t>(best)] = t;
        }
    }
}

void SinusoidalEngine::renderPartials(int hop)
{
    for (int c = 0; c < numCh; ++c)
        std::fill(sineOut[static_cast<size_t>(c)].begin(), sineOut[static_cast<size_t>(c)].begin() + hop, Sample{0});

    if (hop <= 0)
    {
        // Still must advance births/deaths so the track table stays consistent.
    }

    const float invHop = hop > 0 ? 1.0f / static_cast<float>(hop) : 0.0f;

    // Continue / kill existing tracks.
    for (int a = 0; a < numActive; ++a)
    {
        const int t = activeIdx[static_cast<size_t>(a)];
        const int p = trackMatch[static_cast<size_t>(t)];
        const float w0 = tracks[static_cast<size_t>(t)].freq;
        const float w1 = (p >= 0) ? peakFreq[static_cast<size_t>(p)] : w0;      // death holds freq

        for (int c = 0; c < numCh; ++c)
        {
            const float a0 = trackAmp[static_cast<size_t>(t * numCh + c)];
            const float a1 = (p >= 0) ? peakAmpCh[static_cast<size_t>(p * numCh + c)] : 0.0f;  // death fades to 0
            const float g = (w1 - w0) * invHop;
            const float ampInc = (a1 - a0) * invHop;

            double th = trackPhase[static_cast<size_t>(t * numCh + c)];
            double d = static_cast<double>(w0) + 0.5 * g;
            float amp = a0;
            Sample* out = sineOut[static_cast<size_t>(c)].data();
            for (int n = 0; n < hop; ++n)
            {
                out[n] += amp * sineLookup(th);
                th += d;
                if (th >= kTwoPi) th -= kTwoPi; else if (th < 0.0) th += kTwoPi;
                d += g;
                amp += ampInc;
            }
            trackPhase[static_cast<size_t>(t * numCh + c)] = th;
            trackAmp[static_cast<size_t>(t * numCh + c)] = a1;
        }

        tracks[static_cast<size_t>(t)].freq = w1;
        if (p < 0)
        {
            tracks[static_cast<size_t>(t)].active = false;
            freeList[static_cast<size_t>(freeTop++)] = t;
        }
    }

    // Births: unmatched peaks start new tracks, amplitude 0 -> A1 (no zipper).
    for (int p = 0; p < numPeaks; ++p)
    {
        if (peakMatched[static_cast<size_t>(p)] != -1) continue;
        if (freeTop <= 0) break;   // over capacity — drop (bounded)
        const int t = freeList[static_cast<size_t>(--freeTop)];
        tracks[static_cast<size_t>(t)] = Track{ true, peakFreq[static_cast<size_t>(p)] };
        const float w = peakFreq[static_cast<size_t>(p)];
        for (int c = 0; c < numCh; ++c)
        {
            const float a1 = peakAmpCh[static_cast<size_t>(p * numCh + c)];
            const float ampInc = a1 * invHop;
            double th = static_cast<double>(peakPhaseCh[static_cast<size_t>(p * numCh + c)]);
            if (th < 0.0) th += kTwoPi;
            float amp = 0.0f;
            Sample* out = sineOut[static_cast<size_t>(c)].data();
            for (int n = 0; n < hop; ++n)
            {
                out[n] += amp * sineLookup(th);
                th += w;
                if (th >= kTwoPi) th -= kTwoPi; else if (th < 0.0) th += kTwoPi;
                amp += ampInc;
            }
            trackPhase[static_cast<size_t>(t * numCh + c)] = th;
            trackAmp[static_cast<size_t>(t * numCh + c)] = a1;
        }
    }
}

} // namespace chronos::engine
