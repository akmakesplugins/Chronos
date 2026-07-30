// Chronos — engine/PhaseVocoderEngine.h
//
// The high-quality, frequency-domain time-stretch engine. It is a phase vocoder
// with three refinements that lift it well above a textbook implementation:
//
//   1. Identity phase locking (Laroche & Dolson, 1999). A naive phase vocoder
//      advances every bin's phase independently, so bins belonging to one sinusoid
//      drift out of alignment and the result sounds "phasey"/watery. We instead
//      detect spectral peaks, advance only the peaks by their true frequency, and
//      rigidly lock the surrounding bins' phases to their peak. This preserves the
//      intra-sinusoid phase coherence and is the single biggest quality lever.
//
//   2. Transient preservation. Attacks are the hardest thing for a phase vocoder:
//      every analysis frame whose window merely OVERLAPS a coming onset already
//      contains the attack, and re-rendering that frame spreads its energy across
//      the frame's whole output span — i.e. backward in time, as audible pre-echo
//      up to a window before the hit. A phase reset on the onset frame cannot undo
//      that, because the damage is already in the preceding frames. So instead we
//      (a) find onsets sample-accurately in the time domain, (b) gate the coming
//      attack out of the frames that would smear it, (c) re-time the hops so the
//      attack lands at exactly its stretched position, and (d) splice the first few
//      milliseconds of the attack in verbatim, since the window taper cannot carry
//      a full-level attack at a frame edge anyway.
//
//   3. Formant correction. When asked, we estimate the spectral envelope and pre-
//      distort the magnitude so that, after the facade's downstream resampling, the
//      formants land where the user wants (preserved, or shifted).
//
//   4. Stereo-coherent phase advance. Phase is propagated ONCE per frame on a
//      mono (complex-sum) reference, and each channel re-applies its own
//      analysis-phase offset at synthesis. Advancing each channel independently —
//      as this engine used to — lets the tiny L/R differences in the per-channel
//      frequency estimates integrate into an unbounded inter-channel phase drift,
//      which is heard as the stereo image swimming, combing, and "phasing" on
//      sustained material. Sharing the reference makes the output's inter-channel
//      phase difference equal the input's on every frame, by construction.
//
// The engine does time-stretch only; pitch is the facade's resampler. Reconstruction
// uses normalised weighted overlap-add (a parallel window-energy accumulator), so
// gain is exactly unity for any — even time-varying — synthesis hop.

#pragma once

#include <cstdint>
#include <vector>

#include "IStretchEngine.h"
#include "../analysis/SpectralEnvelope.h"
#include "../analysis/TransientDetector.h"
#include "../dsp/AudioFifo.h"
#include "../dsp/Fft.h"
#include "../dsp/Window.h"

namespace chronos::engine
{

class PhaseVocoderEngine final : public IStretchEngine
{
public:
    PhaseVocoderEngine() = default;

    void prepare(const EngineConfig& config) override;
    void reset() override;
    void setTimeStretch(double ratio) override;
    void setPitchContext(double pitchRatio, const StretchParameters& params) override;

    int  feed(const Sample* const* input, int offset, int numFrames) override;
    [[nodiscard]] int available() const override { return outputFifo.numReady(); }
    int  read(Sample* const* output, int offset, int numFrames) override;

    [[nodiscard]] int latencySamples() const override { return frameSize; }
    [[nodiscard]] const char* name() const override { return "PhaseVocoder"; }

private:
    // Process as many analysis frames as the buffered input and free output allow.
    void processFrames();
    // Analyse + resynthesise one frame for all channels (input already peeked).
    // `gateIdx` >= 0 fades out everything from that frame offset (a coming onset);
    // `resetNow` means this frame starts exactly on an onset.
    void processOneFrame(int gateIdx, bool resetNow, int hopIn, double hopOut);
    // Scan the freshly-arrived region of the peeked frame for a sample-accurate
    // onset and queue it.
    void detectOnsets();
    // Capture the verbatim attack region for the head onset once it is fully inside
    // the peeked frame.
    void captureSplice();
    // Advance the SHARED (mono-reference) synthesis phase for this frame.
    void advanceSharedPhase(bool resetNow);
    // Per-channel spectral magnitude shaping + shared-phase application + OLA.
    void reconstructChannel(int ch);
    // Emit finished overlap-add samples and slide the accumulator.
    void emit(double synthesisHop);
    void locatePeaks(const Sample* magnitudes);
    // Flag the bins that identity locking would decorrelate, and set this frame's
    // random rotations + power compensation for them.
    void classifyNoiseBins(bool resetNow, int hopIn, double hopOut);

    EngineConfig config;
    int frameSize = 2048;
    int analysisHop = 512;
    int halfBins = 1025;
    int overlapFactor = 4;
    bool phaseLockEnabled = true;

    double timeStretch = 1.0;
    double maxTimeStretch = 3.8;

    // Formant state (see setPitchContext / reconstructChannel).
    bool   formantActive = false;
    double pitchRatio = 1.0;
    double formantScale = 1.0;   ///< s_total in the envelope-correction derivation.

    dsp::Fft    fft;
    dsp::Window analysisWindow;
    dsp::Window synthesisWindow;

    analysis::TransientDetector transientDetector;
    std::vector<analysis::SpectralEnvelope> envelopes; ///< One per channel.

    dsp::AudioFifo inputFifo;
    dsp::AudioFifo outputFifo;

