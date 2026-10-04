#pragma once
// One-pole lowpass/highpass (6 dB/oct). Port of sf-dsp/src/util/onepole.rs.
#include <algorithm>
#include <cmath>
#include <numbers>

namespace ddaw::dsp {

enum class OnePoleMode { Lowpass, Highpass };

class OnePole {
public:
    explicit OnePole(OnePoleMode m = OnePoleMode::Lowpass) : mode_(m) { update(); }
    void prepare(float sampleRate) noexcept { sr_ = std::max(sampleRate, 1.0f); update(); reset(); }
    void setMode(OnePoleMode m) noexcept { mode_ = m; }
    void setCutoff(float hz) noexcept { cutoff_ = std::clamp(hz, 0.01f, sr_ * 0.49f); update(); }
    void reset() noexcept { state_ = 0.0f; }
    float processSample(float x) noexcept {
        state_ += a_ * (x - state_);
        return mode_ == OnePoleMode::Lowpass ? state_ : x - state_;
    }
    void process(float* buf, int n) noexcept { for (int i = 0; i < n; ++i) buf[i] = processSample(buf[i]); }

private:
    void update() noexcept { a_ = 1.0f - std::exp(-float(2.0 * std::numbers::pi) * cutoff_ / sr_); }  // exact pole placement
    OnePoleMode mode_;
    float a_ = 1.0f, state_ = 0.0f, sr_ = 44100.0f, cutoff_ = 1000.0f;
};

}  // namespace ddaw::dsp
