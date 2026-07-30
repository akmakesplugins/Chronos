// Copyright (c) 2026 ZROLogic. All rights reserved. Proprietary and confidential.
// Chronos — adapters/juce/ChronosJuceStretcher.h
//
// A thin JUCE glue layer over chronos::TimeStretcher. It is header-only and lives
// outside the core CMake target so the DSP library stays framework-free; include
// this file only from a JUCE target.
//
// It presents an élastique-style surface (semitone pitch, stretch ratio, formant
// semitones, planar float I/O) so it can slot into an existing voice/source chain
// like Sonic Atlas's ElastiqueSource with minimal rewiring. Two usage shapes:
//
//   * Streaming a live signal: prepare(), then per block pushInput()/receiveOutput()
//     (or the convenience process()).
//   * Driving from a sample source (loop/reverse/position handled by the caller):
//     feed the source chunk-by-chunk with pushInput() and pull fixed output blocks
//     with receiveOutput(); see fillFromSource() for a ready-made pull loop.
//
// Wrap the audio-thread calls in juce::ScopedNoDenormals in your processBlock; the
// core is otherwise real-time safe (all allocation happens in prepare()).

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <chronos/Chronos.h>

#include <functional>

namespace chronos::juce_adapter
{

class ChronosJuceStretcher
{
public:
    ChronosJuceStretcher() = default;

    /// Allocate for the given stream. Call from prepareToPlay().
    void prepare(double sampleRate, int maxBlockSize, int numChannels,
                 Quality quality = Quality::High)
    {
        StreamConfig cfg;
        cfg.sampleRate   = sampleRate;
        cfg.numChannels  = numChannels;
        cfg.maxBlockSize = maxBlockSize;

        params.quality = quality;
        stretcher.prepare(cfg);
        stretcher.setParameters(params);

        channelPtrs.resize(static_cast<size_t>(numChannels));
        numChannels_ = numChannels;
    }

    void reset() { stretcher.reset(); }

    // -- élastique-style parameter setters (safe on the audio thread) ----------

    void setTimeStretch(double ratio)         { params.timeRatio = ratio;              push(); }
    void setPitchSemitones(double semitones)  { params.setPitchSemitones(semitones);   push(); }
    void setFormantSemitones(double semitones){ params.setFormantSemitones(semitones); push(); }
    void setPreserveFormants(bool on)         { params.preserveFormants = on;          push(); }
    void setPreserveTransients(bool on)       { params.preserveTransients = on;        push(); }
    void setEngine(EngineType e)              { params.engine = e;                     push(); }

    [[nodiscard]] int latencySamples() const { return stretcher.latencySamples(); }
    [[nodiscard]] const char* engineName() const { return stretcher.activeEngineName(); }

    // -- Streaming I/O ---------------------------------------------------------

    /// Feed an input block. Returns samples accepted (normally all).
    int pushInput(const juce::AudioBuffer<float>& input, int numSamples)
    {
        fillPtrs(input);
        return stretcher.pushInput(AudioBlockView(channelPtrs.data(), numChannels_, numSamples));
    }

    /// Pull up to `numSamples` finished samples into `output`. Returns samples written.
    int receiveOutput(juce::AudioBuffer<float>& output, int numSamples)
    {
        fillPtrs(output);
        return stretcher.receiveOutput(AudioBlockView(channelPtrs.data(), numChannels_, numSamples));
    }

    /// Fill `output` (numSamples) by repeatedly pulling stretched audio and, when
    /// the pipeline needs more, requesting input from `provideInput`. The callback
    /// receives a scratch buffer to fill with up to `n` source samples and returns
    /// how many it wrote (0 = source exhausted). This mirrors ElastiqueSource's
    /// pull model where looping/reverse/position live in the caller.
    void fillFromSource(juce::AudioBuffer<float>& output, int numSamples,
                        const std::function<int(juce::AudioBuffer<float>&, int)>& provideInput)
    {
        int produced = receiveOutput(output, numSamples);
        while (produced < numSamples)
        {
            const int want = juce::jmin(sourceScratch.getNumSamples(), numSamples);
            const int got = provideInput(sourceScratch, want);
            if (got <= 0)
                break;                       // source exhausted; leave the rest silent
            pushInput(sourceScratch, got);

            // Pull into the remaining region of `output`.
            juce::AudioBuffer<float> tail(output.getArrayOfWritePointers(), output.getNumChannels(),
                                          produced, numSamples - produced);
            produced += receiveOutput(tail, numSamples - produced);
        }
    }

    /// Convenience: process one block in the common "same size in, same size out"
    /// live-effect case. Output may lag input by the latency during priming.
    int process(const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output, int numSamples)
    {
        pushInput(input, numSamples);
        return receiveOutput(output, numSamples);
    }

    /// Optional scratch used by fillFromSource(); size it in prepare() of the host
    /// if you use that path. Defaults to maxBlockSize when prepared here.
    juce::AudioBuffer<float>& sourceScratchBuffer() { return sourceScratch; }

private:
    void push() { stretcher.setParameters(params); }

    void fillPtrs(const juce::AudioBuffer<float>& buf)
    {
        for (int c = 0; c < numChannels_; ++c)
            channelPtrs[static_cast<size_t>(c)] =
                const_cast<float*>(buf.getReadPointer(juce::jmin(c, buf.getNumChannels() - 1)));
    }

    TimeStretcher stretcher;
    StretchParameters params;
    std::vector<float*> channelPtrs;
    juce::AudioBuffer<float> sourceScratch { 2, 4096 };
    int numChannels_ = 2;
};

} // namespace chronos::juce_adapter
