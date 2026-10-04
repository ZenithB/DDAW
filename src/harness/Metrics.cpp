#include "harness/Metrics.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace ddaw::harness {

double rms(std::span<const float> x) {
    if (x.empty()) return 0.0;
    double s = 0.0;
    for (float v : x) s += double(v) * double(v);
    return std::sqrt(s / double(x.size()));
}

double toDb(double x) { return x <= 0.0 ? -160.0 : std::max(20.0 * std::log10(x), -160.0); }

double rmsNullDb(std::span<const float> a, std::span<const float> b) {
    size_t n = std::min(a.size(), b.size());
    if (n == 0) return 0.0;
    double diff = 0.0, ref = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double d = double(a[i]) - double(b[i]);
        diff += d * d;
        ref += double(b[i]) * double(b[i]);
    }
    if (diff == 0.0) return -160.0;
    if (ref == 0.0) return 160.0;
    return std::clamp(10.0 * std::log10(diff / ref), -160.0, 160.0);
}

namespace {
using Cx = std::complex<double>;

// In-place radix-2 FFT. n must be a power of two. Harness-only (allocation-free but unoptimised).
void fft(std::vector<Cx>& x) {
    const size_t n = x.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        Cx wl = std::polar(1.0, -2.0 * std::numbers::pi / double(len));
        for (size_t i = 0; i < n; i += len) {
            Cx w = 1.0;
            for (size_t k = 0; k < len / 2; ++k) {
                Cx u = x[i + k], v = x[i + k + len / 2] * w;
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}
}  // namespace

double spectralSimilarity(std::span<const float> a, std::span<const float> b, int fftLen, int hop) {
    const size_t n = std::min(a.size(), b.size());
    if (n < size_t(fftLen)) return 0.0;
    const auto len = static_cast<size_t>(fftLen);
    std::vector<double> hann(len);
    for (int i = 0; i < fftLen; ++i)
        hann[size_t(i)] = 0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * i / fftLen));
    std::vector<Cx> fa(len), fb(len);
    double sum = 0.0;
    size_t frames = 0;
    for (size_t pos = 0; pos + size_t(fftLen) <= n; pos += size_t(hop)) {
        for (size_t i = 0; i < size_t(fftLen); ++i) {
            fa[i] = double(a[pos + i]) * hann[i];
            fb[i] = double(b[pos + i]) * hann[i];
        }
        fft(fa); fft(fb);
        double dot = 0.0, na = 0.0, nb = 0.0;
        for (size_t k = 0; k <= size_t(fftLen) / 2; ++k) {
            double la = std::log(std::abs(fa[k]) + 1e-9), lb = std::log(std::abs(fb[k]) + 1e-9);
            dot += la * lb; na += la * la; nb += lb * lb;
        }
        if (na > 0.0 && nb > 0.0) sum += dot / std::sqrt(na * nb);
        ++frames;
    }
    return frames ? sum / double(frames) : 0.0;
}

std::vector<float> toMono(std::span<const float> l, std::span<const float> r) {
    size_t n = std::min(l.size(), r.size());
    std::vector<float> m(n);
    for (size_t i = 0; i < n; ++i) m[i] = 0.5f * (l[i] + r[i]);
    return m;
}

std::span<const float> skip(std::span<const float> x, size_t n) {
    return x.subspan(std::min(n, x.size()));
}

bool allFinite(std::span<const float> x) {
    return std::all_of(x.begin(), x.end(), [](float v) { return std::isfinite(v); });
}

}  // namespace ddaw::harness
