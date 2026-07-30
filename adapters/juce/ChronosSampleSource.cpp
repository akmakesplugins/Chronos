#include "ChronosSampleSource.h"

static float semitonesToRatio (float semitonesUp) noexcept
{
    if (semitonesUp == 0.0f)
        return 1.0f;

    auto oneSemitone = std::pow (2.0, 1.0 / 12.0);
    return (float) std::pow (oneSemitone, (double) semitonesUp);
}

ChronosSource::ChronosSource()
{
    createStretcher();
}

ChronosSource::~ChronosSource() = default;

void ChronosSource::createStretcher()
{
    // The mode control (shared with the élastique backend, so the two can be A/B'd
    // from the same UI) selects the engine rather than a vendor quality tier:
    //   0  phase vocoder at Best      — the default; stereo-coherent, transient-aware
    //   1  phase vocoder at Good      — same character, cheaper
    //   2  WSOLA                      — percussive material; no phasiness by design
    //   3  sinusoidal                 — sparse, purely tonal material
    switch (mode)
    {
        case 1:  params.engine = chronos::EngineType::PhaseVocoder; params.quality = chronos::Quality::Good; break;
        case 2:  params.engine = chronos::EngineType::Wsola;        params.quality = chronos::Quality::High; break;
        case 3:  params.engine = chronos::EngineType::Sinusoidal;   params.quality = chronos::Quality::High; break;
        default: params.engine = chronos::EngineType::PhaseVocoder; params.quality = chronos::Quality::Best; break;
    }

    chronos::StreamConfig cfg;
    cfg.sampleRate   = sampleRate;
    cfg.numChannels  = 2;
    cfg.maxBlockSize = kBlockSize;

    stretcher.prepare (cfg);
    stretcher.setParameters (params);
    prepared = true;

    inputBuffer.setSize (2, kBlockSize, false, false, true);
    outputBuffer.setSize (2, kBlockSize, false, false, true);
    inPtrs.resize (2);
    outPtrs.resize (2);
}

const char* ChronosSource::getEngineName() const
{
    return stretcher.activeEngineName();
}

void ChronosSource::setFormant (float f)
{
    formant = f;
}

void ChronosSource::setStart (int sn)
{
    jassert (sample != nullptr && sn >= 0 && sn < int (sample->getNumSamples()));

    if (sample != nullptr)
        start = juce::jlimit (0, int (sample->getNumSamples() - 1), sn);
    else
        start = sn;
}

void ChronosSource::setEnd (int en)
{
    jassert (sample != nullptr && en >= 0 && en <= int (sample->getNumSamples()));

    if (sample != nullptr)
        end = juce::jlimit (0, int (sample->getNumSamples()), en);
    else
        end = en;
}

void ChronosSource::setSample (juce::AudioSampleBuffer* s, double ssr)
{
    sample = s;
    sourceSampleRate = ssr;
    numChannels = int (s->getNumChannels());
}

void ChronosSource::setMode (int m)
{
    if (m != mode)
    {
        mode = m;
        createStretcher();
    }
}

void ChronosSource::reverseDirection()
{
    if (currentDirection == forward)
        currentDirection = reverse;
    else
        currentDirection = forward;
}

void ChronosSource::setSampleRate (double sr)
{
    if (sampleRate != sr)
    {
        sampleRate = sr;
        createStretcher();
    }
}

void ChronosSource::reset (bool initVars)
{
    if (prepared)
        stretcher.reset();

    fifo.reset();

    if (! initVars) return;

    start = 0;
    end = 0;
    numChannels = 0;
    readPosition = 0;
    mode = 0;
    finished = false;
    looping = false;
    pingpong = false;
    initialDirection = forward;
    currentDirection = forward;
    crossfade = 0.0f;
    pitch = 0.0f;
    timestretch = 1.0f;
    formant = 0.0f;
    sampleRate = 44100.0;
    sourceSampleRate = 44100.0;
}

