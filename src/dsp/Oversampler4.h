#pragma once
// 4x oversampling wrapper for nonlinear devices (dist, crush, cheby). Two cascaded 2x polyphase
// halfband FIR stages each way: a sharp 23-tap stage at the base rate and a relaxed 11-tap stage at
// 2x (about 34 taps in total). Buffers are sized for kMaxBlock at construction; process() does not
// allocate. Port of sf-dsp/src/util/oversample.rs.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

#include "core/Constants.h"

namespace ddaw::dsp {

namespace detail {

// Windowed-sinc halfband taps (Blackman). Odd length, centre tap 0.5, every other side tap zero.
// Side taps are normalised so each polyphase branch has exactly unity DC gain.
inline std::vector<float> halfbandTaps(int n) {
    const int m = (n - 1) / 2;
    std::vector<float> h(static_cast<size_t>(n), 0.0f);
    for (int j = 0; j < n; ++j) {
        const int k = j - m;
        if (k == 0) { h[size_t(j)] = 0.5f; continue; }
        if (k % 2 == 0) continue;  // halfband zeros
        const double x = std::numbers::pi * k * 0.5;
        const double ideal = std::sin(x) / (2.0 * x);
        const double w = 0.42 - 0.5 * std::cos(2.0 * std::numbers::pi * j / (n - 1)) +
                         0.08 * std::cos(2.0 * 2.0 * std::numbers::pi * j / (n - 1));
        h[size_t(j)] = static_cast<float>(ideal * w);
    }
    float side = 0.0f;
    for (int j = 0; j < n; ++j) if (j != m) side += h[size_t(j)];
    if (side > 0.0f) {
        const float scale = 0.5f / side;
        for (int j = 0; j < n; ++j) if (j != m) h[size_t(j)] *= scale;
    }
    return h;
}

inline size_t nextPow2(size_t v) { size_t p = 1; while (p < v) p <<= 1; return p; }

// Polyphase 2x upsampler: even output phase = FIR over the even taps (x2); odd phase = delayed input.
class Up2 {
public:
    explicit Up2(int n) {
        const auto h = halfbandTaps(n);
        for (size_t i = 0; i < h.size(); i += 2) evenTaps_.push_back(h[i] * 2.0f);
        const int m = (n - 1) / 2;
        hist_.assign(nextPow2(std::max(evenTaps_.size(), size_t(m / 2 + 1))), 0.0f);
        centerDelay_ = static_cast<size_t>((m - 1) / 2);
    }
    void reset() { std::fill(hist_.begin(), hist_.end(), 0.0f); pos_ = 0; }
    // One input sample, two output samples.
    void push(float x, float& even, float& odd) noexcept {
        const size_t mask = hist_.size() - 1;
        hist_[pos_] = x;
        float acc = 0.0f;
        for (size_t i = 0; i < evenTaps_.size(); ++i) acc += evenTaps_[i] * hist_[(pos_ + hist_.size() - i) & mask];
        odd = hist_[(pos_ + hist_.size() - centerDelay_) & mask];
        pos_ = (pos_ + 1) & mask;
        even = acc;
    }
private:
    std::vector<float> evenTaps_, hist_;
    size_t pos_ = 0, centerDelay_ = 0;
};

// Polyphase 2x downsampler: even-phase FIR plus the centre tap on the odd phase.
class Down2 {
public:
    explicit Down2(int n) {
        const auto h = halfbandTaps(n);
        for (size_t i = 0; i < h.size(); i += 2) evenTaps_.push_back(h[i]);
        const size_t m = static_cast<size_t>((n - 1) / 2);
        centerDelay_ = (m + 1) / 2;
        histEven_.assign(nextPow2(evenTaps_.size()), 0.0f);
        histOdd_.assign(nextPow2(centerDelay_ + 1), 0.0f);
    }
    void reset() { std::fill(histEven_.begin(), histEven_.end(), 0.0f); std::fill(histOdd_.begin(), histOdd_.end(), 0.0f); posE_ = posO_ = 0; }
    // Two high-rate samples in, one out.
    float pushPair(float v0, float v1) noexcept {
        const size_t me = histEven_.size() - 1, mo = histOdd_.size() - 1;
        histEven_[posE_] = v0;
        histOdd_[posO_] = v1;
        float acc = 0.0f;
        for (size_t i = 0; i < evenTaps_.size(); ++i) acc += evenTaps_[i] * histEven_[(posE_ + histEven_.size() - i) & me];
        acc += 0.5f * histOdd_[(posO_ + histOdd_.size() - centerDelay_) & mo];
        posE_ = (posE_ + 1) & me;
        posO_ = (posO_ + 1) & mo;
        return acc;
    }
private:
    std::vector<float> evenTaps_, histEven_, histOdd_;
    size_t posE_ = 0, posO_ = 0, centerDelay_ = 0;
};

}  // namespace detail

class Oversampler4 {
public:
    static constexpr int kSharpTaps = 23;  // stage at the base rate
    static constexpr int kRelaxTaps = 11;  // stage at 2x

