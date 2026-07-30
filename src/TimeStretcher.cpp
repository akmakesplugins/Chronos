// Chronos — TimeStretcher.cpp
//
// The facade implementation. See TimeStretcher.h for the design overview. The data
// flow for one channel is:
//
//     host input --> [engine: time-stretch by E = timeRatio*pitchRatio] -->
//         stretchFifo --> [resampler: read pitchRatio in per out] --> host output
//
// The engine preserves pitch and only changes duration; the resampler then rescales
// the time axis, which simultaneously restores the requested duration and shifts
// pitch. Formant correction happens inside the engine (it needs the spectrum), told
// the pitchRatio so it can pre-compensate for the resampling that follows.

#include "chronos/TimeStretcher.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "dsp/AudioFifo.h"
#include "dsp/Fft.h"
#include "dsp/SincResampler.h"
#include "engine/PhaseVocoderEngine.h"
#include "engine/SinusoidalEngine.h"
#include "engine/WsolaEngine.h"

namespace chronos
{

namespace
{
    constexpr double kMinPitchRatio = 0.25;
    constexpr double kMaxPitchRatio = 4.0;
}

struct TimeStretcher::Impl
{
    StreamConfig config;
    StretchParameters params;

    // Both engines are owned and prepared so switching is a pointer swap (no
    // allocation on the audio thread). `active` points at the selected one.
    engine::PhaseVocoderEngine phaseVocoder;
    engine::WsolaEngine        wsola;
    engine::SinusoidalEngine   sinusoidal;
    engine::IStretchEngine*    active = nullptr;

    dsp::SincResampler resampler; ///< Windowed-sinc polyphase (anti-aliased) pitch stage.
    dsp::AudioFifo stretchFifo;   ///< Buffers engine output ahead of the resampler.

    // Smoothed control values (snap on first set; then glide toward target).
    double smoothedTime  = 1.0;
    double smoothedPitch = 1.0;
    double targetTime    = 1.0;
    double targetPitch   = 1.0;
    bool   smoothingInit = false;

    // Scratch (all sized in prepare()).
    std::vector<std::vector<Sample>> engineReadBuf; ///< drain engine -> stretchFifo
    std::vector<Sample*> engineReadPtrs;
    std::vector<std::vector<Sample>> resampBuf;     ///< peek stretchFifo -> resampler
    std::vector<Sample*> resampPtrs;
    std::vector<Sample*> outOffsetPtrs;             ///< offset output channel ptrs
    std::vector<std::vector<Sample>> silenceBuf;    ///< zeros for flush()
    std::vector<Sample*> silencePtrs;

    int drainChunk = 0;
    int resampChunk = 0;
    bool prepared = false;
    Quality preparedQuality = Quality::Good;   ///< Quality the engines were last built at.

    // Content-adaptive Auto. The engine is chosen from a HALF-SECOND probe, not a
    // single frame: percussion and tonal material are distinguished by how energy
    // evolves over time, which one 46 ms window simply cannot see. (The previous
    // single-frame probe routed every full drum loop to the phase vocoder — a loop
    // that opens on a kick looks like one decaying sine — while sending strummed
    // guitar and piano chords, whose attacks are bright and dense, to WSOLA.)
    //
    // Features, all accumulated incrementally as input arrives so no analysis spike
    // ever lands on the audio thread:
    //   hprDb      harmonic/percussive energy ratio above 200 Hz, from median
    //              filtering the probe spectrogram in time (harmonic) and in
    //              frequency (percussive) — the feature that actually separates a
    //              drum kit from a bright chord.
    //   fluxCrest  peak-to-mean of the spectral flux: sparse hard hits vs sustain.
    //   corr       L/R correlation — wide/ambient material is not a dry kit, and
    //              WSOLA's mono-averaged similarity search would collapse it.
    static constexpr int kProbeFft = 1024;
    static constexpr int kMedian   = 17;      ///< Median filter length (~99 ms / ~730 Hz).
    static constexpr double kProbeSeconds   = 0.5;
    static constexpr double kPendingSeconds = 1.0;
    static constexpr double kMinProbeSeconds = 0.25;  ///< Below this, features are unreliable.