void ChronosSource::noteOn()
{
    if (prepared)
        stretcher.reset();

    fifo.reset();

    finished = false;

    if (initialDirection == forward)
    {
        readPosition = start;
        currentDirection = initialDirection;
    }
    else
    {
        readPosition = end - 1;
        currentDirection = initialDirection;
    }
}

void ChronosSource::noteOff()
{
}

bool ChronosSource::isFinished()
{
    return finished;
}

int ChronosSource::readFromSampleForward (float* dataL, float* dataR, int numSamples)
{
    auto srcL = sample->getReadPointer (0);
    auto srcR = numChannels > 1 ? sample->getReadPointer (1) : srcL;

    auto crossfadeSamples = int (crossfade * sampleRate);

    for (int i = 0; i < numSamples; i++)
    {
        if (finished)
        {
            *dataL++ = 0;
            *dataR++ = 0;
        }
        else
        {
            auto sampleL = srcL[readPosition];
            auto sampleR = srcR[readPosition];

            if (looping && ! pingpong && crossfadeSamples > 0)
            {
                auto distanceFromEnd = end - readPosition;

                if (distanceFromEnd <= crossfadeSamples)
                {
                    auto fadeOutGain = distanceFromEnd / (float)crossfadeSamples;
                    auto fadeInGain = 1.0f - fadeOutGain;

                    auto crossfadeReadPos = start - distanceFromEnd;

                    if (crossfadeReadPos < end)
                    {
                        sampleL = sampleL * fadeOutGain + (crossfadeReadPos > 0 ? srcL[crossfadeReadPos] : 0) * fadeInGain;
                        sampleR = sampleR * fadeOutGain + (crossfadeReadPos > 0 ? srcR[crossfadeReadPos] : 0) * fadeInGain;
                    }
                }
            }

            *dataL++ = sampleL;
            *dataR++ = sampleR;
            readPosition++;
        }

        if (readPosition >= end && ! finished)
        {
            if (pingpong)
            {
                currentDirection = reverse;
                readPosition = end - 1;

                if (! looping && initialDirection == currentDirection)
                    finished = true;

                return i + 1;
            }
            else if (looping)
            {
                readPosition = start;
            }
            else
            {
                finished = true;
            }
        }
    }
    return numSamples;
}

int ChronosSource::readFromSampleReverse (float* dataL, float* dataR, int numSamples)
{
    auto srcL = sample->getReadPointer (0);
    auto srcR = numChannels > 1 ? sample->getReadPointer (1) : srcL;

    auto crossfadeSamples = int (crossfade * sampleRate);
    auto sampleLength = sample->getNumSamples();

    for (int i = 0; i < numSamples; i++)
    {
        if (finished)
        {
            *dataL++ = 0;
            *dataR++ = 0;
        }
        else
        {
            auto sampleL = srcL[readPosition];
            auto sampleR = srcR[readPosition];

            if (looping && ! pingpong && crossfadeSamples > 0)
            {
                auto distanceFromStart = readPosition - start;

                if (distanceFromStart < crossfadeSamples)
                {
                    auto fadeOutGain = distanceFromStart / (float)crossfadeSamples;
                    auto fadeInGain = 1.0f - fadeOutGain;

                    auto crossfadeReadPos = end + distanceFromStart;

                    if (crossfadeReadPos >= start)
                    {
                        sampleL = sampleL * fadeOutGain + (crossfadeReadPos < sampleLength ? srcL[crossfadeReadPos] : 0.0f) * fadeInGain;
                        sampleR = sampleR * fadeOutGain + (crossfadeReadPos < sampleLength ? srcR[crossfadeReadPos] : 0.0f) * fadeInGain;
                    }
                }
            }

            *dataL++ = sampleL;
            *dataR++ = sampleR;
            readPosition--;
        }

        if (readPosition < start && ! finished)
        {
            if (pingpong)
            {
                currentDirection = forward;
                readPosition = start;

                if (! looping && initialDirection == currentDirection)
                    finished = true;

                return i + 1;
            }
            else if (looping)
            {
                readPosition = end - 1;
            }
            else
            {
                finished = true;
            }
        }
    }
    return numSamples;
}

