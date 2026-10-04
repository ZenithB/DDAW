#include "ddsp/HarmonicSynth.h"

#include <cmath>
#include <numbers>

namespace ddaw::ddsp {

namespace {
constexpr double kTwoPi = 2.0 * std::numbers::pi;

}  // namespace

// ddsp.core.exp_sigmoid(x, exponent=10, max_value=2, threshold=1e-7)
float expSigmoid(float x) noexcept {
    const float s = 1.0f / (1.0f + std::exp(-x));
    return 2.0f * std::pow(s, std::log(10.0f)) + 1e-7f;
}

void makeHarmonicFrame(const float* raw, float f0Hz, int sampleRate, HarmonicFrame& out) noexcept {
    out.f0Hz = f0Hz;
    out.amp = expSigmoid(raw[0]);
    float sum = 0.0f;
    for (int k = 0; k < kNumHarmonics; ++k) {
        float h = expSigmoid(raw[kNumAmps + k]);
        if (f0Hz * float(k + 1) >= float(sampleRate) * 0.5f) h = 0.0f;  // remove above Nyquist
        out.hd[k] = h;
        sum += h;
    }
    const float denom = sum == 0.0f ? 1e-7f : sum;  // core.safe_divide
    for (int k = 0; k < kNumHarmonics; ++k) out.hd[k] /= denom;
}

void HarmonicSynth::renderInterval(const HarmonicFrame& prev, const HarmonicFrame& next, float* out) noexcept {
    const int hop = hop_;
    const double nyq = sampleRate_ * 0.5;
    for (int p = 0; p < hop; ++p) {
        const float t = float(p) / float(hop);
        const float f0 = prev.f0Hz + (next.f0Hz - prev.f0Hz) * t;
        // periodic Hann, window length 2*hop: weight of the *next* frame
        const float w = 0.5f - 0.5f * std::cos(float(kTwoPi) * float(p) / float(2 * hop));

        phase_ += kTwoPi * double(f0) / double(sampleRate_);  // inclusive cumsum, like tf.cumsum
        if (phase_ >= kTwoPi) phase_ = std::fmod(phase_, kTwoPi);

        const float a0 = prev.amp * (1.0f - w), a1 = next.amp * w;
        float acc = 0.0f;
        for (int k = 0; k < kNumHarmonics; ++k) {
            if (double(f0) * double(k + 1) >= nyq) break;  // per-sample Nyquist mask (oscillator_bank)
            const float amp = a0 * prev.hd[k] + a1 * next.hd[k];
            acc += amp * float(std::sin(std::fmod(double(k + 1) * phase_, kTwoPi)));
        }
        out[p] = acc;
    }
}

}  // namespace ddaw::ddsp
