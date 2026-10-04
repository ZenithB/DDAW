#pragma once
// Performance-tracking sources and how they are normalised into a 0..1 control (PLAN 4.5). Shared by the
// engine's modulation stage (live routes) and the curve recorder (automation lanes), so a recorded lane
// reproduces the live route exactly.
#include <algorithm>
#include <cmath>
#include <string>

#include "core/Device.h"

namespace ddaw {

enum class PerfSource : uint8_t { F0, Loudness, Confidence, Envelope };

inline PerfSource perfSourceFromName(const std::string& s) {
    if (s == "loudness") return PerfSource::Loudness;
    if (s == "confidence") return PerfSource::Confidence;
    if (s == "envelope") return PerfSource::Envelope;
    return PerfSource::F0;
}
inline const char* perfSourceName(PerfSource s) {
    switch (s) {
        case PerfSource::F0: return "f0";
        case PerfSource::Loudness: return "loudness";
        case PerfSource::Confidence: return "confidence";
        case PerfSource::Envelope: return "envelope";
    }
    return "f0";
}

// Default ranges per source, used when a route does not set its own: f0 80..800 Hz (a log scale, so equal
// intervals move the control equally), loudness -60..0 dB, envelope and confidence 0..1.
inline void perfDefaultRange(PerfSource s, double& lo, double& hi) {
    switch (s) {
        case PerfSource::F0: lo = 80.0; hi = 800.0; break;
        case PerfSource::Loudness: lo = -60.0; hi = 0.0; break;
        case PerfSource::Confidence: case PerfSource::Envelope: lo = 0.0; hi = 1.0; break;
    }
}

// The source's value in 0..1 over [lo, hi]. Returns false when the frame carries no value for it (an
// unvoiced frame has no pitch): the control then holds its last value.
inline bool perfNormalize(PerfSource s, const PerformanceFrame& f, double lo, double hi, float& out) {
    double v = 0.0;
    switch (s) {
        case PerfSource::F0:
            if (f.f0Hz <= 0.0f || lo <= 0.0 || hi <= lo) return false;
            v = std::log(double(f.f0Hz) / lo) / std::log(hi / lo);
            break;
        case PerfSource::Loudness: v = hi > lo ? (double(f.loudnessDb) - lo) / (hi - lo) : 0.0; break;
        case PerfSource::Confidence: v = double(f.confidence); break;
        case PerfSource::Envelope: v = hi > lo ? (double(f.envelope) - lo) / (hi - lo) : 0.0; break;
    }
    out = float(std::clamp(v, 0.0, 1.0));
    return true;
}

}  // namespace ddaw
