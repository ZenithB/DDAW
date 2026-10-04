#pragma once
// Radix-2 complex FFT with precomputed twiddles and bit reversal. Allocation-free after prepare().
#include <cmath>
#include <numbers>
#include <vector>

namespace ddaw::dsp {

class Fft {
public:
    void prepare(int n) {   // n must be a power of two
        n_ = n;
        cos_.resize(size_t(n / 2));
        sin_.resize(size_t(n / 2));
        for (int i = 0; i < n / 2; ++i) {
            cos_[size_t(i)] = std::cos(2.0 * std::numbers::pi * i / n);
            sin_[size_t(i)] = -std::sin(2.0 * std::numbers::pi * i / n);
        }
        rev_.resize(size_t(n));
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev_[size_t(i)] = r;
        }
    }
    int size() const noexcept { return n_; }
    // In place forward transform of (re, im), each n_ values.
    void forward(double* re, double* im) const noexcept {
        for (int i = 0; i < n_; ++i) {
            const int j = rev_[size_t(i)];
            if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        }
        for (int len = 2; len <= n_; len <<= 1) {
            const int half = len >> 1, step = n_ / len;
            for (int i = 0; i < n_; i += len)
                for (int k = 0; k < half; ++k) {
                    const double wr = cos_[size_t(k * step)], wi = sin_[size_t(k * step)];
                    const int a = i + k, b = a + half;
                    const double tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr; im[a] += ti;
                }
        }
    }

private:
    int n_ = 0;
    std::vector<double> cos_, sin_;
    std::vector<int> rev_;
};

}  // namespace ddaw::dsp