    dsp::Fft probeFft;
    std::vector<std::vector<Sample>> pendingBuf;  ///< [ch][pendingCap] input held until latch
    std::vector<Sample*> pendingPtrs;
    std::vector<float> probeMono;        ///< [kProbeFft] mono-sum frame scratch
    std::vector<dsp::Complex> probeSpec; ///< [kProbeFft] FFT scratch (preallocated)
    std::vector<float> probeWindow;      ///< [kProbeFft] Hann
    std::vector<std::vector<float>> magRing;  ///< [kMedian][probeBins] recent magnitudes
    std::vector<float> prevMag;          ///< [probeBins] for the flux
    std::vector<float> medScratch;       ///< [kMedian] nth_element workspace
    std::vector<float> freqScratch;      ///< [kMedian] nth_element workspace
    std::vector<float> fluxSeq;          ///< one flux value per probe hop

    int probeHoldSamples = 0;   ///< Audible samples needed before latching.
    int pendingCap = 0;         ///< Hard cap on held input.
    int probeBins = 0;
    int probeHop = 0;
    int probeK0 = 0;            ///< First bin above 200 Hz.
    int probeFilled = 0;        ///< Samples accumulated into the current probe frame.
    int ringCount = 0;          ///< Frames written into magRing.
    int ringHead = 0;
    int audibleHeld = 0;        ///< Held samples since the first non-silent one.
    bool probeStarted = false;  ///< True once audio above the silence floor arrived.
    double eHarmonic = 0.0, ePercussive = 0.0;
    double sumL = 0.0, sumR = 0.0, sumLL = 0.0, sumRR = 0.0, sumLR = 0.0;
    long long corrCount = 0;

    int  pendingFilled = 0;
    int  pendingPos = 0;        ///< How much of the hold has been replayed.
    bool autoLatched = false;
    engine::IStretchEngine* autoEngine = nullptr;   ///< Engine the probe latched onto.

    /// Feed newly-held input through the probe's running analysis. Called only
    /// while Auto is still buffering; RT-safe (all storage sized in prepare()).
    void accumulateProbe(const Sample* const* in, int offset, int num) noexcept
    {
        const int ch = config.numChannels;
        constexpr float kSilence = 1.0e-3f;      // -60 dBFS

        for (int i = 0; i < num; ++i)
        {
            float mono = 0.0f;
            for (int c = 0; c < ch; ++c)
                mono += in[c][offset + i];
            mono /= static_cast<float>(ch);

            // Leading silence is held but not analysed, so a loop that starts with a
            // gap still gets a full half-second of actual audio to judge.
            if (!probeStarted)
            {
                if (std::abs(mono) < kSilence)
                    continue;
                probeStarted = true;
            }
            ++audibleHeld;

            if (ch >= 2)
            {
                const double l = in[0][offset + i], r = in[1][offset + i];
                sumL += l; sumR += r; sumLL += l * l; sumRR += r * r; sumLR += l * r;
                ++corrCount;
            }

            probeMono[static_cast<size_t>(probeFilled++)] = mono;
            if (probeFilled < kProbeFft)
                continue;

            // --- One probe frame is full: transform and fold into the features. ---
            dsp::Complex* spec = probeSpec.data();
            for (int n = 0; n < kProbeFft; ++n)
                spec[n] = dsp::Complex(probeMono[static_cast<size_t>(n)] * probeWindow[static_cast<size_t>(n)], 0.0f);
            probeFft.forward(spec);

            auto& frame = magRing[static_cast<size_t>(ringHead)];
            float flux = 0.0f;
            for (int k = 0; k < probeBins; ++k)
            {
                const float m = std::abs(spec[static_cast<size_t>(k)]);
                frame[static_cast<size_t>(k)] = m;
                const float d = m - prevMag[static_cast<size_t>(k)];
                if (d > 0.0f)
                    flux += d;
                prevMag[static_cast<size_t>(k)] = m;
            }
            if (fluxSeq.size() < fluxSeq.capacity())
                fluxSeq.push_back(flux);

            ringHead = (ringHead + 1) % kMedian;
            ++ringCount;

            // Harmonic/percussive split (Fitzgerald): a harmonic partial is steady
            // ACROSS TIME, so a median along time keeps it; a percussive hit is broad
            // ACROSS FREQUENCY, so a median along frequency keeps it. Soft-mask the
            // centre frame of the ring by which median dominates, and bank the energy.
            if (ringCount >= kMedian)
            {
                const int centre = (ringHead + kMedian / 2) % kMedian;
                const auto& cf = magRing[static_cast<size_t>(centre)];
                for (int k = probeK0; k < probeBins; ++k)
                {
                    for (int t = 0; t < kMedian; ++t)
                        medScratch[static_cast<size_t>(t)] = magRing[static_cast<size_t>(t)][static_cast<size_t>(k)];
                    std::nth_element(medScratch.begin(), medScratch.begin() + kMedian / 2, medScratch.end());
                    const double h = medScratch[static_cast<size_t>(kMedian / 2)];

                    for (int j = 0; j < kMedian; ++j)
                    {
                        const int kk = clamp(k - kMedian / 2 + j, 0, probeBins - 1);
                        freqScratch[static_cast<size_t>(j)] = cf[static_cast<size_t>(kk)];
                    }
                    std::nth_element(freqScratch.begin(), freqScratch.begin() + kMedian / 2, freqScratch.end());
                    const double pcv = freqScratch[static_cast<size_t>(kMedian / 2)];

                    const double s2 = static_cast<double>(cf[static_cast<size_t>(k)]) * cf[static_cast<size_t>(k)];
                    const double den = h * h + pcv * pcv + 1e-20;
                    eHarmonic   += s2 * (h * h) / den;
                    ePercussive += s2 * (pcv * pcv) / den;
                }
            }

            // Slide the frame by one hop (75% overlap).
            std::copy(probeMono.begin() + probeHop, probeMono.begin() + kProbeFft, probeMono.begin());
            probeFilled = kProbeFft - probeHop;
        }
    }