    Oversampler4()
        : up1L_(kSharpTaps), up1R_(kSharpTaps), up2L_(kRelaxTaps), up2R_(kRelaxTaps),
          dn2L_(kRelaxTaps), dn2R_(kRelaxTaps), dn1L_(kSharpTaps), dn1R_(kSharpTaps),
          b2L_(2 * kMaxBlock), b2R_(2 * kMaxBlock), b4L_(4 * kMaxBlock), b4R_(4 * kMaxBlock) {}

    void reset() {
        up1L_.reset(); up1R_.reset(); up2L_.reset(); up2R_.reset();
        dn2L_.reset(); dn2R_.reset(); dn1L_.reset(); dn1R_.reset();
        std::fill(b2L_.begin(), b2L_.end(), 0.0f); std::fill(b2R_.begin(), b2R_.end(), 0.0f);
        std::fill(b4L_.begin(), b4L_.end(), 0.0f); std::fill(b4R_.begin(), b4R_.end(), 0.0f);
    }

    // Upsample 4x, run f(float* l, float* r, int n4) on the high-rate block, downsample back in place.
    // n <= kMaxBlock.
    template <class F>
    void process(float* l, float* r, int n, F&& f) noexcept {
        n = std::min(n, kMaxBlock);
        for (int i = 0; i < n; ++i) {
            up1L_.push(l[i], b2L_[size_t(2 * i)], b2L_[size_t(2 * i + 1)]);
            up1R_.push(r[i], b2R_[size_t(2 * i)], b2R_[size_t(2 * i + 1)]);
        }
        for (int i = 0; i < 2 * n; ++i) {
            up2L_.push(b2L_[size_t(i)], b4L_[size_t(2 * i)], b4L_[size_t(2 * i + 1)]);
            up2R_.push(b2R_[size_t(i)], b4R_[size_t(2 * i)], b4R_[size_t(2 * i + 1)]);
        }
        f(b4L_.data(), b4R_.data(), 4 * n);
        for (int i = 0; i < 2 * n; ++i) {
            b2L_[size_t(i)] = dn2L_.pushPair(b4L_[size_t(2 * i)], b4L_[size_t(2 * i + 1)]);
            b2R_[size_t(i)] = dn2R_.pushPair(b4R_[size_t(2 * i)], b4R_[size_t(2 * i + 1)]);
        }
        for (int i = 0; i < n; ++i) {
            l[i] = dn1L_.pushPair(b2L_[size_t(2 * i)], b2L_[size_t(2 * i + 1)]);
            r[i] = dn1R_.pushPair(b2R_[size_t(2 * i)], b2R_[size_t(2 * i + 1)]);
        }
    }

private:
    detail::Up2 up1L_, up1R_, up2L_, up2R_;
    detail::Down2 dn2L_, dn2R_, dn1L_, dn1R_;
    std::vector<float> b2L_, b2R_, b4L_, b4R_;
};

}  // namespace ddaw::dsp
