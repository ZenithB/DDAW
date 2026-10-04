#pragma once
// Streaming sample-rate converter for analysis paths (windowed-sinc, linear-phase, band-limited to the
// lower of the two Nyquist frequencies). Arbitrary ratios; allocation-free after prepare().
//
// Output sample m sits at input time m * (inRate / outRate), so the output stream is aligned with the
// input in TIME; it becomes AVAILABLE `latencyIn()` input samples late (the filter's look-ahead).
// Equal rates pass through unchanged with no delay.
#include <cmath>
#include <numbers>
#include <vector>

namespace ddaw::dsp {

class StreamResampler {
public:
    // halfWidth: zero crossings each side of the kernel (at the output Nyquist). 16 gives ~-90 dB stopband.
    void prepare(double inRate, double outRate, int halfWidth = 16) {
        ratio_ = inRate / outRate;
        passthrough_ = std::abs(ratio_ - 1.0) < 1e-9;
        L_ = halfWidth;
        stretch_ = std::max(ratio_, 1.0);        // widen the kernel when decimating (cutoff = output Nyquist)
        const int span = int(std::ceil(L_ * stretch_)) + 2;
        size_t cap = 64;
        while (int(cap) < 2 * span + 16) cap <<= 1;
        hist_.assign(cap, 0.0f);
        mask_ = cap - 1;
        // Kaiser-windowed sinc, tabulated over [-L, L] in units of output-rate zero crossings
        constexpr int kRes = 512;
        res_ = kRes;
        tab_.assign(size_t(2 * L_ * kRes + 2), 0.0f);
        const double beta = 8.6;
        auto i0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 40; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
        const double norm = i0(beta);
        for (size_t j = 0; j < tab_.size(); ++j) {
            const double v = double(j) / kRes - L_;
            const double w = std::abs(v) >= L_ ? 0.0 : i0(beta * std::sqrt(1.0 - (v / L_) * (v / L_))) / norm;
            const double s = std::abs(v) < 1e-12 ? 1.0 : std::sin(std::numbers::pi * v) / (std::numbers::pi * v);
            tab_[j] = float(s * w);
        }
        reset();
    }
    void reset() {
        std::fill(hist_.begin(), hist_.end(), 0.0f);
        written_ = 0;
        nextOut_ = 0;
    }
    // Input samples between a moment and the moment the output sample for it can be produced.
    int latencyIn() const noexcept { return passthrough_ ? 0 : int(std::ceil(L_ * stretch_)); }
    double ratio() const noexcept { return ratio_; }
    int64_t inputCount() const noexcept { return written_; }

    // Push n input samples; `emit(float y, double inputTime)` is called for each output sample that
    // became available (inputTime = its position on the input timeline, in input samples).
    template <class F> void process(const float* x, int n, F&& emit) {
        if (passthrough_) {
            for (int i = 0; i < n; ++i) { emit(x[i], double(written_)); ++written_; }
            return;
        }
        for (int i = 0; i < n; ++i) {
            hist_[size_t(written_) & mask_] = x[i];
            ++written_;
            // output m needs input up to ceil(t) + reach, where t = m * ratio
            for (;;) {
                const double t = double(nextOut_) * ratio_;
                const int64_t last = int64_t(std::floor(t + L_ * stretch_));
                if (last >= written_) break;
                emit(compute(t), t);
                ++nextOut_;
            }
        }
    }

private:
    float compute(double t) const noexcept {
        const int64_t first = int64_t(std::ceil(t - L_ * stretch_));
        const int64_t last = int64_t(std::floor(t + L_ * stretch_));
        double acc = 0.0, norm = 0.0;
        for (int64_t k = first; k <= last; ++k) {
            const double v = (double(k) - t) / stretch_;       // kernel argument in zero crossings
            const double pos = (v + L_) * res_;
            const size_t j = size_t(pos);
            const float frac = float(pos - double(j));
            const float w = tab_[j] + (tab_[j + 1] - tab_[j]) * frac;
            const float xs = k >= 0 ? hist_[size_t(k) & mask_] : 0.0f;
            acc += double(w) * double(xs);
            norm += double(w);
        }
        return float(norm != 0.0 ? acc / norm : 0.0);      // unity DC gain whatever the table's rounding
    }

    double ratio_ = 1.0, stretch_ = 1.0;
    bool passthrough_ = true;
    int L_ = 16, res_ = 512;
    std::vector<float> hist_, tab_;
    size_t mask_ = 0;
    int64_t written_ = 0, nextOut_ = 0;
};

}  // namespace ddaw::dsp
