#pragma once
// Measurement helpers shared by the B6 synth-family tests: render a device, read single-bin amplitudes and band energies.
#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>
#include <vector>

#include "DeviceTestKit.h"
#include "dsp/Fft.h"

namespace ddaw::testkit::b6 {

inline void set(InstrumentDevice& d, const char* key, float v) {
    for (const auto& p : d.params()) if (std::string(p.key) == key) { d.setParam(p.index, v); return; }
    FAIL("no parameter " << key);
}

// Stereo render of `blocks` blocks of kMaxBlock frames.
inline std::pair<std::vector<float>, std::vector<float>> renderLR(InstrumentDevice& d, int blocks, const ModInputs& mod = {}) {
    std::vector<float> outL, outR, l(kMaxBlock), r(kMaxBlock);
    for (int b = 0; b < blocks; ++b) {
        std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
        d.process(l.data(), r.data(), kMaxBlock, ctx(), mod);
        outL.insert(outL.end(), l.begin(), l.end());
        outR.insert(outR.end(), r.begin(), r.end());
    }
    return {outL, outR};
}
inline std::vector<float> render(InstrumentDevice& d, int blocks, const ModInputs& mod = {}) { return renderLR(d, blocks, mod).first; }

inline double hz(int pitch) { return 440.0 * std::pow(2.0, (pitch - 69) / 12.0); }

// Hann-windowed single-bin amplitude (relative to a unit sine) at `f` over x[from, from+len).
inline double bin(const std::vector<float>& x, size_t from, size_t len, double f, double sr = kSr) {
    REQUIRE(from + len <= x.size());   // an analysis window must lie inside the render
    double re = 0, im = 0, wsum = 0;
    for (size_t i = 0; i < len; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / double(len));
        const double ph = 2.0 * std::numbers::pi * f * double(from + i) / sr;
        re += w * x[from + i] * std::cos(ph);
        im -= w * x[from + i] * std::sin(ph);
        wsum += w;
    }
    return 2.0 * std::sqrt(re * re + im * im) / wsum;
}

// The largest bin amplitude within +-`span` Hz of `f` (a few steps), for partials that are not exactly on a bin.
inline double peak(const std::vector<float>& x, size_t from, size_t len, double f, double span, double sr = kSr) {
    double m = 0;
    for (int i = -8; i <= 8; ++i) m = std::max(m, bin(x, from, len, f + span * i / 8.0, sr));
    return m;
}

inline double rms(const std::vector<float>& x, size_t from, size_t len) {
    REQUIRE(from + len <= x.size());
    double s = 0;
    for (size_t i = from; i < from + len && i < x.size(); ++i) s += double(x[i]) * x[i];
    return std::sqrt(s / double(len));
}

inline double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1e-12)); }

// Mean frequency of the zero crossings in x[from, from+len): a pitch estimate for a clean periodic signal.
inline double zeroCrossHz(const std::vector<float>& x, size_t from, size_t len, double sr = kSr) {
    int c = 0;
    for (size_t i = from + 1; i < from + len && i < x.size(); ++i) if ((x[i - 1] < 0.0f) != (x[i] < 0.0f)) ++c;
    return 0.5 * double(c) * sr / double(len);
}

// Spectral centroid in Hz over [lo, hi] from the power spectrum (Hann window, FFT) of x[from, from+len); len a power of two.
inline double centroid(const std::vector<float>& x, size_t from, size_t len, double lo, double hi, int = 0, double sr = kSr) {
    REQUIRE(from + len <= x.size());
    dsp::Fft fft; fft.prepare(int(len));
    std::vector<double> re(len), im(len, 0.0);
    for (size_t i = 0; i < len; ++i) re[i] = double(x[from + i]) * (0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / double(len)));
    fft.forward(re.data(), im.data());
    double num = 0, den = 0;
    for (size_t k = 1; k < len / 2; ++k) {
        const double f = double(k) * sr / double(len);
        if (f < lo || f > hi) continue;
        const double p = re[k] * re[k] + im[k] * im[k];
        num += f * p; den += p;
    }
    return den > 0 ? num / den : 0.0;
}

// The share of the signal's power above `lo` Hz (a brightness measure that does not depend on level), from an FFT of
// x[from, from+len) with a Hann window; len a power of two.
inline double powerAbove(const std::vector<float>& x, size_t from, size_t len, double lo, double sr = kSr) {
    REQUIRE(from + len <= x.size());
    dsp::Fft fft; fft.prepare(int(len));
    std::vector<double> re(len), im(len, 0.0);
    for (size_t i = 0; i < len; ++i) re[i] = double(x[from + i]) * (0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / double(len)));
    fft.forward(re.data(), im.data());
    double above = 0, all = 0;
    for (size_t k = 1; k < len / 2; ++k) {
        const double p = re[k] * re[k] + im[k] * im[k];
        all += p;
        if (double(k) * sr / double(len) >= lo) above += p;
    }
    return all > 0 ? above / all : 0.0;
}

inline std::vector<float> minus(const std::vector<float>& a, const std::vector<float>& b) {
    std::vector<float> o(std::min(a.size(), b.size()));
    for (size_t i = 0; i < o.size(); ++i) o[i] = a[i] - b[i];
    return o;
}

}  // namespace ddaw::testkit::b6
