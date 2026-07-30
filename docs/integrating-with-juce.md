# Integrating Chronos into a JUCE plugin

Chronos ships two JUCE adapters. Pick by what you are stretching:

| Adapter | Use it for |
|---|---|
| `adapters/juce/ChronosJuceStretcher.h` | A **live signal**: audio arrives block by block from the host. |
| `adapters/juce/ChronosSampleSource.{h,cpp}` | **Sample playback**: you own a buffer and need loop / ping-pong / reverse / crossfade. Drop-in for an élastique-based source. |

Both are thin. The engine underneath is `chronos::TimeStretcher`, which you can
also drive directly — see [architecture.md](architecture.md).

---

## 1. Add the library

Chronos is dependency-free C++17. Either add it as a CMake subproject:

```cmake
add_subdirectory(Chronos)
target_link_libraries(my_plugin PRIVATE chronos)   # brings include/chronos onto the path
```

…or, for a Projucer project, vendor the tree and add the sources directly:

```
Source/3rdParty/chronos/include/chronos/*.h
Source/3rdParty/chronos/src/**/*.{h,cpp}          <- all 11 .cpp files, set to compile
```

with these header search paths (set them project-wide so every exporter gets them):

```
../../Source/3rdParty/chronos/include
../../Source/3rdParty/chronos/src
```

`src` is needed as well as `include` because the engine's internal headers include
each other by relative path.

---

## 2. Live-signal use

```cpp
chronos::juce_adapter::ChronosJuceStretcher stretcher;

void prepareToPlay (double sr, int maxBlock) override
{
    stretcher.prepare (sr, maxBlock, getTotalNumOutputChannels(), chronos::Quality::High);
}

void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
{
    juce::ScopedNoDenormals noDenormals;
    stretcher.setTimeStretch (1.5);
    stretcher.setPitchSemitones (+3.0);
    stretcher.process (buffer, buffer, buffer.getNumSamples());
}
```

Output lags input by `latencySamples()` while the pipeline primes. Report it with
`setLatencySamples()` so the host can compensate.

---

## 3. Sample-playback use (the élastique-replacement path)

`ChronosSampleSource` exposes the **same public interface** as Sonic Atlas's
`ElastiqueSource`, so swapping backends is a type alias rather than a rewrite. The
loop / ping-pong / reverse / crossfade logic is reproduced verbatim from that class —
only the stretching engine differs.

### 3.1 The swap

In `Voice.h`:

```cpp
#ifndef PI_USE_CHRONOS
 #define PI_USE_CHRONOS 0
#endif

#if PI_USE_CHRONOS
 #include "ChronosSource.h"
 using StretchSource = ChronosSource;
#else
 #include "ElastiqueSource.h"
 using StretchSource = ElastiqueSource;
#endif
```

Then replace the remaining three mentions and define `PI_USE_CHRONOS=1`:

| Where | Before | After |
|---|---|---|
| `Voice.h` member | `std::unique_ptr<ElastiqueSource> source;` | `std::unique_ptr<StretchSource> source;` |
| `Voice.cpp:144` | `std::make_unique<ElastiqueSource>()` | `std::make_unique<StretchSource>()` |
| `Voice.cpp:161` | `ElastiqueSource::reverse / ::forward` | `StretchSource::reverse / ::forward` |

That is the entire diff on the host side.

### 3.2 Where each call lands — Sonic Atlas call map

Every `source->` call site in `Source/DSP/Voice.cpp`, and what Chronos does with it.
Line numbers are from Sonic Atlas 1.3.0 (`Develop`).

**Note-start — `noteStarted()`, lines 150–163**

| Call | Chronos behaviour |
|---|---|
| `setSampleRate(currentSampleRate)` | Rebuilds the stretcher if the rate changed. Allocates — same as élastique's `createElastique()`. |
| `setSample(&buffer, layer->sampleRate)` | Stores the buffer pointer and its native rate. The rate mismatch is folded into the pitch ratio at `process()` time. |
| `setStart` / `setEnd` | Clamped against the buffer length; drive the loop bounds. Engine-independent. |
| `setLooping` / `setPingPong` | Engine-independent — consumed by the sample reader. |
| `setCrossfade(fade / 1000.0f)` | Seconds. Applied inside the reader when looping without ping-pong. |
| `setInitialDirection(reverse\|forward)` | Engine-independent. |
| `setMode(global.elastiqueMode)` | **Semantics change.** For élastique this picks Pro / Efficient / Mobile / Monophonic. For Chronos it picks the *engine* — see 3.3. |
| `noteOn()` | Resets the stretcher and FIFO, clears `finished`, seeks to `start` (or `end-1` in reverse). |

