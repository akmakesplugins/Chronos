// Copyright (c) 2026 ZROLogic. All rights reserved. Proprietary and confidential.
// Chronos — engine/PhaseVocoderEngine.cpp

#include "PhaseVocoderEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../dsp/MathUtils.h"

namespace chronos::engine
{

using dsp::Complex;
using dsp::kTwoPi;
using dsp::wrapPhase;

namespace
{
    struct QualitySpec { int frameSize; int overlap; bool phaseLock; };

    QualitySpec specFor(Quality q) noexcept
    {
        switch (q)
        {
            case Quality::Draft: return { 1024, 4, false };
            case Quality::Good:  return { 2048, 4, true  };
            case Quality::High:  return { 2048, 8, true  };
            case Quality::Best:  return { 4096, 8, true  };
        }
        return { 2048, 4, true };
    }
}

void PhaseVocoderEngine::prepare(const EngineConfig& cfg)
{
    config = cfg;

    const QualitySpec spec = specFor(cfg.quality);
    frameSize        = spec.frameSize;
    overlapFactor    = spec.overlap;
    analysisHop      = frameSize / overlapFactor;
    halfBins         = frameSize / 2 + 1;
    phaseLockEnabled = spec.phaseLock;

    // The synthesis hop is analysisHop * timeStretch; to keep frames overlapping
    // (no gaps) it must stay below frameSize, capping the stretch at the overlap
    // factor. Leave a small margin.
    maxTimeStretch = overlapFactor * 0.95;
    const int maxSynthHop = static_cast<int>(std::ceil(analysisHop * maxTimeStretch)) + 1;

    fft.setSize(frameSize);
    analysisWindow.design(frameSize, dsp::WindowType::Hann, 0);   // no COLA baking:
    synthesisWindow.design(frameSize, dsp::WindowType::Hann, 0);  // normBuffer handles gain

    transientDetector.prepare(halfBins, cfg.sampleRate, analysisHop);
    transientDetector.setSensitivity(0.5f);

    const int ch = cfg.numChannels;

    envelopes.resize(static_cast<size_t>(ch));
    for (auto& e : envelopes)
        e.prepare(frameSize, &fft);

    // Input FIFO must hold a near-full frame plus one host block; output FIFO must
    // hold the largest burst a single feed() can synthesise.
    inputFifo.prepare(ch, frameSize + cfg.maxBlockSize + analysisHop + 8);
    const int outCap = static_cast<int>(std::ceil(cfg.maxBlockSize * maxTimeStretch)) + frameSize + maxSynthHop + 8;
    outputFifo.prepare(ch, outCap);

    spectrum.assign(static_cast<size_t>(ch), std::vector<Complex>(static_cast<size_t>(frameSize)));
    magnitude.assign(static_cast<size_t>(ch), std::vector<float>(static_cast<size_t>(halfBins)));
    analysisPhase.assign(static_cast<size_t>(ch), std::vector<float>(static_cast<size_t>(halfBins)));
    monoSpec.assign(static_cast<size_t>(halfBins), Complex{});
    aphMono.assign(static_cast<size_t>(halfBins), 0.0f);
    prevAphMono.assign(static_cast<size_t>(halfBins), 0.0f);
    sharedSynthPhase.assign(static_cast<size_t>(halfBins), 0.0f);

    trueFreq.assign(static_cast<size_t>(halfBins), 0.0f);
    detectMag.assign(static_cast<size_t>(halfBins), 0.0f);
    noiseBin.assign(static_cast<size_t>(halfBins), std::uint8_t{0});
    randOffset.assign(static_cast<size_t>(halfBins), 0.0f);
    peakOf.assign(static_cast<size_t>(halfBins), 0);
    peaks.clear();
    peaks.reserve(static_cast<size_t>(halfBins));   // ensures push_back never allocates in process()

    olaLength = frameSize + maxSynthHop + 8;
    olaBuffer.assign(static_cast<size_t>(ch), std::vector<Sample>(static_cast<size_t>(olaLength), Sample{0}));
    normBuffer.assign(static_cast<size_t>(olaLength), Sample{0});
    normFullBuffer.assign(static_cast<size_t>(olaLength), Sample{0});

    // Transient machinery. The splice covers the window-taper-weak first moments of
    // an attack; the pre-roll gives the crossfade somewhere to start.
    refractorySamples = static_cast<int>(0.040 * cfg.sampleRate);
    unityFrames       = std::max(1, static_cast<int>(std::lround(0.012 * cfg.sampleRate / analysisHop)));
    splicePreRoll     = static_cast<int>(0.003 * cfg.sampleRate);
    spliceLen         = splicePreRoll + static_cast<int>(0.025 * cfg.sampleRate);
    spliceFadeIn      = std::max(1, static_cast<int>(0.0015 * cfg.sampleRate));
    spliceHold        = splicePreRoll + static_cast<int>(0.012 * cfg.sampleRate);
    if (spliceLen > frameSize)          // tiny frames: keep the splice inside one frame
    {
        spliceLen  = frameSize;
        spliceHold = std::min(spliceHold, spliceLen - 1);
    }
    for (auto& slot : spliceBufs)
        slot.assign(static_cast<size_t>(ch), std::vector<Sample>(static_cast<size_t>(spliceLen), Sample{0}));
    monoPrefix.assign(static_cast<size_t>(frameSize) + 1, 0.0);

    frameScratch.assign(static_cast<size_t>(ch), std::vector<Sample>(static_cast<size_t>(frameSize), Sample{0}));
    emitScratch.assign(static_cast<size_t>(ch), std::vector<Sample>(static_cast<size_t>(maxSynthHop), Sample{0}));
    framePtrs.resize(static_cast<size_t>(ch));
    emitPtrs.resize(static_cast<size_t>(ch));
    for (int c = 0; c < ch; ++c)
    {
        framePtrs[static_cast<size_t>(c)] = frameScratch[static_cast<size_t>(c)].data();
        emitPtrs[static_cast<size_t>(c)]  = emitScratch[static_cast<size_t>(c)].data();
    }

    reset();
}

void PhaseVocoderEngine::reset()
{
    inputFifo.reset();
    outputFifo.reset();
    for (auto& e : envelopes)
        e.reset();

    std::fill(prevAphMono.begin(), prevAphMono.end(), 0.0f);
    std::fill(sharedSynthPhase.begin(), sharedSynthPhase.end(), 0.0f);
    for (int c = 0; c < config.numChannels; ++c)
        std::fill(olaBuffer[static_cast<size_t>(c)].begin(), olaBuffer[static_cast<size_t>(c)].end(), Sample{0});
    std::fill(normBuffer.begin(), normBuffer.end(), Sample{0});
    std::fill(normFullBuffer.begin(), normFullBuffer.end(), Sample{0});
    std::fill(noiseBin.begin(), noiseBin.end(), std::uint8_t{0});
    hopRemainder = 0.0;
    frameRandomize = false;
    compGain = 1.0f;
    rngState = 0x9e3779b9u;      // deterministic: the same input always renders the same
    primed = false;

    inputAbs = 0;
    outEmittedAbs = 0;
    prevHopIn = 0;
    prevHopOut = 0.0;
    debtOut = 0.0;
    lastOnsetAbs = -1;
    unityLeft = 0;
    onsetCount = 0;
    for (auto& sp : splices)
        sp = Splice{};
}

void PhaseVocoderEngine::setTimeStretch(double ratio)
{
    timeStretch = clamp(ratio, 0.1, maxTimeStretch);
}

void PhaseVocoderEngine::setPitchContext(double pitch, const StretchParameters& params)
{
    pitchRatio    = pitch;
    formantActive = params.preserveFormants || std::abs(params.formantRatio - 1.0) > 1.0e-6;
    formantScale  = (params.preserveFormants ? 1.0 : pitch) * params.formantRatio;
    if (formantScale < 1.0e-6)
        formantScale = 1.0e-6;
}

int PhaseVocoderEngine::feed(const Sample* const* input, int offset, int numFrames)
{
    const int accepted = std::min(numFrames, inputFifo.freeSpace());
    if (accepted > 0)
        inputFifo.write(input, offset, accepted);
    processFrames();
    return accepted;
}

int PhaseVocoderEngine::read(Sample* const* output, int offset, int numFrames)
{
    return outputFifo.read(output, offset, numFrames);
}

void PhaseVocoderEngine::detectOnsets()
{
    // Time-domain, sample-accurate onset scan over the freshly-arrived region of the
    // raw (pre-window) frame. Consecutive fresh regions tile the input, so every
    // sample is scanned exactly once — and an onset is always found on the FIRST
    // frame whose window could leak it backward, which is what makes the gate work.
    const int ch = config.numChannels;
    const int lo = std::max(kContextWin + 1, frameSize - prevHopIn - kAttackWin);
    const int hi = frameSize - kAttackWin;
    if (hi <= lo || onsetCount >= kMaxPending)
        return;

    double* pref = monoPrefix.data();
    pref[0] = 0.0;
    float prevS = 0.0f;
    for (int n = 0; n < frameSize; ++n)
    {
        float s = 0.0f;
        for (int c = 0; c < ch; ++c)
            s += frameScratch[static_cast<size_t>(c)][static_cast<size_t>(n)];
        const float d = (n == 0) ? 0.0f : s - prevS;
        prevS = s;
        pref[n + 1] = pref[n] + static_cast<double>(d) * d;
    }

    for (int r = lo; r < hi; r += 8)
    {
        const double before = (pref[r] - pref[r - kContextWin]) / kContextWin + 1e-14;
        const double after  = (pref[r + kAttackWin] - pref[r]) / kAttackWin + 1e-14;
        if (after > kOnsetRatio * before && after > 1e-8)
        {
            const long long p = inputAbs + r;   // attack lies in [r, r+kAttackWin); err early
            if (p - lastOnsetAbs < refractorySamples)
                continue;
            PendingOnset po;
            po.pAbs = (p <= inputAbs ? inputAbs + 1 : p);
            onsetQ[onsetCount++] = po;
            lastOnsetAbs = p;
            break;                              // one onset per fresh hop is enough
        }
    }
}

void PhaseVocoderEngine::captureSplice()
{
    // Copy the attack verbatim once [p-preRoll, p-preRoll+spliceLen) lies wholly
    // inside the peeked frame. Taken raw and per channel, so the attack keeps its
    // exact inter-channel timing and phase.
    if (onsetCount <= 0 || onsetQ[0].spliceSlot >= 0)
        return;

    const long long dist = onsetQ[0].pAbs - inputAbs;
    const int pre = static_cast<int>(std::min<long long>(splicePreRoll, std::max<long long>(dist - 1, 0)));
    if (dist <= 0 || dist - pre + spliceLen > frameSize)
        return;

    for (int s = 0; s < kMaxSplices; ++s)
    {
        if (splices[s].used)
            continue;
        const int src = static_cast<int>(dist) - pre;
        for (int c = 0; c < config.numChannels; ++c)
            std::memcpy(spliceBufs[s][static_cast<size_t>(c)].data(),
                        frameScratch[static_cast<size_t>(c)].data() + src,
                        static_cast<size_t>(spliceLen) * sizeof(Sample));
        splices[s].used     = true;
        splices[s].length   = spliceLen;
        splices[s].preRoll  = pre;
        splices[s].outStart = 0;                // anchored below from the hop schedule
        onsetQ[0].spliceSlot = s;
        break;
    }
}

void PhaseVocoderEngine::processFrames()
{
    const int maxSynthHop = static_cast<int>(emitScratch.empty() ? 0 : emitScratch[0].size());
    const double E = timeStretch;

    while (inputFifo.numReady() >= frameSize && outputFifo.freeSpace() >= maxSynthHop)
    {
        inputFifo.peek(framePtrs.data(), 0, frameSize);
        detectOnsets();

        // What is this frame's role relative to the next pending onset?
        bool resetNow = false;
        int  gateIdx  = -1;
        if (onsetCount > 0)
        {
            if (onsetQ[0].pAbs <= inputAbs)
            {
                // The frame starts exactly on the attack: resynthesise it at the
                // input's own phases and hold unity rate through it.
                resetNow = true;
                unityLeft = unityFrames;
                if (onsetQ[0].spliceSlot >= 0)
                    splices[onsetQ[0].spliceSlot].outStart =
                        outEmittedAbs - splices[onsetQ[0].spliceSlot].preRoll;
                --onsetCount;
                for (int i = 0; i < onsetCount; ++i)
                    onsetQ[i] = onsetQ[i + 1];
            }
            else if (onsetQ[0].pAbs - inputAbs < frameSize)
            {
                gateIdx = static_cast<int>(onsetQ[0].pAbs - inputAbs);
            }
        }

        captureSplice();

        // --- Hop schedule -------------------------------------------------------
        // Unity rate through the attack; the time-debt that creates is split in two.
        // The transition part (delta*(E-1)) is PRE-paid by the gated frames running
        // up to the onset, which is what makes the attack land at exactly E*p; the
        // unity-region part is post-paid by later stationary frames.
        int    hopIn;
        double hopOut;
        const double nominal   = analysisHop * E;
        const double hopOutCap = static_cast<double>(maxSynthHop - 1);

        if (unityLeft > 0)
        {
            hopIn = analysisHop;
            hopOut = analysisHop;
            --unityLeft;
        }
        else if (gateIdx >= 0 && gateIdx <= analysisHop)
        {
            // Transition hop: land the next frame exactly on the onset, at unity so
            // the gated content-end meets the reset frame with no coverage hole.
            hopIn = gateIdx;
            hopOut = gateIdx;
        }
        else if (gateIdx > analysisHop)
        {
            const int delta = ((gateIdx - 1) % analysisHop) + 1;
            const int m     = (gateIdx - delta) / analysisHop;
            hopIn = analysisHop;
            if (onsetQ[0].unityApproach < 0)
            {
                onsetQ[0].unityApproach   = std::max(0, std::min(1, m - 1));
                onsetQ[0].prepayRemaining = (delta + onsetQ[0].unityApproach * analysisHop) * (E - 1.0);
            }
            const int ua = onsetQ[0].unityApproach;
            if (m <= ua)
            {
                // Final approach frames stay at unity so their gate masks keep
                // covering the output right up to the attack.
                hopOut = analysisHop;
            }
            else
            {
                const double pay = onsetQ[0].prepayRemaining / (m - ua);
                onsetQ[0].prepayRemaining -= pay;
                hopOut = nominal + pay;
                if (debtOut > 0.0)
                    hopOut += std::min(debtOut, 0.15 * nominal);   // dense material may
                hopOut = clamp(hopOut, 4.0, hopOutCap);            // never see a normal frame
            }
        }
        else
        {
            hopIn = analysisHop;
            hopOut = nominal + clamp(debtOut,
                                     -std::min(0.35 * nominal, nominal - 4.0),
                                     std::min(0.15 * nominal, std::max(0.0, hopOutCap - nominal)));
        }

        // One invariant does all the accounting: the difference between the ideal
        // output for this input hop and what the schedule actually chose. The
        // long-term ratio is exact whenever the debt returns to zero.
        debtOut += hopIn * E - hopOut;

        // Keep a pending splice's output anchor in step with the schedule: the
        // output still to come before the onset is this frame's hop plus everything
        // scheduled after it.
        if (onsetCount > 0 && onsetQ[0].spliceSlot >= 0 && gateIdx > 0)
        {
            const long long gNext = static_cast<long long>(gateIdx) - hopIn;
            double remainingOut = hopOut;
            if (gNext > 0)
            {
                if (gNext > analysisHop)
                {
                    const int d2 = static_cast<int>((gNext - 1) % analysisHop) + 1;
                    const int m2 = static_cast<int>((gNext - d2) / analysisHop);
                    const int ua = std::max(0, onsetQ[0].unityApproach);
                    remainingOut += (m2 > ua)
                        ? (m2 - ua) * nominal + onsetQ[0].prepayRemaining + ua * analysisHop + d2
                        : m2 * static_cast<double>(analysisHop) + d2;
                }
                else
                {
                    remainingOut += static_cast<double>(gNext);
                }
            }
            Splice& sp = splices[onsetQ[0].spliceSlot];
            sp.outStart = outEmittedAbs + std::llround(remainingOut) - sp.preRoll;
        }

        processOneFrame(gateIdx, resetNow, hopIn, hopOut);
        inputFifo.discard(hopIn);
        inputAbs += hopIn;
        prevHopIn = hopIn;
        prevHopOut = hopOut;
    }
}

void PhaseVocoderEngine::processOneFrame(int gateIdx, bool resetNow, int hopIn, double hopOut)
{
    const int ch = config.numChannels;

    // Pre-echo gate. A pending onset at frame offset `gateIdx` means everything from
    // there on is the coming attack. Fade it out of this analysis frame so none of
    // its energy can be smeared backward through the window into the output before
    // the hit — the mechanism that makes phase vocoders sound soft on drums.
    if (gateIdx >= 0 && gateIdx < frameSize)
    {
        const int fade = std::min(64, gateIdx);
        const int gs = gateIdx - fade;
        for (int c = 0; c < ch; ++c)
        {
            Sample* f = frameScratch[static_cast<size_t>(c)].data();
            for (int n = gs; n < gateIdx; ++n)
            {
                const float t = static_cast<float>(n - gs) / static_cast<float>(fade);
                f[n] *= 0.5f * (1.0f + std::cos(static_cast<float>(dsp::kPi) * t));
            }
            std::memset(f + gateIdx, 0, static_cast<size_t>(frameSize - gateIdx) * sizeof(Sample));
        }
    }

    std::fill(detectMag.begin(), detectMag.end(), 0.0f);
    std::fill(monoSpec.begin(), monoSpec.end(), Complex{});

    // --- Analysis: window, transform, and gather magnitude/phase per channel. ---
    for (int c = 0; c < ch; ++c)
    {
        Sample* frame = frameScratch[static_cast<size_t>(c)].data();
        analysisWindow.apply(frame);
        fft.forwardReal(frame, spectrum[static_cast<size_t>(c)].data());

        auto& mag = magnitude[static_cast<size_t>(c)];
        auto& aph = analysisPhase[static_cast<size_t>(c)];
        const Complex* spec = spectrum[static_cast<size_t>(c)].data();
        for (int k = 0; k < halfBins; ++k)
        {
            mag[static_cast<size_t>(k)] = std::abs(spec[k]);
            aph[static_cast<size_t>(k)] = std::arg(spec[k]);
            detectMag[static_cast<size_t>(k)] += mag[static_cast<size_t>(k)];
            monoSpec[static_cast<size_t>(k)] += spec[k];
        }

        if (formantActive)
            envelopes[static_cast<size_t>(c)].compute(spec);
    }

    if (phaseLockEnabled)
    {
        locatePeaks(detectMag.data());
        classifyNoiseBins(resetNow, hopIn, hopOut);
    }

    // ONE shared phase advance per frame (mono reference) — inter-channel phase
    // offsets are re-applied per channel from the analysis spectra, so the input's
    // stereo image is preserved exactly instead of drifting.
    advanceSharedPhase(resetNow);

    // --- Synthesis per channel into the shared overlap-add accumulator. ---
    for (int c = 0; c < ch; ++c)
        reconstructChannel(c);

    // Window-energy accumulators (shared by all channels; added once per frame). A
    // gated frame only contributes energy where it still has content, so the frames
    // that follow are normalised back up to full level past the cut.
    const Sample* synWin = synthesisWindow.data();
    if (gateIdx >= 0 && gateIdx < frameSize)
    {
        const int fade = std::min(64, gateIdx);
        const int gs = gateIdx - fade;
        for (int n = 0; n < gs; ++n)
            normBuffer[static_cast<size_t>(n)] += synWin[n] * synWin[n];
        for (int n = gs; n < gateIdx; ++n)
        {
            const float t = static_cast<float>(n - gs) / static_cast<float>(fade);
            const float gm = 0.5f * (1.0f + std::cos(static_cast<float>(dsp::kPi) * t));
            normBuffer[static_cast<size_t>(n)] += synWin[n] * synWin[n] * gm;
        }
    }
    else
    {
        for (int n = 0; n < frameSize; ++n)
            normBuffer[static_cast<size_t>(n)] += synWin[n] * synWin[n];
    }
    for (int n = 0; n < frameSize; ++n)
        normFullBuffer[static_cast<size_t>(n)] += synWin[n] * synWin[n];

    emit(hopOut);
    primed = true;
}

void PhaseVocoderEngine::classifyNoiseBins(bool resetNow, int hopIn, double hopOut)
{
    // Identity locking assumes every bin in a region belongs to one sinusoid. Where
    // that is false — the peakless hiss stretches above the last real partial, and
    // regions whose "peak" is just the tallest noise sample — locking pins a bin to a
    // DISTANT peak, so its per-frame synthesis-phase error is (w_k - w_p)(Hs - Ha):
    // hundreds of radians. Successive frames then overlap-add incoherently and the
    // band loses Sum(w^4)/Sum(w^2)^2 of its power (−7.1 dB at 0.8x/Best, −4.4 dB at
    // 1.5x — measured to 0.1 dB of that prediction). That is the HF dulling.
    //
    // The cure is to stop pretending those bins are coherent: give them a fresh
    // random rotation each frame so they are decorrelated BY CONSTRUCTION, and then
    // add back exactly the power the now-known-incoherent sum loses.
    // Judge coherence by this frame's LOCAL hop ratio, not the global one: the
    // transient schedule runs the frames around each attack at unity rate, and there
    // the overlap-add really is coherent — decorrelating those would throw away the
    // very attack sharpness the schedule just bought, and the compensation gain
    // (derived for incoherent addition) would over-boost them.
    const double localRatio = hopIn > 0 ? hopOut / hopIn : timeStretch;
    frameRandomize = primed && !resetNow && std::abs(localRatio - 1.0) > 0.03;
    if (!frameRandomize)
        return;

    // Exact incoherent-WOLA deficit for w-enveloped frames, (Sum(w^4)/Sum(w^2)^2)^-1/2,
    // at this frame's ACTUAL synthesis hop.
    compGain = static_cast<float>(kHannM2 * std::sqrt(frameSize / (std::max(1.0, hopOut) * kHannM4)));
    compGain = clamp(compGain, 1.0f, 6.0f);

    // One offset per 32-bin chunk, shared by all channels: a constant rotation across
    // a narrow band preserves that band's temporal envelope inside the frame (so hits
    // stay sharp), and sharing it across channels leaves the stereo image intact.
    for (int chunk = 0; chunk < halfBins; chunk += kRandChunk)
    {
        rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
        const float u = (static_cast<float>(rngState) * (1.0f / 4294967296.0f) - 0.5f)
                        * 2.0f * static_cast<float>(dsp::kPi);
        const int end = std::min(chunk + kRandChunk, halfBins);
        for (int j = chunk; j < end; ++j)
            randOffset[static_cast<size_t>(j)] = u;
    }

    std::fill(noiseBin.begin(), noiseBin.end(), std::uint8_t{0});
    if (peaks.empty())
        return;                 // graceful path: locatePeaks already made every bin its own peak

    // Walk each peak's region (a contiguous run of equal peakOf) and mark as noise
    // either the whole region — when its peak lacks prominence over the region's
    // off-lobe mean, i.e. it is a noise maximum that will jump bins next frame — or
    // just the bins too far from the peak to be part of its main lobe.
    int k = 0;
    for (const int p : peaks)
    {
        int kEnd = k;
        while (kEnd < halfBins && peakOf[static_cast<size_t>(kEnd)] == p)
            ++kEnd;

        float rest = 0.0f;
        int nRest = 0;
        for (int j = k; j < kEnd; ++j)
        {
            if (j >= p - 2 && j <= p + 2)
                continue;                       // skip the peak's own main lobe
            rest += detectMag[static_cast<size_t>(j)];
            ++nRest;
        }
        const float meanRest = nRest > 0 ? rest / static_cast<float>(nRest) : 0.0f;
        const bool noisyRegion = nRest > 0
                                 && detectMag[static_cast<size_t>(p)] < kPeakProminence * meanRest;

        for (int j = k; j < kEnd; ++j)
            if (noisyRegion || std::abs(j - p) > kMainLobeBins)
                noiseBin[static_cast<size_t>(j)] = std::uint8_t{1};

        k = kEnd;
    }
}

void PhaseVocoderEngine::advanceSharedPhase(bool resetNow)
{
    // The transient schedule varies both hops, so the phase maths must use the
    // ACTUAL ones: the expected phase advance comes from the input hop that led
    // INTO this frame, and the synthesis advance from the output spacing between
    // the previous frame and this one.
    const int   hopIn    = (prevHopIn > 0 && prevHopIn < frameSize) ? prevHopIn : analysisHop;
    const float omega0   = static_cast<float>(kTwoPi) / static_cast<float>(frameSize);
    const float omegaHop = omega0 * static_cast<float>(hopIn);
    const float sHop     = static_cast<float>(prevHopOut);
    const float invHa    = 1.0f / static_cast<float>(hopIn);

    float* S = sharedSynthPhase.data();

    // Mono-reference analysis phase. Where the channels cancel (|sum| tiny versus
    // the summed magnitudes) the argument is numerically meaningless; predict the
    // phase forward at bin frequency instead so trueFreq degrades to the bin centre.
    for (int k = 0; k < halfBins; ++k)
    {
        const Complex s = monoSpec[static_cast<size_t>(k)];
        const float mSum = detectMag[static_cast<size_t>(k)];
        if (std::abs(s) > 1.0e-3f * mSum)
            aphMono[static_cast<size_t>(k)] = std::arg(s);
        else
            aphMono[static_cast<size_t>(k)] = wrapPhase(prevAphMono[static_cast<size_t>(k)]
                                                        + static_cast<float>(k) * omegaHop);
    }

    for (int k = 0; k < halfBins; ++k)
    {
        const float expected = static_cast<float>(k) * omegaHop;
        const float dev = wrapPhase(aphMono[static_cast<size_t>(k)] - prevAphMono[static_cast<size_t>(k)] - expected);
        trueFreq[static_cast<size_t>(k)] = static_cast<float>(k) * omega0 + dev * invHa;
    }

    if (!primed || resetNow)
    {
        // Phase reset: with S = aphMono the per-channel synthesis phase becomes
        // exactly the channel's analysis phase (offsets cancel), as before.
        for (int k = 0; k < halfBins; ++k)
            S[k] = aphMono[static_cast<size_t>(k)];
    }
    else if (phaseLockEnabled)
    {
        // Identity phase locking: advance peaks by their true frequency, then lock
        // every other bin rigidly to its peak's phase — preserving the phase
        // relationships within each sinusoid's main lobe. Applied to the shared
        // reference, each channel still reproduces its own intra-lobe structure,
        // since out_c[k] - out_c[p] = aph_c[k] - aph_c[p] exactly as before.
        for (int p : peaks)
            S[p] = wrapPhase(S[p] + trueFreq[static_cast<size_t>(p)] * sHop);

        for (int k = 0; k < halfBins; ++k)
        {
            if (frameRandomize && noiseBin[static_cast<size_t>(k)])
            {
                // Noise bin: analysis phase plus this frame's chunk rotation. Per
                // channel this resolves to aph_c[k] + randOffset[k], so successive
                // frames are mutually decorrelated while the intra-frame and
                // inter-channel structure is untouched.
                S[k] = wrapPhase(aphMono[static_cast<size_t>(k)] + randOffset[static_cast<size_t>(k)]);
                continue;
            }
            const int p = peakOf[static_cast<size_t>(k)];
            S[k] = wrapPhase(S[p] + (aphMono[static_cast<size_t>(k)] - aphMono[static_cast<size_t>(p)]));
        }
    }
    else
    {
        // Classic per-bin phase propagation (Draft quality) — drift-free too, since
        // it now runs once on the shared reference rather than per channel.
        for (int k = 0; k < halfBins; ++k)
            S[k] = wrapPhase(S[k] + trueFreq[static_cast<size_t>(k)] * sHop);
    }

    std::memcpy(prevAphMono.data(), aphMono.data(), static_cast<size_t>(halfBins) * sizeof(float));
}

void PhaseVocoderEngine::reconstructChannel(int c)
{
    auto& mag = magnitude[static_cast<size_t>(c)];
    auto& aph = analysisPhase[static_cast<size_t>(c)];
    Complex* spec = spectrum[static_cast<size_t>(c)].data();
    const float* S = sharedSynthPhase.data();

    // Build the half-spectrum with (optionally formant-corrected) magnitude and the
    // shared synthesis phase plus this channel's own analysis-phase offset. The
    // offset term hands the input's exact inter-channel phase difference to the
    // output every frame: out_L[k]-out_R[k] = aph_L[k]-aph_R[k].
    for (int k = 0; k < halfBins; ++k)
    {
        float m = mag[static_cast<size_t>(k)];
        if (frameRandomize && noiseBin[static_cast<size_t>(k)])
            m *= compGain;      // restore the power an incoherent overlap-add loses
        if (formantActive)
        {
            const float g = static_cast<float>(k);
            const float srcBin = static_cast<float>(pitchRatio * g / formantScale);
            const float eHere = envelopes[static_cast<size_t>(c)].envelopeAt(g) + 1.0e-9f;
            const float eThere = envelopes[static_cast<size_t>(c)].envelopeAt(srcBin);
            m *= eThere / eHere;
        }
        const float phase = S[k] + (aph[static_cast<size_t>(k)] - aphMono[static_cast<size_t>(k)]);
        spec[k] = Complex(m * std::cos(phase), m * std::sin(phase));
    }

    // DC and Nyquist must be real for a real time-domain signal. The upper-half
    // conjugate mirror is implied — inverseReal() reconstructs it internally, so we
    // don't write it here.
    spec[0]            = Complex(spec[0].real(), 0.0f);
    spec[frameSize / 2] = Complex(spec[frameSize / 2].real(), 0.0f);

    // Back to time and overlap-add with the synthesis window.
    Sample* timeFrame = frameScratch[static_cast<size_t>(c)].data();
    fft.inverseReal(spec, timeFrame);

    const Sample* synWin = synthesisWindow.data();
    Sample* ola = olaBuffer[static_cast<size_t>(c)].data();
    for (int n = 0; n < frameSize; ++n)
        ola[n] += timeFrame[n] * synWin[n];
}

void PhaseVocoderEngine::emit(double synthesisHop)
{
    hopRemainder += synthesisHop;
    int hsInt = static_cast<int>(std::floor(hopRemainder));
    hopRemainder -= hsInt;

    if (hsInt <= 0)
        return;

    const int maxSynthHop = static_cast<int>(emitScratch[0].size());
    if (hsInt > maxSynthHop)
        hsInt = maxSynthHop;

    const int ch = config.numChannels;
    for (int n = 0; n < hsInt; ++n)
    {
        // Tikhonov-regularised normalisation. Where window coverage is complete this
        // is 1/norm to within 0.15 dB; where coverage collapses — around a transient
        // cut — it fades to silence instead of amplifying the little that is there.
        const float norm = normBuffer[static_cast<size_t>(n)];
        float gain = norm / (norm * norm + kNormEpsSq);
        // And bound the gate compensation: phase-locked resynthesis is not
        // sample-exact, so unbounded compensation turns a local mismatch into a
        // full-scale spike. 4/normFull is ~12 dB over the full-coverage gain.
        const float full = normFullBuffer[static_cast<size_t>(n)];
        if (full > 1.0e-3f)
            gain = std::min(gain, 4.0f / full);
        for (int c = 0; c < ch; ++c)
            emitScratch[static_cast<size_t>(c)][static_cast<size_t>(n)] = olaBuffer[static_cast<size_t>(c)][static_cast<size_t>(n)] * gain;
    }

    // Mix any active verbatim attacks over the vocoder's own rendering of them. The
    // window taper cannot carry a full-level attack at a frame edge, so the first
    // few milliseconds come from the input untouched, then cross-fade back.
    for (auto& sp : splices)
    {
        if (!sp.used)
            continue;
        const long long a0 = std::max(sp.outStart, outEmittedAbs);
        const long long a1 = std::min(sp.outStart + sp.length, outEmittedAbs + hsInt);
        for (long long q = a0; q < a1; ++q)
        {
            const int tau = static_cast<int>(q - sp.outStart);
            float w, pvW;
            if (tau < spliceFadeIn)
            {
                w = 0.5f * (1.0f - std::cos(static_cast<float>(dsp::kPi) * tau / spliceFadeIn));
                pvW = 1.0f - w;
            }
            else if (tau < spliceHold)
            {
                w = 1.0f;
                pvW = 0.0f;
            }
            else
            {
                // Equal-power back into the vocoder render: the splice tail and the
                // vocoder's version of the decay are uncorrelated, so amplitude
                // cross-fading them would dip.
                w = 0.5f * (1.0f + std::cos(static_cast<float>(dsp::kPi) * (tau - spliceHold)
                                            / static_cast<float>(sp.length - spliceHold)));
                pvW = std::sqrt(std::max(0.0f, 1.0f - w * w));
            }
            const int n = static_cast<int>(q - outEmittedAbs);
            const size_t slot = static_cast<size_t>(&sp - splices);
            for (int c = 0; c < ch; ++c)
            {
                Sample& o = emitScratch[static_cast<size_t>(c)][static_cast<size_t>(n)];
                o = o * pvW + spliceBufs[slot][static_cast<size_t>(c)][static_cast<size_t>(tau)] * w;
            }
        }
        if (outEmittedAbs + hsInt >= sp.outStart + sp.length)
            sp.used = false;               // fully played (or stale) — free the slot
    }

    if (outputFifo.freeSpace() >= hsInt)
        outputFifo.write(emitPtrs.data(), 0, hsInt);
    outEmittedAbs += hsInt;

    // Slide the accumulators left by the emitted amount; zero the freed tail.
    const int keep = olaLength - hsInt;
    for (int c = 0; c < ch; ++c)
    {
        Sample* ola = olaBuffer[static_cast<size_t>(c)].data();
        std::memmove(ola, ola + hsInt, static_cast<size_t>(keep) * sizeof(Sample));
        std::memset(ola + keep, 0, static_cast<size_t>(hsInt) * sizeof(Sample));
    }
    std::memmove(normBuffer.data(), normBuffer.data() + hsInt, static_cast<size_t>(keep) * sizeof(Sample));
    std::memset(normBuffer.data() + keep, 0, static_cast<size_t>(hsInt) * sizeof(Sample));
    std::memmove(normFullBuffer.data(), normFullBuffer.data() + hsInt, static_cast<size_t>(keep) * sizeof(Sample));
    std::memset(normFullBuffer.data() + keep, 0, static_cast<size_t>(hsInt) * sizeof(Sample));
}

void PhaseVocoderEngine::locatePeaks(const Sample* magnitudes)
{
    peaks.clear();

    float maxMag = 0.0f;
    for (int k = 0; k < halfBins; ++k)
        maxMag = std::max(maxMag, magnitudes[k]);
    const float threshold = maxMag * 1.0e-3f + 1.0e-9f;

    // Peak picking with a 4-neighbour test (Laroche & Dolson): a bin is a peak only
    // if it dominates the two bins on each side. On polyphonic material this rejects
    // the small interference ripples that beat between two real tones — those pass a
    // lax 3-point test and, once locked, inject phase at a garbage frequency. Genuine
    // partials (well above the -60 dB relative floor and locally dominant) survive,
    // and a single sinusoid's main lobe always does.
    for (int k = 1; k < halfBins - 1; ++k)
    {
        const float m = magnitudes[k];
        if (m <= threshold)
            continue;
        if (m <= magnitudes[k - 1] || m < magnitudes[k + 1])
            continue;
        if (k >= 2 && m <= magnitudes[k - 2])
            continue;
        if (k <= halfBins - 3 && m < magnitudes[k + 2])
            continue;
        peaks.push_back(k);
    }

    if (peaks.empty())
    {
        // No clear peaks (near-silence / noise): every bin is its own peak, which
        // degrades gracefully to classic per-bin propagation.
        for (int k = 0; k < halfBins; ++k)
            peakOf[static_cast<size_t>(k)] = k;
        return;
    }

    // Assign each bin to a peak, splitting adjacent peaks' regions at the spectral
    // VALLEY between them rather than the index midpoint. The rigid phase-lock
    // boundary then falls where the spectrum is weakest (least energy to mis-assign),
    // which markedly cleans up the region between two close tones.
    auto valleyBetween = [&](int a, int b) {
        int m = a; float mv = magnitudes[a];
        for (int j = a + 1; j < b; ++j)
            if (magnitudes[j] < mv) { mv = magnitudes[j]; m = j; }
        return m;
    };

    int idx = 0;
    int boundary = (peaks.size() > 1)
                       ? valleyBetween(peaks[0], peaks[1]) : halfBins;
    for (int k = 0; k < halfBins; ++k)
    {
        while (idx + 1 < static_cast<int>(peaks.size()) && k > boundary)
        {
            ++idx;
            boundary = (idx + 1 < static_cast<int>(peaks.size()))
                           ? valleyBetween(peaks[static_cast<size_t>(idx)], peaks[static_cast<size_t>(idx + 1)])
                           : halfBins;
        }
        peakOf[static_cast<size_t>(k)] = peaks[static_cast<size_t>(idx)];
    }
}

} // namespace chronos::engine