    /// Decide which engine suits the probed material. Everything here is read from
    /// the accumulators; no signal processing happens at latch time.
    engine::IStretchEngine* classifyAndPick() noexcept
    {
        // Formants are only modelled by the phase vocoder.
        if (params.preserveFormants || std::abs(params.formantRatio - 1.0) > 1e-6)
            return &phaseVocoder;

        // Too little audio to judge: the safe default handles everything acceptably.
        if (audibleHeld < static_cast<int>(kMinProbeSeconds * config.sampleRate) || ringCount < kMedian)
            return &phaseVocoder;

        const double hprDb = 10.0 * std::log10((eHarmonic + 1e-20) / (ePercussive + 1e-20));

        double fluxMean = 0.0, fluxMax = 0.0;
        for (const float f : fluxSeq) { fluxMean += f; fluxMax = std::max(fluxMax, static_cast<double>(f)); }
        fluxMean /= std::max<size_t>(1, fluxSeq.size());
        const double fluxCrest = fluxMax / (fluxMean + 1e-12);

        double corr = 1.0;
        if (config.numChannels >= 2 && corrCount > 1)
        {
            const double n = static_cast<double>(corrCount);
            const double cov = sumLR - sumL * sumR / n;
            const double vl  = sumLL - sumL * sumL / n;
            const double vr  = sumRR - sumR * sumR / n;
            corr = (vl > 1e-12 && vr > 1e-12) ? cov / std::sqrt(vl * vr) : 1.0;
        }

        // WSOLA for a dry, hard-hitting kit: percussive-dominant above 200 Hz, sparse
        // sharp flux peaks, and channels coherent enough that its mono-averaged
        // similarity search will not smear the image. Anything else — including wide
        // or ambient percussion — goes to the phase vocoder, which now handles
        // attacks properly and preserves the stereo field exactly.
        const bool percussive = hprDb < 4.5 && fluxCrest > 6.0
                                && (config.numChannels < 2 || corr > 0.9);
        if (percussive)
            return &wsola;

        // Sinusoidal has no noise residual, so real recordings lose their air through
        // it. It is reserved for material that is almost purely tonal.
        if (hprDb > 20.0 && fluxCrest < 3.0)
            return &sinusoidal;

        return &phaseVocoder;
    }

