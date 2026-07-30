// Chronos — engine/IStretchEngine.h
//
// The strategy interface every time-stretch algorithm implements. The facade
// (TimeStretcher) talks only to this, so algorithms can be added or swapped
// without touching the public API or the pitch/format/streaming plumbing.
//
// Contract and division of labour:
//   * An engine performs TIME-STRETCH ONLY — it changes duration while preserving
//     pitch. Pitch shifting is realised by the facade as "stretch then resample",
//     which keeps every engine simple and uniform.
//   * The engine is nonetheless *told* the net pitch ratio (setPitchContext) so it
//     can preserve or shift formants correctly, since formants must be corrected in
//     the spectral domain before the facade's resampler runs.
//   * Streaming is a feed/consume FIFO model: push any amount of input, then drain
//     whatever output is ready. All allocation happens in prepare(); feed()/read()
//     are real-time safe.

#pragma once

#include "chronos/StretchParameters.h"
#include "chronos/Types.h"

namespace chronos::engine
{

struct EngineConfig
{
    double sampleRate  = 44100.0;
    int    numChannels = 2;
    Quality quality    = Quality::Good;
    int    maxBlockSize = 512;   ///< Largest feed()/read() the facade will issue.
};

class IStretchEngine
{
public:
    virtual ~IStretchEngine() = default;

    /// Allocate all working memory. Not real-time safe.
    virtual void prepare(const EngineConfig& config) = 0;

    /// Return to a clean state (clears buffers, phase history) without reallocating.
    virtual void reset() = 0;

    /// Time-stretch factor = output duration / input duration. Safe to change per
    /// block; engines respond on the next synthesis frame.
    virtual void setTimeStretch(double ratio) = 0;

    /// Inform the engine of the net pitch ratio the facade will apply downstream,
    /// plus the formant controls, so formants can be preserved/shifted spectrally.
    virtual void setPitchContext(double pitchRatio, const StretchParameters& params) = 0;

    // -- Streaming (real-time safe) --------------------------------------------

    /// Push `numFrames` planar input samples. Returns frames accepted; if the
    /// engine's input buffer is momentarily full the caller should drain output and
    /// retry with the remainder.
    virtual int feed(const Sample* const* input, int offset, int numFrames) = 0;

    /// Output frames currently ready to read.
    [[nodiscard]] virtual int available() const = 0;

    /// Pop up to `numFrames` planar output samples. Returns frames actually read.
    virtual int read(Sample* const* output, int offset, int numFrames) = 0;

    /// Algorithmic latency in samples (frame size for spectral engines). Used for
    /// host plugin-delay-compensation at unity ratio.
    [[nodiscard]] virtual int latencySamples() const = 0;

    [[nodiscard]] virtual const char* name() const = 0;
};

} // namespace chronos::engine
