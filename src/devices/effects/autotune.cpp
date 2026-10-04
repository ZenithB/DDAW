// `autotune` effect: port of sf-dsp/src/fx/autotune.rs (the `sf-autotune` worklet).
// A dual-tap "tape splice" granular pitch shifter (60 ms window) driven by an in-band YIN detector
// that runs on a 2x-decimated mono mix every 512 input samples over a 1024-sample window. The
// detected MIDI pitch is snapped to the project key/scale (or the nearest semitone in chromatic
// mode) and the correction in semitones glides toward its target with a `speed` ms time constant.
// Wet/dry is Tone.CrossFade's equal-power law. Schema: amount, speed (ms), mix, mode (0 Key / 1 Chr).
//
// Project key: the Rust device also receives `root` (pitch class, def 9 = A) and `mask` (root-relative
// scale bitmask, def 0x5AD = minor) as extra set_param keys. The C++ schema has no such params and
// ProcessContext has no key fields, so they stay at the Rust defaults (A minor); see the report.
// Latency: none declared. The splice delay is 1 sample at unity ratio and sweeps 0..60 ms while
// a correction is being applied; the Rust device has no fixed lookahead.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>

#include "core/Constants.h"
#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/DelayLine.h"
#include "dsp/Smoother.h"

namespace ddaw::devices {
namespace {

enum P : uint16_t { Amount, Speed, Mix, Mode };

constexpr int kDetectN = 1024;    // decimated samples
constexpr int kDetectHop = 512;   // input samples between detections
constexpr int kRingLen = 4096;
constexpr int kRingMask = kRingLen - 1;

// YIN (cumulative-mean-normalised difference) with early exit at the first sub-threshold dip's local
// minimum; `sr` is the decimated rate.
float yinDetect(const std::array<float, kDetectN>& buf, float sr) {
    const int n = kDetectN;
    const int maxTau = std::min(n - 256, static_cast<int>(std::floor(sr / 70.0f)));
    const int minTau = std::max(2, static_cast<int>(std::floor(sr / 1100.0f)));
    if (maxTau <= minTau + 2) return 0.0f;
    const int lim = n - maxTau;
    double running = 0.0, best = 1.0, prev = 1.0;
    long bt = -1, found = -1;
    for (int tau = minTau; tau < maxTau; ++tau) {
        double sum = 0.0;
        for (int i = 0; i < lim; ++i) {
            const double d = static_cast<double>(buf[size_t(i)] - buf[size_t(i + tau)]);
            sum += d * d;
        }
        running += sum;
        const double c = running > 0.0 ? sum * double(tau - minTau + 1) / running : 1.0;
        if (found >= 0) {
            if (c < prev) { found = tau; prev = c; continue; }  // still descending
            break;                                               // local minimum found
        }
        if (c < 0.15) { found = tau; prev = c; continue; }
        if (c < best) { best = c; bt = tau; }
        prev = c;
    }
    long tau = found;
    if (tau < 0) {
        if (best > 0.4) return 0.0f;
        tau = bt;
    }
    return tau > 0 ? sr / static_cast<float>(tau) : 0.0f;
}

// Dual-tap granular shifter (`ShiftCore`): two taps half a window apart whose delay ramps 0..window at
// rate (1 - ratio); each tap sin-fades to zero exactly when its delay wraps.
class ShiftCore {
public:
    void prepare(float sampleRate) {
        const int maxD = static_cast<int>(std::ceil(sampleRate * 0.6f));
        dlL_.prepare(maxD);
        dlR_.prepare(maxD);
        reset();
    }
    void setPitch(float semi) { target_ = std::pow(2.0f, semi / 12.0f); }
    void reset() {
        ratio_ = 1.0f; target_ = 1.0f; ph_ = 0.0;
        dlL_.clear(); dlR_.clear();
    }
    void block(const float* inL, const float* inR, float* wetL, float* wetR, int n, float sampleRate) {
        const float winS = win_ * sampleRate;
        double ph = ph_;
        for (int i = 0; i < n; ++i) {
            ratio_ += (target_ - ratio_) * 0.01f;  // ~2 ms glide so pitch changes don't zipper
            const double inc = (1.0 - static_cast<double>(ratio_)) / static_cast<double>(winS);
            dlL_.write(inL[i]);
            dlR_.write(inR[i]);
            ph += inc;
            ph -= std::floor(ph);
            const double phB = std::fmod(ph + 0.5, 1.0);
            // The worklet reads d back from the just-written sample (d = 0 newest); here newest is 1.
            const float dA = static_cast<float>(ph) * winS + 1.0f;
            const float dB = static_cast<float>(phB) * winS + 1.0f;
            const float gA = std::sin(std::numbers::pi_v<float> * static_cast<float>(ph));
            const float gB = std::sin(std::numbers::pi_v<float> * static_cast<float>(phB));
            wetL[i] = dlL_.readFrac(dA) * gA + dlL_.readFrac(dB) * gB;
            wetR[i] = dlR_.readFrac(dA) * gA + dlR_.readFrac(dB) * gB;
        }
        ph_ = ph;
    }

private:
    float win_ = 0.06f;
    float ratio_ = 1.0f, target_ = 1.0f;
    double ph_ = 0.0;
    dsp::DelayLine dlL_, dlR_;
};

class AutotuneFx final : public EffectDevice {
public:
    AutotuneFx() { mix_.snap(1.0f); }

