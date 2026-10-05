#pragma once
// Morph map (B5): a point on a 2D field blends the parameter sets stored at anchor points. The weights are
// non-negative and sum to 1, so a blend always stays inside the range spanned by the anchors.
//   Idw: Shepard inverse-distance weighting, w_i ~ 1 / d_i^power. Exact at an anchor (the stick on an anchor gives
//        that anchor's values), smooth elsewhere; a higher power makes each anchor's zone of influence flatter.
//   Rbf: normalised Gaussian kernels, w_i ~ exp(-d_i^2 / (2 width^2)). Smooth everywhere and independent of how far
//        away the other anchors are; it only approximates an anchor's values (width sets how closely). Far from
//        every anchor (the kernels underflow) it falls back to Idw.
// Real-time safe: no allocation, plain arithmetic.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ddaw::dsp {

enum class MorphMethod : uint8_t { Idw, Rbf };

// `w` receives n weights. n >= 1.
inline void morphWeights(MorphMethod method, float power, float width, const float* ax, const float* ay, int n, float x, float y, float* w) noexcept {
    if (n <= 0) return;
    if (n == 1) { w[0] = 1.0f; return; }
    float sum = 0.0f;
    if (method == MorphMethod::Rbf) {
        const float inv = 1.0f / (2.0f * std::max(width, 0.01f) * std::max(width, 0.01f));
        for (int i = 0; i < n; ++i) {
            const float dx = x - ax[i], dy = y - ay[i];
            w[i] = std::exp(-(dx * dx + dy * dy) * inv);
            sum += w[i];
        }
        if (sum > 1e-12f) { for (int i = 0; i < n; ++i) w[i] /= sum; return; }
    }
    const float half = 0.5f * std::clamp(power, 0.5f, 8.0f);
    sum = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float dx = x - ax[i], dy = y - ay[i];
        const float d2 = dx * dx + dy * dy;
        if (d2 < 1e-12f) {   // on an anchor: exactly that anchor
            for (int k = 0; k < n; ++k) w[k] = k == i ? 1.0f : 0.0f;
            return;
        }
        w[i] = std::pow(d2, -half);
        sum += w[i];
    }
    for (int i = 0; i < n; ++i) w[i] /= sum;
}

// Response curve of one target: u in 0..1, exponent c (c < 1 lifts the low end, c > 1 holds it back).
inline float morphShape(float u, float c) noexcept {
    u = std::clamp(u, 0.0f, 1.0f);
    return (c > 0.999f && c < 1.001f) ? u : std::pow(u, std::clamp(c, 0.1f, 10.0f));
}

}  // namespace ddaw::dsp
