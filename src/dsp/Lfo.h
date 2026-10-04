#pragma once
// LFO shapes, an exact mirror of lfoShapeValue in synthyy's schema.ts (frozen LFO_SHAPES order):
// 0 sine, 1 triangle, 2 saw up, 3 saw down, 4 square, 5 sample-and-hold, 6 random (smoothstep).
// Math is double so the S&H hash agrees across collaborators. Port of sf-dsp/src/util/lfo.rs.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace ddaw::dsp {

inline double lfoHash(double n) noexcept {
    const double x = std::sin(n * 127.1 + 311.7) * 43758.5453;
    return x - std::floor(x);
}

// Value in [-1, 1] for a shape index and an unwrapped phase. Out-of-range shapes are sine.
inline float lfoShapeValue(uint32_t shape, double phase) noexcept {
    const double f = phase - std::floor(phase);
    double v;
    switch (shape) {
        case 1: v = f < 0.5 ? 4.0 * f - 1.0 : 3.0 - 4.0 * f; break;
        case 2: v = 2.0 * f - 1.0; break;
        case 3: v = 1.0 - 2.0 * f; break;
        case 4: v = f < 0.5 ? 1.0 : -1.0; break;
        case 5: v = lfoHash(std::floor(phase)) * 2.0 - 1.0; break;
        case 6: {
            const double a = lfoHash(std::floor(phase)), b = lfoHash(std::floor(phase) + 1.0);
            const double t = f * f * (3.0 - 2.0 * f);
            v = (a + (b - a) * t) * 2.0 - 1.0;
            break;
        }
        default: v = std::sin(f * 2.0 * std::numbers::pi); break;
    }
    return static_cast<float>(v);
}

// Free-running LFO with an unwrapped phase so S&H/Random keep their per-cycle identity.
class Lfo {
public:
    void prepare(float sampleRate) noexcept { sr_ = std::max(double(sampleRate), 1.0); }
    void setShape(uint32_t s) noexcept { shape_ = s; }
    void setFreq(float hz) noexcept { step_ = double(std::max(hz, 0.0f)) / sr_; }
    void setPhaseOffset(float cycles) noexcept { offset_ = cycles; }  // schema `phase` param
    void setPhase(double cycles) noexcept { phase_ = cycles; }
    double phase() const noexcept { return phase_; }
    float next() noexcept {
        const float v = lfoShapeValue(shape_, phase_ + offset_);
        phase_ += step_;
        return v;
    }
    float valueAt(double cycles) const noexcept { return lfoShapeValue(shape_, cycles + offset_); }  // tempo-synced use

private:
    uint32_t shape_ = 0;
    double phase_ = 0.0, step_ = 0.0, offset_ = 0.0, sr_ = 44100.0;
};

}  // namespace ddaw::dsp