    std::span<const ParamSpec> params() const override { return schema::kFxAutotune; }

    void prepare(double sr, int) override {
        sr_ = static_cast<float>(std::max(sr, 1.0));
        core_.prepare(sr_);
        mix_.prepare(sr_, 15.0f);
        reset();
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Amount: amount_ = v; break;
            case Speed: speedMs_ = v; break;
            case Mix: mix_.setTarget(v); if (fresh_) mix_.snap(v); break;  // pre-audio: no glide from the default
            case Mode: mode_ = static_cast<int>(v); break;
            default: break;
        }
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        fresh_ = false;
        int i = 0;
        while (i < n) {
            const int m = std::min(n - i, kMaxBlock);
            processChunk(l + i, r + i, m);
            i += m;
        }
    }

    void reset() override {
        core_.reset();
        cur_ = 0.0f;
        desired_ = 0.0f;
        ring_.fill(0.0f);
        rw_ = 0;
        decimPending_ = false;
        decimHold_ = 0.0f;
        sinceHop_ = 0;
        mix_.snap(mix_.target());
        fresh_ = true;
    }

private:
    bool inMask(int pitch) const {
        int d = (pitch - root_) % 12;
        if (d < 0) d += 12;
        return ((mask_ >> d) & 1u) == 1u;
    }
    int snap(int pitch) const {
        if (mode_ == 1 || inMask(pitch)) return pitch;
        for (int off = 1; off <= 6; ++off) {
            if (inMask(pitch - off)) return pitch - off;
            if (inMask(pitch + off)) return pitch + off;
        }
        return pitch;
    }

    void detect() {
        const int start = (rw_ + kRingLen - kDetectN) & kRingMask;
        for (int i = 0; i < kDetectN; ++i) ana_[size_t(i)] = ring_[size_t((start + i) & kRingMask)];
        double rms = 0.0;
        for (float a : ana_) rms += static_cast<double>(a * a);
        rms = std::sqrt(rms / kDetectN);
        if (rms <= 0.004) return;  // silence: hold the correction
        const float f = yinDetect(ana_, sr_ * 0.5f);
        if (f <= 0.0f) return;
        const float midi = 69.0f + 12.0f * std::log2(f / 440.0f);
        const int target = snap(static_cast<int>(std::round(midi)));
        desired_ = std::clamp((static_cast<float>(target) - midi) * amount_, -12.0f, 12.0f);
    }

    // One <= kMaxBlock chunk, mirroring one worklet process() quantum.
    void processChunk(float* l, float* r, int n) {
        for (int i = 0; i < n; ++i) {  // feed the decimated detection ring (mono mix, pairwise average)
            const float x = (l[i] + r[i]) * 0.5f;
            if (decimPending_) {
                ring_[size_t(rw_)] = (decimHold_ + x) * 0.5f;
                rw_ = (rw_ + 1) & kRingMask;
                decimPending_ = false;
            } else {
                decimHold_ = x;
                decimPending_ = true;
            }
        }
        sinceHop_ += n;
        if (sinceHop_ >= kDetectHop) { sinceHop_ = 0; detect(); }
        const float coeff = 1.0f - static_cast<float>(std::exp(
            -(static_cast<double>(n) / static_cast<double>(sr_)) / std::max(static_cast<double>(speedMs_) / 1000.0, 0.001)));
        cur_ += (desired_ - cur_) * coeff;
        core_.setPitch(cur_);
        std::array<float, kMaxBlock> wetL, wetR;
        core_.block(l, r, wetL.data(), wetR.data(), n, sr_);
        constexpr float kHalfPi = std::numbers::pi_v<float> / 2.0f;
        for (int i = 0; i < n; ++i) {  // equal-power dry/wet (Tone.CrossFade)
            const float m = std::clamp(mix_.next(), 0.0f, 1.0f);
            const float dry = std::cos(m * kHalfPi);
            const float wet = std::sin(m * kHalfPi);
            l[i] = l[i] * dry + wetL[size_t(i)] * wet;
            r[i] = r[i] * dry + wetR[size_t(i)] * wet;
        }
    }

    float sr_ = 44100.0f;
    ShiftCore core_;
    float amount_ = 1.0f, speedMs_ = 20.0f;
    dsp::Smoother mix_;
    int mode_ = 0;
    int root_ = 9;                 // A; no project-key input yet
    uint32_t mask_ = 0x5AD;        // minor; no project-key input yet
    float cur_ = 0.0f, desired_ = 0.0f;
    std::array<float, kRingLen> ring_{};
    int rw_ = 0;
    bool decimPending_ = false;
    float decimHold_ = 0.0f;
    int sinceHop_ = 0;
    std::array<float, kDetectN> ana_{};
    bool fresh_ = true;
};

}  // namespace

std::unique_ptr<EffectDevice> make_autotune() { return std::make_unique<AutotuneFx>(); }

}  // namespace ddaw::devices