    /// (Re)build both engines at the currently-stored quality. Allocates — setup
    /// thread only. Kept in one place so prepare() and a quality change from
    /// setParameters() go through identical logic.
    void prepareEngines()
    {
        engine::EngineConfig ec;
        ec.sampleRate   = config.sampleRate;
        ec.numChannels  = config.numChannels;
        ec.quality      = params.quality;
        ec.maxBlockSize = config.maxBlockSize;
        phaseVocoder.prepare(ec);
        wsola.prepare(ec);
        sinusoidal.prepare(ec);
        preparedQuality = params.quality;
        active = chooseEngine(params);
    }

    engine::IStretchEngine* chooseEngine(const StretchParameters& p) noexcept
    {
        EngineType e = p.engine;
        const bool needsFormants = p.preserveFormants || std::abs(p.formantRatio - 1.0) > 1.0e-6;

        // Auto policy: once the content probe has latched an engine, STICK with it —
        // otherwise a later setParameters() call (which hosts issue every block) would
        // silently revert the choice back to the phase vocoder. Before the latch, and
        // whenever formants are needed, fall back to the phase vocoder.
        if (e == EngineType::Auto)
        {
            if (! needsFormants && autoLatched && autoEngine != nullptr)
                return autoEngine;
            e = EngineType::PhaseVocoder;
        }
        if (needsFormants)
            e = EngineType::PhaseVocoder;   // Sinusoidal formant warp is a later phase

        switch (e)
        {
            case EngineType::Wsola:      return static_cast<engine::IStretchEngine*>(&wsola);
            case EngineType::Sinusoidal: return static_cast<engine::IStretchEngine*>(&sinusoidal);
            case EngineType::PhaseVocoder:
            case EngineType::Auto:       break;
        }
        return static_cast<engine::IStretchEngine*>(&phaseVocoder);
    }

    void updateSmoothing() noexcept
    {
        // One-pole glide. Fast enough to track automation, slow enough to avoid
        // audible steps. Converges to a constant target within a few blocks.
        constexpr double coeff = 0.35;
        smoothedTime  += (targetTime  - smoothedTime)  * coeff;
        smoothedPitch += (targetPitch - smoothedPitch) * coeff;

        active->setTimeStretch(smoothedTime * smoothedPitch);   // E = timeRatio*pitchRatio
        resampler.setRatio(smoothedPitch);                      // input-per-output
    }

    void drainEngineToFifo() noexcept
    {
        while (active->available() > 0 && stretchFifo.freeSpace() > 0)
        {
            const int n = std::min({ active->available(), stretchFifo.freeSpace(), drainChunk });
            active->read(engineReadPtrs.data(), 0, n);
            stretchFifo.write(engineReadPtrs.data(), 0, n);
        }
    }

    /// Feed `num` samples starting at `offset` into the active engine, draining its
    /// output into stretchFifo, retrying on partial acceptance.
    void feedAndDrain(const Sample* const* in, int offset, int num) noexcept
    {
        int rem = num, off = offset;
        while (rem > 0)
        {
            const int accepted = active->feed(in, off, rem);
            drainEngineToFifo();
            if (accepted <= 0) break;
            off += accepted;
            rem -= accepted;
        }
    }

    /// Feed as much of the held probe input as the pipeline will currently take.
    ///
    /// Half a second of held input becomes up to half a second times the stretch
    /// ratio of output, which is far more than the bridge FIFO holds — so it cannot
    /// all be pushed at once. Anything still held stays queued and goes in as the
    /// caller consumes output; feeding it blindly would silently discard it.
    void replayPending() noexcept
    {
        while (pendingPos < pendingFilled)
        {
            const int accepted = active->feed(pendingPtrs.data(), pendingPos, pendingFilled - pendingPos);
            drainEngineToFifo();
            if (accepted <= 0)
                break;
            pendingPos += accepted;
        }

        if (pendingPos >= pendingFilled)
        {
            pendingPos = 0;
            pendingFilled = 0;              // backlog cleared
            return;
        }

        // Compact what is left to the front so fresh input can queue behind it and
        // stream order is preserved.
        if (pendingPos > 0)
        {
            const int remain = pendingFilled - pendingPos;
            for (int c = 0; c < config.numChannels; ++c)
                std::memmove(pendingBuf[static_cast<size_t>(c)].data(),
                             pendingBuf[static_cast<size_t>(c)].data() + pendingPos,
                             static_cast<size_t>(remain) * sizeof(Sample));
            pendingFilled = remain;
            pendingPos = 0;
        }
    }

