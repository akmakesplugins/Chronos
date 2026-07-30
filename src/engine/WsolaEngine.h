// Copyright (c) 2026 ZROLogic. All rights reserved. Proprietary and confidential.
// Chronos — engine/WsolaEngine.h
//
// Waveform-Similarity Overlap-Add: a time-domain time-stretcher. For each output
// segment it searches a small tolerance window around the nominal input position
// for the segment whose overlap region best matches the *natural continuation* of
// the previously placed segment (via normalised cross-correlation), then overlap-
// adds it. Because it never leaves the time domain it introduces no phasiness and
// preserves transients essentially for free — which makes it the better choice for
// percussive and monophonic material, and far cheaper than the phase vocoder.
//
// It does not model formants, so the facade only selects it when formant work is
// not requested. Pitch is still handled by the facade's resampler downstream.

#pragma once

#include <vector>

#include "IStretchEngine.h"
#include "../dsp/AudioFifo.h"
#include "../dsp/Window.h"

namespace chronos::engine
{

class WsolaEngine final : public IStretchEngine
{
public:
    WsolaEngine() = default;

    void prepare(const EngineConfig& config) override;
    void reset() override;
    void setTimeStretch(double ratio) override;
    void setPitchContext(double pitchRatio, const StretchParameters& params) override;

    int  feed(const Sample* const* input, int offset, int numFrames) override;
    [[nodiscard]] int available() const override { return outputFifo.numReady(); }
    int  read(Sample* const* output, int offset, int numFrames) override;

    [[nodiscard]] int latencySamples() const override { return frameSize; }
    [[nodiscard]] const char* name() const override { return "WSOLA"; }

private:
    void processSegments();
    int  findBestOffset(int nominalStart);   // returns delta in [-tolerance, +tolerance]
    int  findOnset(int lo, int hi);          // strongest onset index in the work window, or -1
    void placeSegment(int chosenStart);

    EngineConfig config;
    int frameSize = 1024;   ///< Segment length L.
    int synthHop  = 512;    ///< Fixed output hop Hs = L/2 (50% overlap Hann).
    int overlap   = 512;    ///< L - Hs.
    int tolerance = 256;    ///< Cross-correlation search half-range.

    double timeStretch  = 1.0;
    double minStretch   = 0.25;
    double maxStretch   = 4.0;
    double nominalPos   = 0.0;   ///< Fractional nominal input read position in the FIFO.
    double analysisHop  = 512.0; ///< Ha = Hs / timeStretch.

    dsp::Window window;
    dsp::AudioFifo inputFifo;
    dsp::AudioFifo outputFifo;

    // Work buffers.
    std::vector<std::vector<Sample>> workWindow; ///< [ch][workLen] peeked input
    std::vector<Sample*> workPtrs;
    int workLen = 0;

    std::vector<Sample> templateSeg;   ///< Natural-continuation template (mono, overlap len).
    std::vector<double> onsetPrefix;   ///< [workLen+1] prefix-sum of mono energy for onset search.
    bool hasTemplate = false;
    float templateEnergy = 0.0f;       ///< For normalising the similarity score to [0,1].
    float lastCorr = 1.0f;             ///< Achieved similarity of the last placement.
    int  onsetHold = 0;                ///< Segments to wait before onset-locking again (refractory).

    std::vector<std::vector<Sample>> olaBuffer; ///< [ch][olaLength]
    std::vector<Sample> normBuffer;             ///< [olaLength] window sum   (coherent case)
    std::vector<Sample> normSqBuffer;           ///< [olaLength] window^2 sum (incoherent case)
    int olaLength = 0;

    std::vector<std::vector<Sample>> emitScratch; ///< [ch][synthHop]
    std::vector<Sample*> emitPtrs;
};

} // namespace chronos::engine
