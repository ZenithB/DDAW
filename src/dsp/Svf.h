#pragma once
// State-variable filter, Andrew Simper's trapezoidal-integrator form (cytomic). Stable across the
// audio band at any sane Q. Port of sf-dsp/src/util/svf.rs.
#include <algorithm>
#include <cmath>
#include <numbers>

namespace ddaw::dsp {

enum class SvfMode { Lowpass, Highpass, Bandpass, Notch };

class Svf {
public:
    explicit Svf(SvfMode m = SvfMode::Lowpass) : mode_(m) { update(); }

    void prepare(float sampleRate) noexcept { sr_ = std::max(sampleRate, 1.0f); update(); reset(); }
    void setMode(SvfMode m) noexcept { mode_ = m; }
    void setCutoffQ(float cutoffHz, float q) noexcept {
        cutoff_ = std::clamp(cutoffHz, 1.0f, sr_ * 0.49f);  // keep the prewarp finite
        q_ = std::max(q, 0.025f);
        update();
    }
    void reset() noexcept { ic1_ = ic2_ = 0.0f; }

    float processSample(float v0) noexcept {
        const float v3 = v0 - ic2_;
        const float v1 = a1_ * ic1_ + a2_ * v3;
        const float v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = 2.0f * v1 - ic1_;
        ic2_ = 2.0f * v2 - ic2_;
        switch (mode_) {
            case SvfMode::Lowpass: return v2;
            case SvfMode::Bandpass: return v1;
            case SvfMode::Highpass: return v0 - k_ * v1 - v2;
            case SvfMode::Notch: default: return v0 - k_ * v1;
        }
    }
    void process(float* buf, int n) noexcept { for (int i = 0; i < n; ++i) buf[i] = processSample(buf[i]); }

private:
    void update() noexcept {
        g_ = std::tan(float(std::numbers::pi) * cutoff_ / sr_);
        k_ = 1.0f / q_;
        a1_ = 1.0f / (1.0f + g_ * (g_ + k_));
        a2_ = g_ * a1_;
        a3_ = g_ * a2_;
    }
    SvfMode mode_;
    float g_ = 0, k_ = 0, a1_ = 0, a2_ = 0, a3_ = 0, ic1_ = 0, ic2_ = 0;
    float sr_ = 44100.0f, cutoff_ = 1000.0f, q_ = float(std::numbers::sqrt2 / 2.0);
};

}  // namespace ddaw::dsp
