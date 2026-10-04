#pragma once
// Envelope follower. Peak mode: attack/release smoothing of the rectified signal. RMS mode: a symmetric
// running mean of the square (its window is the release time; the attack time is not used), then the root.
#include <cmath>

namespace ddaw::dsp {

class EnvelopeFollower {
public:
    enum class Mode { Peak, Rms };
    void prepare(double sr, Mode m = Mode::Peak) { sr_ = sr; mode_ = m; setAttackMs(5.0f); setReleaseMs(80.0f); y_ = 0.0f; }
    void setAttackMs(float ms) { att_ = coeff(ms); }
    void setReleaseMs(float ms) { rel_ = coeff(ms); }
    void reset() { y_ = 0.0f; }
    float next(float x) noexcept {
        const float a = mode_ == Mode::Peak ? std::abs(x) : x * x;
        const float c = mode_ == Mode::Rms ? rel_ : (a > y_ ? att_ : rel_);
        y_ = a + c * (y_ - a);
        return mode_ == Mode::Peak ? y_ : std::sqrt(y_);
    }
    void fill(const float* in, float* out, int n) noexcept { for (int i = 0; i < n; ++i) out[i] = next(in[i]); }
    float value() const noexcept { return mode_ == Mode::Peak ? y_ : std::sqrt(y_); }

private:
    float coeff(float ms) const { return ms <= 0.0f ? 0.0f : std::exp(-1.0f / (0.001f * ms * float(sr_))); }
    double sr_ = 48000.0;
    Mode mode_ = Mode::Peak;
    float att_ = 0.0f, rel_ = 0.0f, y_ = 0.0f;
};

}  // namespace ddaw::dsp
