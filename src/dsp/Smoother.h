#pragma once
// One-pole parameter smoother (ARCH 12). Allocation-free; safe on the audio thread.
#include <cmath>

namespace ddaw::dsp {

class Smoother {
public:
    // timeMs <= 0, or under one sample, makes the smoother instant (matches synthyy's Smoother).
    void prepare(double sampleRate, float timeMs) noexcept {
        sr_ = sampleRate;
        setTimeMs(timeMs);
    }
    void setTimeMs(float timeMs) noexcept {
        const float samples = timeMs * 0.001f * static_cast<float>(sr_);
        coeff_ = samples < 1.0f ? 1.0f : 1.0f - std::exp(-1.0f / samples);
    }
    void setTarget(float v) noexcept { target_ = v; }
    void snap(float v) noexcept { target_ = current_ = v; }
    float next() noexcept {
        current_ += coeff_ * (target_ - current_);
        return current_;
    }
    void fill(float* dst, int n) noexcept {
        for (int i = 0; i < n; ++i) dst[i] = next();
    }
    float current() const noexcept { return current_; }
    float target() const noexcept { return target_; }
    bool settled() const noexcept { return std::abs(target_ - current_) < 1e-6f; }

private:
    float coeff_ = 1.0f, target_ = 0.0f, current_ = 0.0f;
    double sr_ = 44100.0;
};

}  // namespace ddaw::dsp
