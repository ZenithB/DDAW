#include "dsp/Limiter.h"

#include <algorithm>
#include <cmath>

namespace ddaw::dsp {

void Limiter::prepare(double sr, float ceilingDb, float lookaheadMs, float releaseMs) {
    L_ = std::clamp(static_cast<int>(std::lround(lookaheadMs * 0.001 * sr)), 1, kMaxLookahead);
    ceiling_ = std::pow(10.0f, ceilingDb / 20.0f);
    relCoef_ = 1.0f - std::exp(-1.0f / (releaseMs * 0.001f * static_cast<float>(sr)));
    dl_.assign(static_cast<size_t>(L_) + 1, 0.0f);
    dr_.assign(static_cast<size_t>(L_) + 1, 0.0f);
    reset();
}

void Limiter::reset() noexcept {
    std::fill(dl_.begin(), dl_.end(), 0.0f);
    std::fill(dr_.begin(), dr_.end(), 0.0f);
    gmin_.fill(1.0f);
    gminOut_.fill(1.0f);
    gRel_ = 1.0f;
    pos_ = 0;
    sinceSync_ = 0;
    boxSum_ = double(L_ + 1);
    grDb_ = 0.0f;
}

void Limiter::process(float* l, float* r, int n) noexcept {
    const int W = L_ + 1;
    float minGain = 1.0f;
    for (int i = 0; i < n; ++i) {
        const float peak = std::max(std::abs(l[i]), std::abs(r[i]));
        const float target = peak > ceiling_ ? ceiling_ / peak : 1.0f;
        // release smoothing: falls instantly, rises exponentially; never above the target
        gRel_ = std::min(target, gRel_ + (1.0f - gRel_) * relCoef_);
        gmin_[static_cast<size_t>(pos_)] = gRel_;

        // sliding minimum over the last W samples (W <= 257, so a direct scan is cheap)
        float m = 1.0f;
        for (int k = 0; k < W; ++k) m = std::min(m, gmin_[static_cast<size_t>((pos_ - k + W) % W)]);
        // boxcar mean of the min-filtered gain over the last W samples
        boxSum_ += double(m) - double(gminOut_[static_cast<size_t>(pos_)]);
        gminOut_[static_cast<size_t>(pos_)] = m;
        const float g = static_cast<float>(boxSum_ / double(W));

        // Audio delayed by exactly L: write the new sample, then read the oldest slot (written L
        // samples ago). g(n) <= g_t(n-L) then holds, so x(n-L) is never amplified past the ceiling.
        dl_[static_cast<size_t>(pos_)] = l[i];
        dr_[static_cast<size_t>(pos_)] = r[i];
        const size_t oldest = static_cast<size_t>((pos_ + 1) % W);
        const float xl = dl_[oldest], xr = dr_[oldest];
        l[i] = xl * g;
        r[i] = xr * g;
        minGain = std::min(minGain, g);
        pos_ = (pos_ + 1) % W;
    }
    // Periodically resync the running sum so rounding drift cannot accumulate over hours.
    sinceSync_ += n;
    if (sinceSync_ >= (1 << 16)) {
        sinceSync_ = 0;
        double s = 0.0;
        for (int k = 0; k < W; ++k) s += double(gminOut_[static_cast<size_t>(k)]);
        boxSum_ = s;
    }
    grDb_ = 20.0f * std::log10(std::max(minGain, 1e-6f));
}

}  // namespace ddaw::dsp
