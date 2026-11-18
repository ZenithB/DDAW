#pragma once
// Small pieces shared by the effects that grew new controls after the synthyy port (ARCH 27): each is exactly transparent when its
// control is at the neutral value, so the golden fixtures (which never set the new keys) stay bit-identical.
#include <algorithm>
#include <cmath>
#include <numbers>

#include "dsp/OnePole.h"

namespace ddaw::dsp {

// Lowers a wet signal while the dry input is loud (a ducking delay or reverb: the tail comes back when the playing stops).
class WetDuck {
public:
    void prepare(float sr) noexcept { att_ = std::exp(-1.0f / (0.004f * sr)); rel_ = std::exp(-1.0f / (0.18f * sr)); env_ = 0.0f; }
    void reset() noexcept { env_ = 0.0f; }
    // The gain for the wet signal given the dry input's peak; exactly 1 (and the follower idle) when amount is 0.
    float gain(float inPeak, float amount) noexcept {
        if (amount <= 0.0f) { env_ = 0.0f; return 1.0f; }
        env_ = inPeak > env_ ? inPeak + (env_ - inPeak) * att_ : inPeak + (env_ - inPeak) * rel_;
        const float u = std::min(1.0f, env_ * 8.0f);                 // full ducking from -18 dBFS up
        return 1.0f - amount * u * u * (3.0f - 2.0f * u);
    }

private:
    float att_ = 0.99f, rel_ = 0.999f, env_ = 0.0f;
};

// A one-pole low-pass (a tone control) that is a straight wire at or above 20 kHz.
class ToneLp {
public:
    void prepare(float sr) noexcept { lp_.prepare(sr); }
    void reset() noexcept { lp_.reset(); }
    void set(float hz) noexcept {
        const bool on = hz < 19990.0f;
        if (on) { lp_.setCutoff(hz); if (!on_) lp_.reset(); }
        on_ = on;
    }
    float process(float x) noexcept { return on_ ? lp_.processSample(x) : x; }

private:
    OnePole lp_{OnePoleMode::Lowpass};
    bool on_ = false;
};

// A one-pole high-pass (a low cut) that is a straight wire at or below 20 Hz.
class LowCut {
public:
    void prepare(float sr) noexcept { hp_.prepare(sr); }
    void reset() noexcept { hp_.reset(); }
    void set(float hz) noexcept {
        const bool on = hz > 20.5f;
        if (on) { hp_.setCutoff(hz); if (!on_) hp_.reset(); }
        on_ = on;
    }
    float process(float x) noexcept { return on_ ? hp_.processSample(x) : x; }

private:
    OnePole hp_{OnePoleMode::Highpass};
    bool on_ = false;
};

// Slow, uneven drift for a tape-style wobble of a delay time (two sines that never line up), in -1..1.
class TapeDrift {
public:
    void prepare(float sr) noexcept { sr_ = sr; }
    void reset() noexcept { p1_ = p2_ = 0.0; }
    float next() noexcept {
        p1_ += 2.0 * std::numbers::pi * 0.63 / sr_; p2_ += 2.0 * std::numbers::pi * 1.91 / sr_;
        if (p1_ > 2.0 * std::numbers::pi) p1_ -= 2.0 * std::numbers::pi;
        if (p2_ > 2.0 * std::numbers::pi) p2_ -= 2.0 * std::numbers::pi;
        return float(0.75 * std::sin(p1_) + 0.25 * std::sin(p2_));
    }

private:
    double p1_ = 0, p2_ = 0, sr_ = 44100.0;
};

}  // namespace ddaw::dsp
