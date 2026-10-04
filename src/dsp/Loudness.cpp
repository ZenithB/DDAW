#include "dsp/Loudness.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ddaw::dsp {

void LoudnessFrame::prepare() {
    fft_.prepare(kFft);
    window_.resize(kFft);
    for (int i = 0; i < kFft; ++i) window_[size_t(i)] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / kFft);   // periodic Hann
    // librosa.A_weighting on the FFT bin frequencies, clipped at -80 dB, applied to power
    weight_.resize(kFft / 2 + 1);
    const double c0 = 12194.217 * 12194.217, c1 = 20.598997 * 20.598997, c2 = 107.65265 * 107.65265, c3 = 737.86223 * 737.86223;
    for (int i = 0; i <= kFft / 2; ++i) {
        const double f = double(i) * 16000.0 / kFft, f2 = f * f;
        double db = -80.0;
        if (f2 > 0.0) {
            db = 2.0 + 20.0 * (std::log10(c0) + 2.0 * std::log10(f2) - std::log10(f2 + c0) - std::log10(f2 + c1) - 0.5 * std::log10(f2 + c2) - 0.5 * std::log10(f2 + c3));
            db = std::max(db, -80.0);
        }
        weight_[size_t(i)] = std::pow(10.0, db / 10.0);
    }
    re_.assign(kFft, 0.0);
    im_.assign(kFft, 0.0);
}

float LoudnessFrame::compute(const float* x) noexcept {
    for (int i = 0; i < kFft; ++i) { re_[size_t(i)] = double(x[i]) * window_[size_t(i)]; im_[size_t(i)] = 0.0; }
    fft_.forward(re_.data(), im_.data());
    double sum = 0.0;
    for (int i = 0; i <= kFft / 2; ++i) sum += (re_[size_t(i)] * re_[size_t(i)] + im_[size_t(i)] * im_[size_t(i)]) * weight_[size_t(i)];
    const double power = std::max(sum / double(kFft / 2 + 1), std::pow(10.0, -kRangeDb / 10.0));
    return float(std::max(10.0 * std::log10(power), -double(kRangeDb)));
}

}  // namespace ddaw::dsp