    /// Classify the probed material, switch to the chosen engine, and begin feeding
    /// the held input to it. Called once per stream when Auto is selected.
    void latchAuto() noexcept
    {
        autoEngine = classifyAndPick();
        active = autoEngine;
        autoLatched = true;          // set before feeding so chooseEngine() sticks
        active->reset();
        stretchFifo.reset();
        pendingPos = 0;
        replayPending();
    }
};

TimeStretcher::TimeStretcher() : impl(std::make_unique<Impl>()) {}
TimeStretcher::~TimeStretcher() = default;
TimeStretcher::TimeStretcher(TimeStretcher&&) noexcept = default;
TimeStretcher& TimeStretcher::operator=(TimeStretcher&&) noexcept = default;

void TimeStretcher::prepare(const StreamConfig& config)
{
    auto& p = *impl;
    p.config = config;

    const int ch = config.numChannels;

    p.prepareEngines();          // builds both engines at p.params.quality

    p.resampler.prepare(ch);

    // stretchFifo must absorb the largest burst the engine can emit for one input
    // block at the maximum combined stretch, plus a frame of headroom.
    const int maxEngineStretch = 8;
    const int fifoCap = config.maxBlockSize * maxEngineStretch + 8192;
    p.stretchFifo.prepare(ch, fifoCap);

    p.drainChunk  = config.maxBlockSize * maxEngineStretch + 16;
    p.resampChunk = static_cast<int>(std::ceil(config.maxBlockSize * kMaxPitchRatio)) + 16;

    auto makePlanar = [ch](std::vector<std::vector<Sample>>& buf, std::vector<Sample*>& ptrs, int len)
    {
        buf.assign(static_cast<size_t>(ch), std::vector<Sample>(static_cast<size_t>(len), Sample{0}));
        ptrs.resize(static_cast<size_t>(ch));
        for (int c = 0; c < ch; ++c)
            ptrs[static_cast<size_t>(c)] = buf[static_cast<size_t>(c)].data();
    };
    makePlanar(p.engineReadBuf, p.engineReadPtrs, p.drainChunk);
    makePlanar(p.resampBuf,     p.resampPtrs,     p.resampChunk);
    makePlanar(p.silenceBuf,    p.silencePtrs,    config.maxBlockSize);
    p.outOffsetPtrs.resize(static_cast<size_t>(ch));

    // Content-adaptive Auto probe. Everything it needs is sized here so the running
    // analysis in accumulateProbe() never allocates.
    p.probeHoldSamples = static_cast<int>(Impl::kProbeSeconds * config.sampleRate);
    p.pendingCap       = static_cast<int>(Impl::kPendingSeconds * config.sampleRate);
    makePlanar(p.pendingBuf, p.pendingPtrs, p.pendingCap);

    p.probeBins = Impl::kProbeFft / 2 + 1;
    p.probeHop  = Impl::kProbeFft / 4;
    p.probeK0   = std::max(1, static_cast<int>(std::ceil(200.0 * Impl::kProbeFft / config.sampleRate)));
    p.probeFft.setSize(Impl::kProbeFft, /*withRealSupport*/ false);
    p.probeMono.assign(static_cast<size_t>(Impl::kProbeFft), 0.0f);
    p.probeSpec.assign(static_cast<size_t>(Impl::kProbeFft), dsp::Complex{});
    p.probeWindow.assign(static_cast<size_t>(Impl::kProbeFft), 0.0f);
    for (int i = 0; i < Impl::kProbeFft; ++i)
        p.probeWindow[static_cast<size_t>(i)] =
            static_cast<float>(0.5 - 0.5 * std::cos(2.0 * M_PI * i / (Impl::kProbeFft - 1)));
    p.magRing.assign(static_cast<size_t>(Impl::kMedian),
                     std::vector<float>(static_cast<size_t>(p.probeBins), 0.0f));
    p.prevMag.assign(static_cast<size_t>(p.probeBins), 0.0f);
    p.medScratch.assign(static_cast<size_t>(Impl::kMedian), 0.0f);
    p.freqScratch.assign(static_cast<size_t>(Impl::kMedian), 0.0f);
    p.fluxSeq.clear();
    p.fluxSeq.reserve(static_cast<size_t>(p.pendingCap / p.probeHop) + 4);

    p.prepared = true;
    reset();

    // Re-apply parameters so the engine gets pitch context and smoothing snaps.
    setParameters(p.params);
}

void TimeStretcher::reset()
{
    auto& p = *impl;
    p.phaseVocoder.reset();
    p.wsola.reset();
    p.sinusoidal.reset();
    p.resampler.reset();
    p.stretchFifo.reset();
    p.smoothingInit = false;
    p.pendingFilled = 0;
    p.pendingPos    = 0;
    p.autoLatched   = false;
    p.autoEngine    = nullptr;

    // Probe accumulators.
    p.probeFilled  = 0;
    p.ringCount    = 0;
    p.ringHead     = 0;
    p.audibleHeld  = 0;
    p.probeStarted = false;
    p.eHarmonic = p.ePercussive = 0.0;
    p.sumL = p.sumR = p.sumLL = p.sumRR = p.sumLR = 0.0;
    p.corrCount = 0;
    p.fluxSeq.clear();
    std::fill(p.prevMag.begin(), p.prevMag.end(), 0.0f);
    for (auto& f : p.magRing)
        std::fill(f.begin(), f.end(), 0.0f);
}

void TimeStretcher::setParameters(const StretchParameters& params)
{
    auto& p = *impl;

    // The continuous/mode parameters are all real-time safe to change.
    p.params.timeRatio         = params.timeRatio;
    p.params.pitchRatio        = params.pitchRatio;
    p.params.preserveFormants  = params.preserveFormants;
    p.params.formantRatio      = params.formantRatio;
    p.params.preserveTransients = params.preserveTransients;
    p.params.engine            = params.engine;
    p.params.quality           = params.quality;

    // Quality is different: changing it resizes the engines' FFT/window, so it
    // reallocates. That is a setup-time operation (a host would not automate
    // quality per block), so we re-prepare here on a change — NOT real-time safe,
    // must be called off the audio thread. Doing it here (rather than only in
    // prepare()) means the common prepare()-then-setParameters(Best) order, and
    // any later quality switch, actually takes effect instead of silently running
    // at the quality prepare() happened to see first.
    if (p.prepared && params.quality != p.preparedQuality)
    {
        p.prepareEngines();          // rebuilds both engines at the new quality
        p.stretchFifo.reset();
        p.resampler.reset();
        p.smoothingInit = false;     // re-snap smoothing to the new targets below
    }

    p.targetTime  = clamp(params.timeRatio, 0.05, 20.0);
    p.targetPitch = clamp(params.pitchRatio, kMinPitchRatio, kMaxPitchRatio);

    if (!p.smoothingInit)
    {
        p.smoothedTime  = p.targetTime;
        p.smoothedPitch = p.targetPitch;
        p.smoothingInit = true;
    }

    engine::IStretchEngine* chosen = p.chooseEngine(p.params);
    if (chosen != p.active && p.active != nullptr)
    {
        // Engine switch: reset the newcomer and clear the bridge FIFO. A crossfade
        // between engines would be smoother and is a natural future addition.
        chosen->reset();
        p.stretchFifo.reset();
    }
    p.active = chosen;
    p.active->setPitchContext(p.targetPitch, p.params);

    // Leaving Auto before the probe latched would otherwise strand everything the
    // probe was holding, since only the Auto branch of pushInput replays it.
    if (p.prepared && p.params.engine != EngineType::Auto && !p.autoLatched && p.pendingFilled > 0)
        p.replayPending();      // queued, order-preserving, and never dropped
}

const StretchParameters& TimeStretcher::parameters() const noexcept
{
    return impl->params;
}

int TimeStretcher::pushInput(const AudioBlockView& input)
{
    auto& p = *impl;
    p.updateSmoothing();

    const int total = input.numSamples();
    int offset = 0;

    // Content-adaptive Auto: hold input while the probe listens, then latch an engine
    // and feed everything held to it. Until the latch, nothing goes downstream — the
    // hold is a real timeline shift, which latencySamples() reports.
    if (p.params.engine == EngineType::Auto && !p.autoLatched)
    {
        const int take = std::min(total, p.pendingCap - p.pendingFilled);
        for (int c = 0; c < p.config.numChannels; ++c)
            std::copy(input.channel(c), input.channel(c) + take,
                      p.pendingBuf[static_cast<size_t>(c)].begin() + p.pendingFilled);
        p.accumulateProbe(input.channelArray(), 0, take);
        p.pendingFilled += take;
        offset += take;

        // Latch on enough AUDIBLE material, or when the hold is full (a stream that
        // is all but silent must not be buffered forever).
        if (p.audibleHeld >= p.probeHoldSamples || p.pendingFilled >= p.pendingCap)
            p.latchAuto();
        else
            return total;   // still listening
    }

    // Anything still held from the probe has to go in first to keep stream order.
    if (p.pendingFilled > 0)
    {
        p.replayPending();
        if (p.pendingFilled > 0)
        {
            // Still backlogged — queue this block behind it rather than overtaking.
            // The backlog drains as the caller pulls output; if the caller pushes
            // without pulling until even the queue is full, the remainder falls
            // through to feedAndDrain and the engine's own backpressure applies.
            const int take = std::min(total - offset, p.pendingCap - p.pendingFilled);
            for (int c = 0; c < p.config.numChannels; ++c)
                std::copy(input.channel(c) + offset, input.channel(c) + offset + take,
                          p.pendingBuf[static_cast<size_t>(c)].begin() + p.pendingFilled);
            p.pendingFilled += take;
            offset += take;
        }
    }

    p.feedAndDrain(input.channelArray(), offset, total - offset);
    return total;
}

int TimeStretcher::availableOutput() const
{
    auto& p = *impl;
    // A conservative estimate: buffered stretched samples convertible at the
    // current pitch ratio, plus whatever the engine can still emit.
    const int fifoReady = p.stretchFifo.numReady();
    return p.resampler.outputAvailable(fifoReady);
}

int TimeStretcher::receiveOutput(const AudioBlockView& output)
{
    auto& p = *impl;
    const int ch = p.config.numChannels;
    const int wanted = output.numSamples();

    p.resampler.setRatio(p.smoothedPitch);

    int produced = 0;
    while (produced < wanted)
    {
        p.drainEngineToFifo();
        if (p.pendingFilled > 0)
            p.replayPending();   // consuming output frees room for the probe backlog

        const int avail = p.stretchFifo.numReady();
        if (avail < 4)          // need a few samples for the 4-point kernel
            break;

        const int chunk = std::min(avail, p.resampChunk);
        p.stretchFifo.peek(p.resampPtrs.data(), 0, chunk);

        for (int c = 0; c < ch; ++c)
            p.outOffsetPtrs[static_cast<size_t>(c)] = output.channel(c) + produced;

        const auto prog = p.resampler.process(p.resampPtrs.data(), chunk,
                                              p.outOffsetPtrs.data(), wanted - produced, ch);
        p.stretchFifo.discard(prog.inputConsumed);
        produced += prog.outputProduced;

        if (prog.outputProduced == 0 && prog.inputConsumed == 0)
            break;              // stuck — need more engine output than we have
    }
    return produced;
}

int TimeStretcher::process(const AudioBlockView& input, const AudioBlockView& output)
{
    pushInput(input);
    return receiveOutput(output);
}

int TimeStretcher::flush()
{
    auto& p = *impl;
    // If Auto never accumulated a full probe window (short stream), latch now on
    // whatever was buffered so the held input is not lost.
    if (p.params.engine == EngineType::Auto && !p.autoLatched && p.pendingFilled > 0)
        p.latchAuto();

    // Everything still held must reach the engine before the tail silence, or the
    // end of the stream would simply be missing. The caller drains as we go, which
    // is what frees the pipeline for the rest of the backlog.
    int guard = 0;
    while (p.pendingFilled > 0 && guard++ < 1024)
    {
        const int before = p.pendingFilled;
        p.replayPending();
        if (p.pendingFilled == before)
            break;                      // pipeline full — caller must consume output
    }

    // Push enough silence to evacuate the algorithmic latency of both stages.
    const int total = p.active->latencySamples() + p.config.maxBlockSize;
    int pushed = 0;
    while (pushed < total)
    {
        const int n = std::min(p.config.maxBlockSize, total - pushed);
        p.active->feed(p.silencePtrs.data(), 0, n);
        p.drainEngineToFifo();
        pushed += n;
    }
    return pushed;
}

int TimeStretcher::latencySamples() const
{
    auto& p = *impl;
    const int engineLatency = p.active ? p.active->latencySamples() : 0;
    // In Auto the probe holds input before anything is fed downstream, which shifts
    // the whole output timeline. Report it unconditionally in Auto (not just before
    // the latch) so the figure a host uses for delay compensation stays constant.
    return engineLatency + (p.params.engine == EngineType::Auto ? p.probeHoldSamples : 0);
}

const char* TimeStretcher::activeEngineName() const
{
    return impl->active ? impl->active->name() : "none";
}

// ---------------------------------------------------------------------------
// Offline convenience.
// ---------------------------------------------------------------------------

AudioBuffer processOffline(const AudioBuffer& input,
                           const StretchParameters& params,
                           double sampleRate,
                           Quality quality)
{
    const int ch = input.numChannels();
    const int inLen = input.numSamples();

    StreamConfig cfg;
    cfg.sampleRate   = sampleRate;
    cfg.numChannels  = ch;
    cfg.maxBlockSize = 1024;

    StretchParameters p = params;
    p.quality = quality;

    TimeStretcher ts;
    ts.prepare(cfg);
    ts.setParameters(p);

    // Generous output allocation: requested duration plus a comfortable tail.
    const int estimate = static_cast<int>(std::ceil(inLen * params.timeRatio)) + ts.latencySamples() * 2 + 4096;
    AudioBuffer out(ch, estimate);

    std::vector<std::vector<Sample>> inBlock(static_cast<size_t>(ch), std::vector<Sample>(static_cast<size_t>(cfg.maxBlockSize)));
    std::vector<Sample*> inPtrs(static_cast<size_t>(ch));
    for (int c = 0; c < ch; ++c)
        inPtrs[static_cast<size_t>(c)] = inBlock[static_cast<size_t>(c)].data();

    std::vector<Sample*> outPtrs(static_cast<size_t>(ch));

    int inPos = 0, outPos = 0;

    auto receiveInto = [&](int cap)
    {
        for (int c = 0; c < ch; ++c)
            outPtrs[static_cast<size_t>(c)] = out.channel(c) + outPos;
        AudioBlockView ov(outPtrs.data(), ch, std::min(cap, out.numSamples() - outPos));
        outPos += ts.receiveOutput(ov);
    };

    // Feed all input.
    const AudioBlockView inView(inPtrs.data(), ch, cfg.maxBlockSize);
    while (inPos < inLen)
    {
        const int n = std::min(cfg.maxBlockSize, inLen - inPos);
        for (int c = 0; c < ch; ++c)
            std::copy(input.channel(c) + inPos, input.channel(c) + inPos + n, inBlock[static_cast<size_t>(c)].data());
        ts.pushInput(AudioBlockView(inPtrs.data(), ch, n));
        inPos += n;
        receiveInto(out.numSamples() - outPos);
    }

    // Flush the tail.
    ts.flush();
    while (outPos < out.numSamples())
    {
        const int before = outPos;
        receiveInto(out.numSamples() - outPos);
        if (outPos == before)
            break;
    }

    // Trim to what was actually produced.
    AudioBuffer trimmed(ch, outPos);
    for (int c = 0; c < ch; ++c)
        std::copy(out.channel(c), out.channel(c) + outPos, trimmed.channel(c));
    return trimmed;
}

} // namespace chronos
