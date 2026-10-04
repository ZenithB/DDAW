#pragma once
// PolyBLEP-antialiased oscillator. Wave indices mirror synthyy's frozen WAVES order:
// 0 saw, 1 square, 2 triangle, 3 sine, 4 fat saw, 5 fat square, 6 fat triangle. The fat variants map
// onto their base shape; the unison detune is a device concern. Port of sf-dsp/src/util/osc.rs.
#include <algorithm>
#include <cmath>
#include <numbers>

namespace ddaw::dsp {

enum class Wave { Saw, Square, Triangle, Sine };

inline Wave waveFromIndex(int i) noexcept {
    switch (i) {
        case 1: case 5: return Wave::Square;
        case 2: case 6: return Wave::Triangle;
        case 3: return Wave::Sine;
        default: return Wave::Saw;  // 0, 4 and out-of-range
    }
}

class PolyBlepOsc {
public:
    explicit PolyBlepOsc(Wave w = Wave::Saw) : wave_(w) {}

    void prepare(float sampleRate) noexcept { sr_ = std::max(sampleRate, 1.0f); }
    void setWave(Wave w) noexcept { if (w != wave_) { wave_ = w; triState_ = 0.0f; } }
    // Phase-continuous frequency change (only the step moves).
    void setFreq(float hz) noexcept { step_ = std::clamp(hz / sr_, 0.0f, 0.5f); }
    void resetPhase(float phase) noexcept { phase_ = phase - std::floor(phase); triState_ = 0.0f; }

    float next() noexcept {
        const float t = phase_;
        const float dt = std::max(step_, 1e-9f);
        float out;
        switch (wave_) {
            case Wave::Sine: out = std::sin(t * float(2.0 * std::numbers::pi)); break;
            case Wave::Saw: out = 2.0f * t - 1.0f - polyBlep(t, dt); break;
            case Wave::Square: {
                float s = t < 0.5f ? 1.0f : -1.0f;
                s += polyBlep(t, dt);
                const float t2 = t + 0.5f;
                out = s - polyBlep(t2 - std::floor(t2), dt);
                break;
            }
            case Wave::Triangle: default: {
                // Leaky-integrated BLEP square; gain 4*dt keeps unit amplitude.
                float s = t < 0.5f ? 1.0f : -1.0f;
                s += polyBlep(t, dt);
                const float t2 = t + 0.5f;
                s -= polyBlep(t2 - std::floor(t2), dt);
                triState_ = 4.0f * dt * s + (1.0f - 4.0f * dt) * triState_;
                out = triState_;
                break;
            }
        }
        phase_ += step_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        return out;
    }

private:
    // Two-sample polynomial band-limited step correction around phase 0.
    static float polyBlep(float t, float dt) noexcept {
        if (t < dt) { const float x = t / dt; return x + x - x * x - 1.0f; }
        if (t > 1.0f - dt) { const float x = (t - 1.0f) / dt; return x * x + x + x + 1.0f; }
        return 0.0f;
    }
    Wave  wave_;
    float phase_ = 0.0f, step_ = 0.0f, sr_ = 44100.0f, triState_ = 0.0f;
};

}  // namespace ddaw::dsp
