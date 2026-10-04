#pragma once
// Cheap Moog-style ladder lowpass: four cascaded one-pole stages with resonance feedback and a
// soft-clipped input, for mono-bass duty. 12 or 24 dB/oct via the output tap.
// Port of sf-dsp/src/util/ladder.rs.
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace ddaw::dsp {

enum class LadderMode { Db12, Db24 };

class Ladder {
public:
    Ladder() { update(); }
    void prepare(float sampleRate) noexcept { sr_ = std::max(sampleRate, 1.0f); update(); reset(); }
    void setMode(LadderMode m) noexcept { mode_ = m; }
    // res in [0, 1]; about 1.0 approaches self-oscillation (k -> 4, clamped).
    void setCutoffRes(float hz, float res) noexcept {
        cutoff_ = std::clamp(hz, 10.0f, sr_ * 0.45f);
        res_ = std::clamp(res, 0.0f, 1.0f);
        update();
    }
    void reset() noexcept { s_.fill(0.0f); }
    float processSample(float x) noexcept {
        float v = sat(x - k_ * s_[3]);
        for (auto& st : s_) { st += g_ * (v - st); v = st; }
        return mode_ == LadderMode::Db12 ? s_[1] : s_[3];
    }
    void process(float* buf, int n) noexcept { for (int i = 0; i < n; ++i) buf[i] = processSample(buf[i]); }

private:
    void update() noexcept {
        const float t = std::tan(float(std::numbers::pi) * cutoff_ / sr_);  // bilinear-ish warp
        g_ = t / (1.0f + t);
        k_ = 3.98f * res_;
    }
    static float sat(float x) noexcept {  // cheap odd saturator, tanh-free
        x = std::clamp(x, -3.0f, 3.0f);
        return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
    }
    LadderMode mode_ = LadderMode::Db24;
    float g_ = 0, k_ = 0, sr_ = 44100.0f, cutoff_ = 1000.0f, res_ = 0.0f;
    std::array<float, 4> s_{};
};

}  // namespace ddaw::dsp
