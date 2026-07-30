// Chronos — Chronos.h
//
// Umbrella header for the public API. Include this to use the library:
//
//     #include <chronos/Chronos.h>
//
//     chronos::TimeStretcher stretcher;
//     stretcher.prepare({ sampleRate, numChannels, maxBlockSize });
//
//     chronos::StretchParameters p;
//     p.timeRatio = 1.5;              // 50% longer
//     p.setPitchSemitones(+3.0);      // up a minor third
//     p.preserveFormants = true;
//     stretcher.setParameters(p);
//
//     // Real-time: push input, receive output.
//     stretcher.process(inputView, outputView);
//
//     // Offline: one call.
//     auto out = chronos::processOffline(inputBuffer, p, sampleRate);

#pragma once

#include "AudioBuffer.h"
#include "StretchParameters.h"
#include "TimeStretcher.h"
#include "Types.h"

/// Library version, semantic. Bump the minor for additive changes, major for API
/// breaks.
#define CHRONOS_VERSION_MAJOR 0
#define CHRONOS_VERSION_MINOR 1
#define CHRONOS_VERSION_PATCH 0
