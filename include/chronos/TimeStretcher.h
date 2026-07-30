// Chronos — TimeStretcher.h
//
// The public facade. It presents a small, stable, framework-agnostic API and hides
// every implementation detail (engines, FFT, resampler) behind a pimpl, so the
// header pulls in nothing but the value types. It is responsible for:
//
//   * realising pitch shift as "time-stretch (engine) then resample",
//   * choosing / owning the time-stretch engine,
//   * smoothing continuous parameter changes to avoid zipper noise,
//   * the streaming feed/receive plumbing and an offline convenience path.
//
// Threading: prepare()/reset() allocate and are not real-time safe. Everything
// else — setParameters(), pushInput(), receiveOutput(), process() — is allocation-
// and lock-free and safe to call from the audio thread.

#pragma once

#include <memory>

#include "AudioBuffer.h"
#include "StretchParameters.h"
#include "Types.h"

namespace chronos
{

class TimeStretcher
{
public:
    TimeStretcher();
    ~TimeStretcher();

    TimeStretcher(TimeStretcher&&) noexcept;
    TimeStretcher& operator=(TimeStretcher&&) noexcept;
    TimeStretcher(const TimeStretcher&) = delete;
    TimeStretcher& operator=(const TimeStretcher&) = delete;

    /// Allocate for the given stream. Call before any processing; not RT-safe.
    void prepare(const StreamConfig& config);

    /// Clear all buffers and history without reallocating.
    void reset();

    /// Update time/pitch/formant/engine settings. RT-safe; continuous values are
    /// smoothed internally.
    void setParameters(const StretchParameters& params);

    [[nodiscard]] const StretchParameters& parameters() const noexcept;

    // -- Streaming API (real-time safe) ----------------------------------------

    /// Feed a planar input block into the pipeline. Returns the number of input
    /// samples accepted (normally all of them).
    int pushInput(const AudioBlockView& input);

    /// Final output samples ready to be received.
    [[nodiscard]] int availableOutput() const;

    /// Pull up to `output.numSamples()` finished samples into `output`. Returns the
    /// number actually written; the remainder of `output` is left untouched.
    int receiveOutput(const AudioBlockView& output);

    /// Convenience: pushInput(input) then receiveOutput(output). Returns output
    /// samples produced this call. Because stretching decouples input and output
    /// rates, this may be fewer than requested early on (latency priming) — check
    /// the return value.
    int process(const AudioBlockView& input, const AudioBlockView& output);

    /// Push trailing silence to flush the algorithmic latency at end-of-stream, so
    /// the final tail can be received. Offline use; returns silence samples pushed.
    int flush();

    // -- Introspection ---------------------------------------------------------

    /// Algorithmic latency in samples (for host plugin-delay-compensation at unity
    /// time ratio). Valid after prepare().
    [[nodiscard]] int latencySamples() const;

    /// Name of the active engine ("PhaseVocoder" / "WSOLA"). Diagnostics/UI.
    [[nodiscard]] const char* activeEngineName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/// Offline convenience: stretch/pitch an entire buffer in one call and return the
/// result. Handles priming and tail flushing. Not real-time safe (it allocates the
/// result). Ideal for pre-rendering.
[[nodiscard]] AudioBuffer processOffline(const AudioBuffer& input,
                                         const StretchParameters& params,
                                         double sampleRate,
                                         Quality quality = Quality::High);

} // namespace chronos