**Per-block — `updateParams()` / `renderNextBlock()`**

| Call | Line | Chronos behaviour |
|---|---|---|
| `setPitch(noteDelta)` | 649 | Semitones. Combined with the sample-rate ratio into `params.pitchRatio`. RT-safe. |
| `setTimeStretch(stretch)` | 679 | Duration multiplier, corrected for the file/host rate ratio. RT-safe. |
| `setFormant(...)` | 687 | See the calibration warning in 3.4. |
| `setCrossfade(...)` | 594 | RT-safe. |
| `process(micBuffer)` | 317 | The work. Pulls from the sample, pushes through the engine, fills the block. |
| `setPosition(curPos)` | 208, 581 | Moves the read head. |
| `reverseDirection()` | 510, 522 | Flips direction mid-flight (ping-pong, reverse FX). |
| `isFinished()` | 410 | Drives voice teardown. |
| `reset(false)` | 580 | Clears engine + FIFO, keeps settings. `reset(true)` also restores defaults. |
| `getPosition()` | UI | Read head minus engine latency. **Approximate** — see 3.4. |
| `noteOff()` | 232 | No-op, as in élastique. |

### 3.3 The mode control

`ChronosSampleSource::setMode()` maps the existing 0–3 parameter onto engines, so
you can A/B them from the plugin UI without new parameters:

| Mode | Engine | Best for |
|---|---|---|
| 0 | PhaseVocoder, `Quality::Best` | Default. Tonal and mixed material. |
| 1 | PhaseVocoder, `Quality::Good` | Same character, cheaper. |
| 2 | WSOLA, `Quality::High` | Percussive material — no phasiness by construction. |
| 3 | Sinusoidal, `Quality::High` | Sparse, purely tonal material. |

**Do not use `EngineType::Auto` in a sampler.** Auto buffers ~0.5 s of input to
classify the material before committing to an engine. That is right for offline
rendering and wrong for a sampler, where it becomes an audible delay on every
note-on. A sampler already knows what it is playing — choose explicitly.

### 3.4 Behavioural differences to know about

**The plumbing inverts.** élastique is pull-driven: it tells you how many input
frames it wants via `GetFramesNeeded()`, you hand them over, it returns a fixed
block. Chronos is a push/pull FIFO. `ChronosSampleSource::process()` therefore tops
the engine up with source blocks until `receiveOutput()` yields something, with a
bounded retry guard so an exhausted source cannot spin.

**Parameter maths is carried over unchanged**, so both backends respond identically
to the same controls:

```cpp
fileSpeedRatio = sourceSampleRate / sampleRate;
timeRatio      = jmax (0.1f, timestretch / fileSpeedRatio);
pitchRatio     = semitonesToRatio (pitch + 12 * std::log2 (fileSpeedRatio));
```

**Formants are not calibrated against élastique.** élastique takes an absolute
envelope factor; Chronos models the same idea as "preserve, then optionally shift".
The *off* state matches exactly — a centred control disables the envelope stage in
both. Non-zero settings use Chronos's own signed-semitone calibration and have not
been matched to élastique's factor curve by ear. Check this before shipping.

**`getPosition()` is approximate.** élastique reports `GetFramesBuffered()`; the
adapter substitutes `latencySamples()`. Fine for a UI read-out, not sample-exact.

### 3.5 Verifying the swap actually took effect

Compiling is not proof of linking. Check the built binary:

```bash
nm -C "<plugin binary>" | grep -c "chronos::"          # expect > 0
nm -C "<plugin binary>" | grep -c "CElastiqueProV3If"  # expect 0
```

---

## 4. Real-time safety

All allocation happens in `prepare()`. The streaming path — `setParameters`,
`pushInput`, `receiveOutput` — never allocates, locks, or throws.

Two calls **do** reallocate and must stay off the audio thread:

- `setSampleRate()` and `setMode()` on the sample source (both rebuild the engine)
- `TimeStretcher::setParameters()` **when `quality` changes** (it resizes FFTs and windows)

Changing `timeRatio`, `pitchRatio`, formant settings, or engine selection is safe
per block.

---

## 5. Known-good result

Sonic Atlas 1.3.0, Debug and Release, arm64, JUCE 8.0.12: builds clean, runs
stably, and the AU passes full `auval` — every sample rate and buffer size, MIDI,
and parameter ramping.