    // Per-channel spectral state. Outer index = channel.
    std::vector<std::vector<dsp::Complex>> spectrum;   ///< [ch][frameSize]
    std::vector<std::vector<float>> magnitude;         ///< [ch][halfBins]
    std::vector<std::vector<float>> analysisPhase;     ///< [ch][halfBins]
    // Shared (mono-sum reference) phase state — ONE per stream, not per channel.
    std::vector<dsp::Complex> monoSpec;      ///< [halfBins] sum of channel spectra
    std::vector<float> aphMono;              ///< [halfBins] mono analysis phase
    std::vector<float> prevAphMono;          ///< [halfBins]
    std::vector<float> sharedSynthPhase;     ///< [halfBins] shared synthesis phase

    std::vector<float> trueFreq;    ///< [halfBins] scratch (per channel, reused)
    std::vector<float> detectMag;   ///< [halfBins] summed magnitude for detection
    std::vector<int>   peakOf;      ///< [halfBins] nearest peak index per bin
    std::vector<int>   peaks;       ///< peak bin indices (reused)

    // Noise-bin handling (see classifyNoiseBins). Bins that identity locking would
    // decorrelate are instead decorrelated deliberately and power-compensated.
    static constexpr int   kRandChunk      = 32;      ///< Bins sharing one rotation.
    static constexpr int   kMainLobeBins   = 6;       ///< Max distance to count as a peak's lobe.
    static constexpr float kPeakProminence = 4.0f;    ///< Peak/off-lobe ratio of a real partial.
    static constexpr double kHannM2 = 0.375;          ///< mean(w^2) for Hann analysis*synthesis.
    static constexpr double kHannM4 = 35.0 / 128.0;   ///< mean(w^4).

    std::vector<std::uint8_t> noiseBin;   ///< [halfBins] 1 = decorrelate this bin
    std::vector<float> randOffset;        ///< [halfBins] this frame's chunk rotations
    bool  frameRandomize = false;         ///< Whether noise handling applies this frame.
    float compGain = 1.0f;                ///< Incoherent-WOLA power compensation.
    std::uint32_t rngState = 0x9e3779b9u; ///< xorshift32 state (deterministic per stream).

    // Overlap-add reconstruction. Two window-energy accumulators: `normBuffer`
    // follows the gate mask (so ungated frames are normalised back to full level
    // past a cut), `normFullBuffer` is the unmasked sum used to bound how much the
    // gate compensation may boost.
    std::vector<std::vector<Sample>> olaBuffer; ///< [ch][olaLength]
    std::vector<Sample> normBuffer;             ///< [olaLength] gate-masked
    std::vector<Sample> normFullBuffer;         ///< [olaLength] unmasked
    int olaLength = 0;
    double hopRemainder = 0.0;      ///< Fractional part of the synthesis hop.

    // Scratch for reading a frame and emitting samples via the FIFOs.
    std::vector<std::vector<Sample>> frameScratch; ///< [ch][frameSize]
    std::vector<Sample*> framePtrs;
    std::vector<std::vector<Sample>> emitScratch;  ///< [ch][maxSynthHop]
    std::vector<Sample*> emitPtrs;

    bool primed = false;   ///< False until the first frame initialises phases.

    // ---- Transient preservation --------------------------------------------
    // Detection constants. The attack window is deliberately short and the context
    // window long: a genuine attack is a step in FIRST-DIFFERENCE energy, whereas a
    // kick body's low-frequency cycle wobble — which fools a plain short/long energy
    // ratio — is largely differentiated away.
    static constexpr int   kAttackWin   = 64;    ///< ~1.5 ms
    static constexpr int   kContextWin  = 768;   ///< ~17 ms
    static constexpr float kOnsetRatio  = 5.0f;  ///< attack/context energy step
    static constexpr int   kMaxPending  = 4;
    static constexpr int   kMaxSplices  = 3;
    static constexpr float kNormEpsSq   = 2.5e-3f; ///< Tikhonov term in the OLA normaliser.

    long long inputAbs    = 0;    ///< Absolute input index of the current frame start.
    long long outEmittedAbs = 0;  ///< Absolute count of output samples emitted.
    int    prevHopIn      = 0;    ///< Input hop that led INTO the current frame.
    double prevHopOut     = 0.0;  ///< Output spacing from the previous frame to this one.
    double debtOut        = 0.0;  ///< Output samples owed, redistributed over later frames.
    long long lastOnsetAbs = -1;
    int    refractorySamples = 0;
    int    unityLeft      = 0;    ///< Unity-rate frames remaining after an onset.
    int    unityFrames    = 1;    ///< Unity-rate frames held per onset.

    /// An onset that has been detected but not yet reached by the frame cursor.
    struct PendingOnset
    {
        long long pAbs = 0;          ///< Onset position (absolute input sample).
        double prepayRemaining = 0.0;///< Output samples still to pre-pay before it.
        int  unityApproach = -1;     ///< Unity-rate approach frames (latched once).
        int  spliceSlot = -1;        ///< Captured verbatim-attack slot, or -1.
    };
    PendingOnset onsetQ[kMaxPending];
    int onsetCount = 0;

    /// A captured attack, played verbatim over the vocoder's rendering of it.
    struct Splice
    {
        bool used = false;
        long long outStart = 0;      ///< Absolute output sample of the splice start.
        int length = 0;
        int preRoll = 0;
    };
    Splice splices[kMaxSplices];
    std::vector<std::vector<Sample>> spliceBufs[kMaxSplices]; ///< [slot][ch][spliceLen]
    int spliceLen    = 0;   ///< Total captured length.
    int splicePreRoll = 0;  ///< How much of it precedes the onset.
    int spliceFadeIn = 0;
    int spliceHold   = 0;   ///< End of the unity-weight region.

    std::vector<double> monoPrefix;  ///< [frameSize+1] diff-energy prefix for detection.
};

} // namespace chronos::engine
