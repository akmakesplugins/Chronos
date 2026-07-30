// Copyright (c) 2026 ZROLogic. All rights reserved. Proprietary and confidential.
#pragma once

#include <JuceHeader.h>

#include <chronos/Chronos.h>

/**
    Sample-playback source backed by the in-house Chronos time-stretch engine.

    A drop-in replacement for ElastiqueSource: identical public interface, identical
    looping / ping-pong / reverse / crossfade behaviour (that logic is the caller's
    and is reproduced here verbatim), only the stretching engine differs.

    The one structural difference is the direction of the plumbing. élastique is
    pull-driven — it says how many input frames it wants, you hand them over, it
    hands a fixed block back. Chronos is a push/pull FIFO: input goes in whenever
    there is room, output comes out whenever it is ready. So instead of asking
    GetFramesNeeded() we top the engine up until it can satisfy the block.

    The Auto engine is deliberately NOT used here. Auto holds half a second of input
    to listen to the material before choosing, which is fine offline but would be an
    audible delay on every note-on. A sampler already knows what it is playing, so
    the engine is chosen explicitly from the mode control instead.
*/
class ChronosSource
{
public:
    ChronosSource();
    ~ChronosSource();

    enum Direction
    {
        forward,
        reverse,
    };

    void setSample (juce::AudioSampleBuffer* sample, double sourceSampleRate);

    void setSampleRate (double sr);
    void setLooping (bool l)                { looping = l;          }
    void setInitialDirection (Direction d)  { initialDirection = d; }
    void setPingPong (bool p)               { pingpong = p;         }
    void setPosition (int p)                { readPosition = p;     }
    void setPitch (float p)                 { pitch = p;            }
    void setTimeStretch (float t)           { timestretch = t;      }
    void setCrossfade (float c)             { crossfade = c;        }
    void setFormant (float f);
    void setMode (int m);

    void reverseDirection();

    void setStart (int sn);
    void setEnd (int en);

    void reset (bool);

    void noteOn();
    void noteOff();

    bool isFinished();

    void process (juce::AudioSampleBuffer& buffer);

    float getPosition();

    /// Which engine the current mode resolves to — handy when A/B-ing by ear.
    const char* getEngineName() const;

private:
    void createStretcher();
    int readFromSampleForward (float* dataL, float* dataR, int numSamples);
    int readFromSampleReverse (float* dataL, float* dataR, int numSamples);

    static constexpr int kBlockSize = 128;   ///< Matches the élastique source's granularity.

    int     start = 0;
    int     end = 0;
    int     numChannels = 0;
    int     readPosition = 0;
    int     mode = 0;

    bool    finished = false;
    bool    looping = false;
    bool    pingpong = false;

    Direction   initialDirection = forward;
    Direction   currentDirection = forward;

    float   crossfade = 0.0f;
    float   pitch = 0.0f;
    float   timestretch = 1.0f;
    float   formant = 0.0f;

    double sampleRate = 44100.0;
    double sourceSampleRate = 44100.0;

    juce::AudioSampleBuffer* sample = nullptr;

    chronos::TimeStretcher      stretcher;
    chronos::StretchParameters  params;
    bool prepared = false;

    juce::AudioSampleBuffer inputBuffer, outputBuffer;
    std::vector<float*> inPtrs, outPtrs;
    gin::AudioFifo fifo { 2, 1024 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChronosSource)
};
