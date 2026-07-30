# Chronos

A modular, real-time **audio time-stretching and pitch-shifting engine** in
dependency-free C++17. It changes the duration and pitch of audio independently,
with the refinements that separate a professional stretcher from a textbook one:
stereo-coherent phase advance, transient preservation, noise-aware reconstruction,
and formant control.

Built at Pitch Innovations as an in-house alternative to zplane élastique Pro.
Across a 44-case corpus of commercial loops it **reaches or beats élastique on six
of eight objective measures**, with the remaining two documented plainly below.

---

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure      # 40 checks
```

Render a file (also the quickest way to audition the engine):

```bash
./build/chronos_file in.wav out.wav --time 1.5 --pitch 3 --engine auto --quality best
```

Use it from code:

```cpp
#include <chronos/Chronos.h>

chronos::TimeStretcher stretcher;
stretcher.prepare({ 44100.0, /*channels*/ 2, /*maxBlock*/ 512 });

chronos::StretchParameters p;
p.timeRatio = 1.5;             // 50% longer, pitch unchanged
p.setPitchSemitones(+3.0);     // up a minor third, duration unchanged by this
p.preserveFormants = true;     // keep natural timbre
p.quality = chronos::Quality::High;
stretcher.setParameters(p);

// Real time: push input, receive output (sizes differ as audio is stretched)
stretcher.pushInput(inputView);
int got = stretcher.receiveOutput(outputView);

// Offline: one call, handles priming and tail flush
auto out = chronos::processOffline(inputBuffer, p, 44100.0);
```

Embedding in another CMake project:

```cmake
add_subdirectory(Chronos)
target_link_libraries(my_plugin PRIVATE Chronos::chronos)
```

Inside a JUCE plugin, use the adapters in `adapters/juce/` —
see [docs/integrating-with-juce.md](docs/integrating-with-juce.md), which includes a
function-by-function map for replacing an élastique-based sample source.

---

## Where things are

| Path | What |
|---|---|
| `include/chronos/` | Public API — framework-free value types and the `TimeStretcher` facade |
| `src/` | The engine: DSP primitives, analysis, and the three stretch algorithms |
| `adapters/juce/` | JUCE glue: live-signal stretcher and a sample-playback source |
| `examples/` | `chronos_file` — WAV in, stretched WAV out |
| `test/` | Correctness harness (40 checks) |
| `bench/` | Throughput measurement |
| `qa/` | Objective-quality harness: metrics, spectrograms, corpus scoreboards, A/B packs |
| `docs/` | Architecture, JUCE integration, measured quality, tuning reference |

---

## Documentation

| Document | Read it when |
|---|---|
| [docs/architecture.md](docs/architecture.md) | You want to know how it works, or you are extending it |
| [docs/integrating-with-juce.md](docs/integrating-with-juce.md) | You are putting it in a plugin — includes the full Sonic Atlas call map |
| [docs/quality.md](docs/quality.md) | You want the numbers vs élastique, and the honest gaps |
| [docs/tuning.md](docs/tuning.md) | You are changing a constant and want to know what it does |
| [CHANGELOG.md](CHANGELOG.md) | What changed and why |

---

## What it sounds like

Measured against élastique Pro V3.4.9 on 12 commercial loops × 4 stretch/pitch
cases. Mean absolute deviation from the input; lower is better except `*`.

| Property | Chronos | élastique Pro |
|---|---|---|
| Inter-channel phase drift (rad) | **0.010** | 0.010 |
| Side/mid image error (dB) | **0.323** | 1.392 |
| Amplitude modulation | **0.061** | 0.075 |
| Transient crest* (dB) | **0.906** | 0.876 |
| HF retention* (dB) | **0.811** | 0.680 |
| Inter-hit gap fill (dB) | **2.666** | 2.820 |
| Pitch warble (cents) | 11.7 | **8.97** |
| Level accuracy (dB) | 0.605 | **0.176** |

Attack rendering, per detected onset on a drum loop at ×2:

| | Chronos | élastique Pro | Source |
|---|---|---|---|
| Pre-echo before the hit | **−45.4 dB** | −44.1 dB | −37.1 dB |
| HF rise time | **2.0 ms** | 2.0 ms | 2.0 ms |

Throughput on an Apple M3 Pro, stereo, one thread, `Best`: ~107× realtime on
percussive material, ~48× on tonal (élastique: ~78×).

**Known gaps, stated plainly:** pitch warble and absolute level still favour
élastique, both concentrated in the WSOLA (percussive) path; `Auto` latches one
engine per stream rather than re-routing on changing material; formant calibration
is not matched to élastique's envelope curve; and listening tests at scale have not
been done. Full detail in [docs/quality.md](docs/quality.md).

---

## Design in one paragraph

Engines only stretch time. Pitch is realised by the facade as *stretch by
`timeRatio × pitchRatio`, then resample by `pitchRatio`* — so every engine stays
simple and the two operations are independently testable. Three engines sit behind
one strategy interface: a phase vocoder (the default, carrying the stereo-coherence,
transient and noise work), WSOLA (percussive), and a sinusoidal oscillator bank
(sparse tonal). `Auto` picks between them from a half-second content probe. All
allocation is confined to `prepare()`; the streaming path never allocates, locks, or
throws.

---

## Requirements

A C++17 compiler and CMake 3.16+. Nothing else — no third-party libraries are
vendored or linked.

The `qa/` harness additionally needs Python with NumPy, SciPy, soundfile and
Matplotlib; these are development-time only.

---

## License

Proprietary — Pitch Innovations. See [LICENSE.md](LICENSE.md).
