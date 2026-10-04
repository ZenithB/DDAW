#pragma once
// Turning recorded performance frames into automation lanes (JUCE-free, unit tested). A lane value is the
// same 0..1 control a live performance route would have produced (core/Performance.h), placed on the
// timeline from the frame's input position, shifted earlier by the latency compensation, then simplified.
#include <vector>

#include "core/Performance.h"
#include "engine/Engine.h"
#include "project/Project.h"

namespace ddaw::app {

struct CurveTiming {
    double startInput = 0;       // engine input-frame index where the take began
    double startTick = 0;        // timeline tick at that input frame
    double compFrames = 0;       // device + engine latency: the performer played this long before the input arrived
    double ticksPerFrame = 0;    // bpm * 96 / 60 / sampleRate
};

// Points (tick, 0..1) for one source over [lo, hi]. Frames before the take are ignored; an unvoiced pitch
// frame carries no value (the lane holds). `epsilon` is the largest deviation simplification may introduce.
std::vector<project::AutoPoint> buildLane(const std::vector<engine::Engine::TimedPerf>& frames, PerfSource src, double lo, double hi, const CurveTiming& t,
                                          double epsilon = 0.01, size_t maxPoints = 4000);

// Ramer-Douglas-Peucker on a function of time: the vertical deviation from the chord is the error.
std::vector<project::AutoPoint> simplifyLane(const std::vector<project::AutoPoint>& pts, double epsilon);

// Replace the part of `existing` covered by `recorded` (its first to last tick) with `recorded`.
std::vector<project::AutoPoint> mergeLane(const std::vector<project::AutoPoint>& existing, const std::vector<project::AutoPoint>& recorded);

}  // namespace ddaw::app
