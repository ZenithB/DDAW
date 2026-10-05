#pragma once
// sin(2*pi*x) from a 4096-point table with linear interpolation (error below -110 dB), for the inner loops of
// the oversampled FM/AM operators. x is a phase in cycles; any real value is accepted (it wraps).
#include <array>
#include <cmath>
#include <numbers>

namespace ddaw::dsp {

namespace detail {
constexpr int kSinBits = 12;
constexpr int kSinSize = 1 << kSinBits;
inline const std::array<float, kSinSize + 1> kSinTable = [] {
    std::array<float, kSinSize + 1> t{};
    for (int i = 0; i <= kSinSize; ++i) t[size_t(i)] = float(std::sin(2.0 * std::numbers::pi * double(i) / double(kSinSize)));
    return t;
}();
}  // namespace detail

inline float fastSin2Pi(float cycles) noexcept {
    cycles -= std::floor(cycles);
    const float p = cycles * float(detail::kSinSize);
    const int i = int(p);
    const float f = p - float(i);
    return detail::kSinTable[size_t(i)] + f * (detail::kSinTable[size_t(i) + 1] - detail::kSinTable[size_t(i)]);
}

}  // namespace ddaw::dsp