void ChronosSource::process (juce::AudioSampleBuffer& inOutBuffer)
{
    if (! prepared || sample == nullptr)
        return;

    auto dataL = inOutBuffer.getWritePointer (0);
    auto dataR = inOutBuffer.getWritePointer (1);

    int todo = inOutBuffer.getNumSamples();
    while (todo > 0)
    {
        if (fifo.getNumReady() > 0)
        {
            int thisBlock = juce::jmin (todo, fifo.getNumReady());

            float* rawData[] = { dataL, dataR };
            juce::AudioSampleBuffer readBuffer (rawData, 2, thisBlock);
            fifo.read (readBuffer);

            dataL += thisBlock;
            dataR += thisBlock;

            todo -= thisBlock;
        }
        else
        {
            // Same parameter maths as the élastique path, so the two backends respond
            // identically to the same controls. The sample's own rate mismatch is
            // folded into the pitch shift, and the stretch is corrected for it.
            float fileSpeedRatio = float (sourceSampleRate / sampleRate);
            const float resamplingPitchRatio = fileSpeedRatio > 0.0f ? (float) std::log2 (fileSpeedRatio) : 1.0f;
            float timestetchSpeedRatio = juce::jmax (0.1f, float (timestretch / fileSpeedRatio));
            float timestetchSemitonesUp = float ((pitch + (resamplingPitchRatio * 12.0f)));

            params.timeRatio  = timestetchSpeedRatio;
            params.pitchRatio = semitonesToRatio (timestetchSemitonesUp);

            // Formants. élastique takes an absolute envelope factor and is left at
            // 1.0 (untouched) whenever the control is centred; Chronos models the
            // same thing as "preserve, then optionally shift", so centred means the
            // envelope stage is simply off — which is also the cheaper path.
            //
            // NOTE: the off state matches élastique exactly; the calibration of
            // non-zero settings is Chronos's own (a signed semitone shift of the
            // envelope) and has not been matched to élastique's factor curve by ear.
            if (formant == 0.0f)
            {
                params.preserveFormants = false;
                params.formantRatio = 1.0;
            }
            else
            {
                params.preserveFormants = true;
                params.formantRatio = semitonesToRatio (formant);
            }

            stretcher.setParameters (params);

            // Top the engine up until it can give us a block. Each pass reads one
            // block of source (honouring loop/reverse/ping-pong) and pushes it in.
            int produced = 0;
            int guard = 0;
            while (produced == 0 && guard++ < 64)
            {
                int pos = 0;
                while (pos < kBlockSize)
                {
                    int done = [&]
                    {
                        if (currentDirection == forward)
                            return readFromSampleForward (inputBuffer.getWritePointer (0, pos),
                                                          inputBuffer.getWritePointer (1, pos),
                                                          kBlockSize - pos);

                        return readFromSampleReverse (inputBuffer.getWritePointer (0, pos),
                                                      inputBuffer.getWritePointer (1, pos),
                                                      kBlockSize - pos);
                    } ();

                    if (done <= 0)
                        break;

                    pos += done;
                }

                for (int c = 0; c < 2; ++c)
                {
                    inPtrs[size_t (c)]  = inputBuffer.getWritePointer (c);
                    outPtrs[size_t (c)] = outputBuffer.getWritePointer (c);
                }

                stretcher.pushInput (chronos::AudioBlockView (inPtrs.data(), 2, pos));
                produced = stretcher.receiveOutput (chronos::AudioBlockView (outPtrs.data(), 2, kBlockSize));
            }

            if (produced <= 0)
            {
                // The engine can give nothing more (source exhausted): pad with
                // silence rather than spinning, so the voice can finish cleanly.
                outputBuffer.clear();
                produced = juce::jmin (todo, kBlockSize);
            }

            juce::AudioSampleBuffer written (outputBuffer.getArrayOfWritePointers(), 2, produced);
            fifo.write (written);
        }
    }
}

float ChronosSource::getPosition()
{
    if (sample != nullptr)
        return float (readPosition) - float (stretcher.latencySamples());
    return 0.0f;
}
