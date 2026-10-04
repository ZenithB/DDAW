#include "dsp/Yin.h"

#include <algorithm>
#include <cmath>

namespace ddaw::dsp {

void Yin::prepare(double sr, float minHz, float maxHz, float threshold) {
    sr_ = sr;
    threshold_ = threshold;
    tauMax_ = std::max(8, int(std::ceil(sr / std::max(minHz, 20.0f))));
    tauMin_ = std::clamp(int(std::floor(sr / std::max(maxHz, minHz * 2.0f))), 2, tauMax_ - 3);
    d_.assign(size_t(tauMax_ + 1), 0.0f);
    cmnd_.assign(size_t(tauMax_ + 1), 1.0f);
}

YinResult Yin::estimate(const float* x) noexcept {
    const int W = tauMax_;   // integration window; the signal window is W + tauMax_ long
    // difference function d(tau) = sum_{j<W} (x[j] - x[j + tau])^2
    // Single-precision with four independent accumulators so the compiler vectorises it: the sums are of squares
    // of a signal in [-1, 1] over a few hundred samples, far inside float's range for a pitch decision.
    for (int tau = 1; tau < tauMax_; ++tau) {
        float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
        const float* y = x + tau;
        int j = 0;
        for (; j + 4 <= W; j += 4) {
            const float d0 = x[j] - y[j], d1 = x[j + 1] - y[j + 1], d2 = x[j + 2] - y[j + 2], d3 = x[j + 3] - y[j + 3];
            a0 += d0 * d0; a1 += d1 * d1; a2 += d2 * d2; a3 += d3 * d3;
        }
        for (; j < W; ++j) { const float d = x[j] - y[j]; a0 += d * d; }
        d_[size_t(tau)] = (a0 + a1) + (a2 + a3);
    }
    // silence: no period to find
    float energy = 0.0f;
    for (int j = 0; j < W; ++j) energy += x[j] * x[j];
    if (energy < 1e-9 * W) return {};
    // cumulative mean normalised difference
    cmnd_[0] = 1.0f;
    double run = 0.0;
    for (int tau = 1; tau < tauMax_; ++tau) {
        run += double(d_[size_t(tau)]);
        cmnd_[size_t(tau)] = run > 0.0 ? float(double(d_[size_t(tau)]) * tau / run) : 1.0f;
    }
    // first dip below the threshold, followed down to its local minimum; otherwise the global minimum
    int best = -1;
    for (int tau = tauMin_; tau < tauMax_ - 1; ++tau) {
        if (cmnd_[size_t(tau)] < threshold_) {
            while (tau + 1 < tauMax_ - 1 && cmnd_[size_t(tau + 1)] < cmnd_[size_t(tau)]) ++tau;
            best = tau;
            break;
        }
    }
    if (best < 0) {
        float m = 2.0f;
        for (int tau = tauMin_; tau < tauMax_ - 1; ++tau) if (cmnd_[size_t(tau)] < m) { m = cmnd_[size_t(tau)]; best = tau; }
        if (best < 0) return {};
    }
    // parabolic interpolation around the minimum
    double tauF = best;
    if (best > 1 && best < tauMax_ - 1) {
        const double a = cmnd_[size_t(best - 1)], b = cmnd_[size_t(best)], c = cmnd_[size_t(best + 1)];
        const double den = a - 2.0 * b + c;
        if (std::abs(den) > 1e-12) tauF = best + 0.5 * (a - c) / den;
    }
    YinResult r;
    r.confidence = std::clamp(1.0f - cmnd_[size_t(best)], 0.0f, 1.0f);
    r.f0Hz = tauF > 0 ? float(sr_ / tauF) : 0.0f;
    return r;
}

}  // namespace ddaw::dsp
