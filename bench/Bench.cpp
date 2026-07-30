// Chronos — bench/Bench.cpp
//
// Measures streaming throughput as a multiple of real time (xRT). One xRT means
// the engine processes one second of audio in one second of wall-clock; a plugin
// needs comfortably more than 1x per voice, with headroom for the rest of the host.
// This is a rough single-thread figure, not a rigorous benchmark, but it grounds
// the "real-time" claim in a number on this machine.

#include <chronos/Chronos.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace chronos;

static double benchOne(Quality quality, EngineType engine, double timeRatio, double pitchSemis,
                       bool formants, double sr, int seconds)
{
    const int ch = 2, block = 512;
    const int total = static_cast<int>(seconds * sr);

    StreamConfig cfg; cfg.sampleRate = sr; cfg.numChannels = ch; cfg.maxBlockSize = block;
    TimeStretcher ts; ts.prepare(cfg);
    StretchParameters p;
    p.timeRatio = timeRatio;
    p.setPitchSemitones(pitchSemis);
    p.preserveFormants = formants;
    p.quality = quality;
    p.engine = engine;
    ts.setParameters(p);

    std::vector<std::vector<Sample>> in(2, std::vector<Sample>(block));
    std::vector<std::vector<Sample>> out(2, std::vector<Sample>(block * 8));
    std::vector<Sample*> inPtrs { in[0].data(), in[1].data() };
    std::vector<Sample*> outPtrs { out[0].data(), out[1].data() };
    for (int n = 0; n < block; ++n)
    {
        const float s = 0.4f * std::sin(2.0 * M_PI * 220.0 * n / sr);
        in[0][static_cast<size_t>(n)] = in[1][static_cast<size_t>(n)] = s;
    }

    const auto t0 = std::chrono::high_resolution_clock::now();
    int fed = 0;
    while (fed < total)
    {
        ts.pushInput(AudioBlockView(inPtrs.data(), ch, block));
        ts.receiveOutput(AudioBlockView(outPtrs.data(), ch, block * 8));
        fed += block;
    }
    const auto t1 = std::chrono::high_resolution_clock::now();

    const double wall = std::chrono::duration<double>(t1 - t0).count();
    return static_cast<double>(seconds) / wall;   // xRT
}

int main()
{
    const double sr = 44100.0;
    const int secs = 20;
    std::printf("Chronos throughput (stereo, %.0f kHz, single voice)  —  higher is better\n\n", sr / 1000.0);
    std::printf("%-42s %8s\n", "configuration", "xRT");
    std::printf("%-42s %8s\n", "------------------------------------------", "--------");

    struct Row { const char* name; Quality q; EngineType e; double t; double semis; bool f; };
    const Row rows[] = {
        { "PhaseVocoder Good,  1.5x, +0st",     Quality::Good, EngineType::PhaseVocoder, 1.5, 0.0, false },
        { "PhaseVocoder High,  1.5x, +0st",     Quality::High, EngineType::PhaseVocoder, 1.5, 0.0, false },
        { "PhaseVocoder High,  1.0x, +7st",     Quality::High, EngineType::PhaseVocoder, 1.0, 7.0, false },
        { "PhaseVocoder High,  1.0x, +7st fmt", Quality::High, EngineType::PhaseVocoder, 1.0, 7.0, true  },
        { "PhaseVocoder Best,  2.0x, -5st",     Quality::Best, EngineType::PhaseVocoder, 2.0, -5.0, false },
        { "WSOLA Good,         1.5x, +0st",     Quality::Good, EngineType::Wsola,        1.5, 0.0, false },
    };

    for (const auto& r : rows)
    {
        const double xrt = benchOne(r.q, r.e, r.t, r.semis, r.f, sr, secs);
        std::printf("%-42s %7.1fx\n", r.name, xrt);
    }
    std::printf("\n(>1x = faster than real time; a voice at N xRT costs ~1/N of a core.)\n");
    return 0;
}
