#include "dsp/Tracker.h"

#include <algorithm>
#include <cmath>

namespace ddaw::dsp {

void PerformanceTracker::prepare(double hostRate, const TrackerConfig& cfg) {
    hostRate_ = hostRate;
    resampler_.prepare(hostRate, kAnalysisRate);
    loudness_.prepare();
    env_.prepare(kAnalysisRate, EnvelopeFollower::Mode::Peak);
    // size every buffer for the lowest floor once, so setConfig() later never allocates
    yin_.prepare(kAnalysisRate, kMinFloorHz, kMaxCeilHz);
    ring_.assign(4096, 0.0f);
    envRing_.assign(4096, 0.0f);
    mask_ = ring_.size() - 1;
    scratch_.assign(size_t(std::max(LoudnessFrame::kFft, yin_.windowSamples())), 0.0f);
    setConfig(cfg);
    reset();
}

void PerformanceTracker::setConfig(const TrackerConfig& in) noexcept {
    cfg_ = in;
    cfg_.minHz = std::clamp(cfg_.minHz, kMinFloorHz, 1000.0f);
    cfg_.maxHz = std::clamp(cfg_.maxHz, cfg_.minHz * 1.5f, kMaxCeilHz);
    yin_.prepare(kAnalysisRate, cfg_.minHz, cfg_.maxHz, cfg_.yinThreshold);
    env_.setAttackMs(cfg_.envAttackMs);
    env_.setReleaseMs(cfg_.envReleaseMs);
    right_ = std::max(LoudnessFrame::kFft / 2, yin_.latencySamples());
    latencyHost_ = int(std::ceil(double(right_) * resampler_.ratio())) + resampler_.latencyIn();
}

void PerformanceTracker::reset() {
    resampler_.reset();
    env_.reset();
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    std::fill(envRing_.begin(), envRing_.end(), 0.0f);
    count_ = 0;
    nextFrame_ = 0;
}

void PerformanceTracker::computeFrame(int64_t k, TimedFrame& out) noexcept {
    const int64_t c = k * LoudnessFrame::kHop;
    auto at = [&](int64_t i) { return i < 0 ? 0.0f : ring_[size_t(i) & mask_]; };
    for (int i = 0; i < LoudnessFrame::kFft; ++i) scratch_[size_t(i)] = at(c - LoudnessFrame::kFft / 2 + i);
    const float ld = loudness_.compute(scratch_.data());
    const int w = yin_.windowSamples(), half = yin_.latencySamples();
    for (int i = 0; i < w; ++i) scratch_[size_t(i)] = at(c - half + i);
    const YinResult y = yin_.estimate(scratch_.data());
    const bool voiced = y.f0Hz > 0.0f && y.confidence >= cfg_.voicedConfidence && ld > cfg_.voicedFloorDb;
    out.frame.f0Hz = voiced ? y.f0Hz : 0.0f;
    out.frame.confidence = y.confidence;
    out.frame.loudnessDb = ld;
    out.frame.envelope = std::min(1.0f, c >= 0 ? envRing_[size_t(c) & mask_] : 0.0f);
    out.inputFrame = double(c) * resampler_.ratio();
}

int PerformanceTracker::push(const float* x, int n, TimedFrame* out, int maxOut) noexcept {
    int produced = 0;
    resampler_.process(x, n, [&](float y, double) {
        ring_[size_t(count_) & mask_] = y;
        envRing_[size_t(count_) & mask_] = env_.next(y);
        ++count_;
        // frame k needs samples up to k*hop + right_
        while (count_ > nextFrame_ * LoudnessFrame::kHop + right_) {
            if (produced < maxOut) computeFrame(nextFrame_, out[produced++]);
            ++nextFrame_;
        }
    });
    return produced;
}

}  // namespace ddaw::dsp
