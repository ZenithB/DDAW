#pragma once
// Performance tracker (PLAN 4.5): turns a mono input into a stream of control frames - fundamental
// frequency (YIN), loudness (matched to Magenta's training features), an envelope, and a confidence - at
// 250 frames per second. The same object serves the engine's live tracking path and the DDSP instrument
// (B3). Everything is preallocated; push() never allocates.
//
// Time: frame k describes the input around the moment k * 64 samples (at the 16 kHz analysis rate) after
// the stream started (input before the start counts as silence, as in Magenta's centre padding). A frame
// becomes available after the analysis windows' look-ahead, `latencySamples()` host samples after that
// moment. Each frame carries the host-rate input position of its moment, so a recorder can place it on a
// timeline independent of when it arrived.
#include <vector>

#include "core/Device.h"
#include "dsp/EnvelopeFollower.h"
#include "dsp/Loudness.h"
#include "dsp/StreamResampler.h"
#include "dsp/Yin.h"

namespace ddaw::dsp {

struct TrackerConfig {
    float minHz = 70.0f, maxHz = 1500.0f;    // the pitch range searched (a higher floor lowers the latency)
    float yinThreshold = 0.15f;
    float voicedConfidence = 0.5f;            // below this the frame is unvoiced (f0 = 0)
    float voicedFloorDb = -70.0f;             // and so is anything quieter than this
    float envAttackMs = 5.0f, envReleaseMs = 80.0f;
};

struct TimedFrame {
    PerformanceFrame frame{};
    double inputFrame = 0.0;      // host-rate input sample index of the frame's moment (not its arrival)
};

class PerformanceTracker {
public:
    static constexpr int kAnalysisRate = 16000;
    static constexpr float kMinFloorHz = 40.0f, kMaxCeilHz = 2000.0f;
    void prepare(double hostRate, const TrackerConfig& cfg = {});
    // Change the pitch range, thresholds and envelope times without reallocating (audio-thread safe).
    // The floor is limited to 40 Hz and the ceiling to 2 kHz; a new floor changes latencySamples().
    void setConfig(const TrackerConfig& cfg) noexcept;
    void reset();
    // Mono input in, frames out (at most `maxOut`, the rest are dropped). Returns the number produced.
    int push(const float* x, int n, TimedFrame* out, int maxOut) noexcept;
    // Host samples from a sound to the frame that reports it (look-ahead of both analyses + resampling).
    int latencySamples() const noexcept { return latencyHost_; }
    double hostRate() const noexcept { return hostRate_; }
    const TrackerConfig& config() const noexcept { return cfg_; }

private:
    void computeFrame(int64_t k, TimedFrame& out) noexcept;
    TrackerConfig cfg_;
    double hostRate_ = 48000.0;
    StreamResampler resampler_;
    LoudnessFrame loudness_;
    Yin yin_;
    EnvelopeFollower env_;
    std::vector<float> ring_, envRing_, scratch_;
    size_t mask_ = 0;
    int64_t count_ = 0, nextFrame_ = 0;
    int right_ = 256, latencyHost_ = 0;
};

}  // namespace ddaw::dsp
