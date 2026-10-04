#pragma once
// Metronome blip synth (port of sf-engine metro.rs / engine-tone scheduleMetronome): a sine ping every
// beat of the current meter, accented on bar starts. 1760 Hz at 0.5 velocity accented, 1175 Hz at 0.25
// otherwise, about 30 ms long. Mixes into the master input, like the live synth.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace ddaw::engine {

class Metronome {
public:
    void prepare(double sampleRate) noexcept { sr_ = static_cast<float>(std::max(sampleRate, 1.0)); remaining_ = 0; amp_ = 0.0f; }

    void trigger(bool accent) noexcept {
        const float freq = accent ? 1760.0f : 1175.0f;
        phase_ = 0.0f;
        step_ = freq / sr_;
        amp_ = accent ? 0.5f : 0.25f;
        decay_ = std::exp(-1.0f / (0.008f * sr_));     // exponential decay, tau 8 ms
        remaining_ = static_cast<uint32_t>(0.06f * sr_);  // hard stop at 60 ms
    }

    // Additive.
    void process(float* l, float* r, int n) noexcept {
        for (int i = 0; i < n && remaining_ > 0; ++i) {
            const float s = std::sin(phase_ * float(2.0 * std::numbers::pi)) * amp_;
            phase_ += step_;
            if (phase_ >= 1.0f) phase_ -= 1.0f;
            amp_ *= decay_;
            --remaining_;
            l[i] += s;
            r[i] += s;
        }
    }

private:
    float sr_ = 44100.0f, phase_ = 0, step_ = 0, amp_ = 0, decay_ = 0;
    uint32_t remaining_ = 0;
};

}  // namespace ddaw::engine
