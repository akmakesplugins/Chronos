// Copyright (c) 2026 ZROLogic. All rights reserved. Proprietary and confidential.
// Chronos — engine/SinusoidalEngine.h
//
// A sinusoidal-modelling time-stretch engine (McAulay–Quatieri partial tracking
// with a quadratic-phase additive oscillator bank). It is the engine that beats a
// phase vocoder — and élastique — on two axes:
//
//   * Mono tonal purity: each partial becomes ONE pure oscillator, so all its
//     energy sits at exactly the right frequency with zero window/bin leakage.
//   * Real, non-stationary polyphony: partials are tracked frame-to-frame and
//     resynthesised along their trajectories, so vibrato / glissando / inharmonic
//     content stays clean where a phase vocoder's stationary-peak locking turns
//     watery.
//
// The pivotal simplification: the engine only *time-stretches* (pitch is preserved
// and handled downstream). Because pitch is preserved, an oscillator's per-output
// sample phase increment is exactly the analysed frequency ω (rad/sample) — the
// same as the input. Time-stretching therefore only changes HOW MANY output
// samples we render between analysis frames, not the oscillator's rate. So the sine
// path has no overlap-add, no window, and no normalisation: the oscillator bank is
// the signal, rendered contiguously and emitted whole each frame.
//
// v1 models the sinusoidal part only (no stochastic residual); it is routed by the
// facade for sparse/expressive tonal material, with the phase vocoder kept for
// dense mixes and WSOLA for percussion. All allocation happens in prepare(); the
// process() path is allocation- and lock-free.

#pragma once

#include <vector>

#include "IStretchEngine.h"
#include "../analysis/SpectralEnvelope.h"
#include "../analysis/TransientDetector.h"
#include "../dsp/AudioFifo.h"
#include "../dsp/Fft.h"
#include "../dsp/Window.h"

namespace chronos::engine
{

class SinusoidalEngine final : public IStretchEngine
{
public:
    SinusoidalEngine() = default;

    void prepare(const EngineConfig& config) override;
    void reset() override;
    void setTimeStretch(double ratio) override;
    void setPitchContext(double pitchRatio, const StretchParameters& params) override;

    int  feed(const Sample* const* input, int offset, int numFrames) override;
    [[nodiscard]] int available() const override { return outputFifo.numReady(); }
    int  read(Sample* const* output, int offset, int numFrames) override;

    [[nodiscard]] int latencySamples() const override { return frameSize; }
    [[nodiscard]] const char* name() const override { return "Sinusoidal"; }

private:
    void processFrames();
    void processOneFrame();
    void pickPeaks();
    void matchTracks();
    void renderPartials(int hopSamples);
    [[nodiscard]] float sineLookup(double phase) const noexcept;

    static constexpr int   kSineTableSize = 4096;   // power of two
    static constexpr float kFloorRel      = 1.0e-3f;
    static constexpr float kMatchBins     = 1.5f;

    EngineConfig config;
    int frameSize = 2048, analysisHop = 512, halfBins = 1025, overlapFactor = 4, numCh = 2;
    int maxTracks = 128, maxPeaks = 128, maxSynthHop = 0;
    double timeStretch = 1.0, maxTimeStretch = 20.0, hopAccum = 0.0;
    float  sinAmpScale = 1.0f;   ///< 2/Σw : converts a |X| main-lobe peak to oscillator amplitude.
    bool   primed = false;

    // Formant/pitch context (parsed like PV; formant warp is a later phase).
    bool   formantActive = false;
    double pitchRatio = 1.0, formantScale = 1.0;

    dsp::Fft    fft;
    dsp::Window analysisWindow;
    analysis::TransientDetector transientDetector;
    std::vector<analysis::SpectralEnvelope> envelopes;

    dsp::AudioFifo inputFifo, outputFifo;

    // Per-channel spectra.
    std::vector<std::vector<dsp::Complex>> spectrum;   ///< [ch][frameSize]
    std::vector<std::vector<float>>        magnitude;  ///< [ch][halfBins]
    std::vector<float> detectMag;                      ///< [halfBins] Σ_ch |X| (shared peak pick)

    // Peaks this frame: shared frequency, per-channel amplitude/phase.
    int numPeaks = 0;
    std::vector<float> peakFreq;    ///< [maxPeaks] rad/sample, ascending
    std::vector<int>   peakBin;     ///< [maxPeaks] integer bin k0
    std::vector<float> peakAmpCh;   ///< [maxPeaks*numCh] oscillator amplitude (already *sinAmpScale)
    std::vector<float> peakPhaseCh; ///< [maxPeaks*numCh] arg(X[k0]) per channel (births)
    std::vector<int>   peakOrder;   ///< [maxPeaks] scratch for loudness trim
    std::vector<int>   peakMatched; ///< [maxPeaks] track slot, or -1

    // Fixed-capacity track table (the real-time-safe core).
    struct Track { bool active; float freq; };
    std::vector<Track>  tracks;        ///< [maxTracks]
    std::vector<double> trackPhase;    ///< [maxTracks*numCh] double phase accumulator
    std::vector<float>  trackAmp;      ///< [maxTracks*numCh]
    std::vector<int>    trackMatch;    ///< [maxTracks] matched peak, or -1
    std::vector<int>    activeIdx;     ///< [maxTracks] active slots (sorted by freq during match)
    int numActive = 0;
    std::vector<int>    freeList;      ///< [maxTracks] free-slot stack
    int freeTop = 0;

    // Synthesis.
    std::vector<float> sineTable;                  ///< [kSineTableSize+1]
    std::vector<std::vector<Sample>> sineOut;      ///< [ch][maxSynthHop] per-frame render
    std::vector<Sample*> sineOutPtrs;

    // Frame I/O scratch.
    std::vector<std::vector<Sample>> frameScratch; ///< [ch][frameSize]
    std::vector<Sample*> framePtrs;
};

} // namespace chronos::engine
