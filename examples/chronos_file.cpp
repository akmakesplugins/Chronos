// Chronos — tools/chronos_file.cpp
//
// File-based driver: stretch/pitch a WAV through Chronos and write the result.
// The workhorse of the listening-feedback loop — any loop can be processed and
// compared against élastique's output on the same file.
//
//   chronos_file in.wav out.wav [--time 1.5] [--pitch +3] [--engine auto|pv|wsola|sin]
//                               [--quality draft|good|high|best] [--formant]
//
// --time is the duration ratio (1.5 = 50% longer), --pitch is in semitones.
// Prints the engine the Auto probe latched, and the processing speed (xRT).

#include <chronos/Chronos.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "WavIo.h"

using namespace chronos;
using Vec = std::vector<float>;

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr,
            "usage: %s in.wav out.wav [--time R] [--pitch SEMIS] [--engine auto|pv|wsola|sin]\n"
            "       [--quality draft|good|high|best] [--formant]\n", argv[0]);
        return 1;
    }
    const std::string inPath = argv[1], outPath = argv[2];

    double timeRatio = 1.0, pitchSemis = 0.0;
    EngineType engine = EngineType::Auto;
    Quality quality = Quality::Best;
    bool formant = false;

    for (int i = 3; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if      (a == "--time")    timeRatio = std::atof(next());
        else if (a == "--pitch")   pitchSemis = std::atof(next());
        else if (a == "--formant") formant = true;
        else if (a == "--engine")
        {
            const std::string e = next();
            if      (e == "pv")    engine = EngineType::PhaseVocoder;
            else if (e == "wsola") engine = EngineType::Wsola;
            else if (e == "sin")   engine = EngineType::Sinusoidal;
            else                   engine = EngineType::Auto;
        }
        else if (a == "--quality")
        {
            const std::string q = next();
            if      (q == "draft") quality = Quality::Draft;
            else if (q == "good")  quality = Quality::Good;
            else if (q == "high")  quality = Quality::High;
            else                   quality = Quality::Best;
        }
    }

    std::vector<Vec> in;
    double sr = 48000.0;
    if (!wavio::read(inPath, in, sr))
        return 1;

    const int ch = static_cast<int>(in.size());
    const int n  = static_cast<int>(in[0].size());
    const long expected = static_cast<long>(std::llround(static_cast<double>(n) * timeRatio));

    StreamConfig cfg;
    cfg.sampleRate = sr;
    cfg.numChannels = ch;
    cfg.maxBlockSize = 1024;

    TimeStretcher ts;
    ts.prepare(cfg);

    StretchParameters p;
    p.timeRatio = timeRatio;
    p.setPitchSemitones(pitchSemis);
    p.preserveFormants = formant;
    p.quality = quality;
    p.engine = engine;
    ts.setParameters(p);

    std::vector<Vec> out(static_cast<size_t>(ch));
    std::vector<Vec> readBuf(static_cast<size_t>(ch), Vec(4096));
    std::vector<float*> inPtrs(static_cast<size_t>(ch)), outPtrs(static_cast<size_t>(ch));
    for (int c = 0; c < ch; ++c)
        outPtrs[static_cast<size_t>(c)] = readBuf[static_cast<size_t>(c)].data();

    auto drain = [&]()
    {
        int got;
        do
        {
            got = ts.receiveOutput(AudioBlockView(outPtrs.data(), ch, 4096));
            for (int c = 0; c < ch; ++c)
                out[static_cast<size_t>(c)].insert(out[static_cast<size_t>(c)].end(),
                                                   readBuf[static_cast<size_t>(c)].begin(),
                                                   readBuf[static_cast<size_t>(c)].begin() + got);
        } while (got == 4096);
    };

    const auto t0 = std::chrono::steady_clock::now();

    int pos = 0;
    while (pos < n)
    {
        const int b = std::min(1024, n - pos);
        for (int c = 0; c < ch; ++c)
            inPtrs[static_cast<size_t>(c)] = in[static_cast<size_t>(c)].data() + pos;
        ts.pushInput(AudioBlockView(inPtrs.data(), ch, b));
        pos += b;
        drain();
    }
    ts.flush();
    drain();

    const auto t1 = std::chrono::steady_clock::now();
    const double procSec = std::chrono::duration<double>(t1 - t0).count();
    const double audioSec = n / sr;

    for (auto& v : out)
        v.resize(static_cast<size_t>(expected), 0.0f);

    wavio::write(outPath, out, sr);
    std::printf("engine=%s xrt=%.1f in=%d out=%ld sr=%.0f ch=%d\n",
                ts.activeEngineName(), audioSec / procSec, n, expected, sr, ch);
    return 0;
}
