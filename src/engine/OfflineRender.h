#pragma once
// Offline renderer: drives the same Engine and Graph the audio callback runs (render-path parity),
// then trims the reported latency. Session-scope scenes only until A3. Everything it cannot yet
// honour is reported in `unsupported`, never silently dropped.
#include <functional>
#include <string>
#include <vector>

#include "engine/Engine.h"
#include "project/Project.h"
#include "project/SampleBank.h"

namespace ddaw::engine {

struct RenderResult {
    std::vector<float> l, r;
    double sampleRate = 0;
    bool cancelled = false;   // the progress callback asked to stop; l/r hold what was rendered so far
    // Empty when the render is faithful. Otherwise the reasons (device types
    // not in the registry, unhandled features) and the render is a partial one.
    std::vector<std::string> unsupported;
    bool complete() const { return unsupported.empty(); }
};

constexpr double kTailSeconds = 1.0;  // matches synthyy's Rust harness

struct RenderOptions {
    // Master limiter. ToneCompat reproduces the browser's Tone.Limiter(-1) for synthyy golden parity.
    MasterLimiterConfig master;
    // Trim the reported latency so audio lands at timeline position zero (ARCH 10). Browser goldens
    // contain their own uncompensated pre-delays, so parity runs leave this off.
    bool trimLatency = true;
    // Called about every 8192 frames with the fraction done (0..1); return false to cancel the render.
    std::function<bool(double)> progress;
};

// Throws std::runtime_error for unusable input (unknown scene id, no tracks).
// `bank` supplies the samples for audio clips and sampled instruments (a missing id renders silent).
RenderResult renderFixture(const project::Fixture& fx, double sampleRate, const RenderOptions& opt = {}, const project::SampleBank* bank = nullptr);

}  // namespace ddaw::engine
