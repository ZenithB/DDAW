// `plate` effect: port of sf-dsp/src/fx/plate.rs. Pre-delay -> 4-stage allpass diffuser -> 8-line
// Householder FDN (per-line gains place -60 dB at `decay` s) with early diffuser taps; the wet path
// then gets the metallic tilt (1 - 0.3 z^-1), a damping lowpass, and a 1/sqrt(decay) level scale
// (mimicking the browser convolver normalisation). Wet = mix, dry = 1 - mix. All buffers are sized in
// prepare(). Schema: decay, predelay, damp, mix.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>

#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/DelayLine.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

enum P : uint16_t { Decay, Predelay, Damp, Mix };

constexpr int kN = 8;
constexpr float kLineMs[kN] = {7.9f, 11.3f, 13.7f, 17.9f, 21.3f, 26.1f, 31.7f, 38.3f};
constexpr float kDiffMs[4] = {0.73f, 1.31f, 2.19f, 3.41f};
constexpr float kDiffG = 0.65f;
constexpr float kTilt = 0.3f;
constexpr float kEarlyGain = 0.3f;
constexpr float kTapGain = 0.5f;
constexpr float kInject = 0.5f;
constexpr float kWetNorm = 0.5f;
constexpr float kPreMaxSec = 0.25f;
constexpr float kMinDecay = 0.15f;

// Schroeder allpass with a fixed integer delay.
struct Allpass {
    DelayLine line;
    int delay = 1;
    void prepare(int d) { delay = std::max(d, 1); line.prepare(delay + 1); }
    float tick(float x) noexcept {
        const float d = line.read(delay);
        const float v = x + kDiffG * d;
        line.write(v);
        return d - kDiffG * v;
    }
};

class PlateFx final : public EffectDevice {
public:
    PlateFx() {
        decay_.snap(1.8f);
        predelay_.snap(0.02f);
        damp_.snap(6000.0f);
        mix_.snap(0.3f);
        prepare(44100.0, 128);
    }

    std::span<const ParamSpec> params() const override { return schema::kFxPlate; }

    void prepare(double sr, int) override {
        sr_ = std::max(static_cast<float>(sr), 1.0f);
        const int preLen = static_cast<int>(kPreMaxSec * sr_) + 2;
        preL_.prepare(preLen);
        preR_.prepare(preLen);
        for (size_t i = 0; i < 4; ++i) diff_[i].prepare(msToSamples(kDiffMs[i]));
        for (size_t i = 0; i < kN; ++i) {
            lineLen_[i] = msToSamples(kLineMs[i]);
            lines_[i].prepare(lineLen_[i] + 2);
        }
        dampL_.prepare(sr_);
        dampR_.prepare(sr_);
        for (Smoother* s : {&decay_, &predelay_, &damp_, &mix_}) s->prepare(sr_, 15.0f);
        tiltPrevL_ = tiltPrevR_ = 0.0f;
        updateDecayGains(decay_.current());
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Decay: decay_.setTarget(std::clamp(v, kMinDecay, 20.0f)); break;
            case Predelay: predelay_.setTarget(std::clamp(v, 0.0f, 0.2f)); break;
            case Damp: damp_.setTarget(std::clamp(v, 100.0f, 18000.0f)); break;
            case Mix: mix_.setTarget(std::clamp(v, 0.0f, 1.0f)); break;
            default: break;
        }
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        if (n <= 0) return;
        // block-rate params (decay retune = 8 powf, damp = a tan), stepped at <= one block
        for (int i = 0; i < n; ++i) { decay_.next(); damp_.next(); }
        updateDecayGains(decay_.current());
        const float fc = std::clamp(damp_.current(), 100.0f, sr_ * 0.45f);
        const float q = std::numbers::sqrt2_v<float> * 0.5f;
        dampL_.setCutoffQ(fc, q);
        dampR_.setCutoffQ(fc, q);

        for (int i = 0; i < n; ++i) {
            const float xl = l[i], xr = r[i];

            const float pd = std::max(predelay_.next() * sr_, 1.0f);
            preL_.write(xl);
            preR_.write(xr);
            const float pl = preL_.readFrac(pd);
            const float pr = preR_.readFrac(pd);

            // allpass diffusion: instantly dense, like the plate IR
            const float a0 = diff_[0].tick(0.5f * (pl + pr));
            const float a1 = diff_[1].tick(a0);
            const float a2 = diff_[2].tick(a1);
            const float a3 = diff_[3].tick(a2);

            std::array<float, kN> d;
            float sumG = 0.0f;
            for (size_t k = 0; k < kN; ++k) {
                d[k] = lines_[k].read(lineLen_[k]);
                sumG += d[k] * gains_[k];
            }

            // Householder feedback (I - 2/N ones) + alternating-sign injection
            const float house = sumG * (2.0f / float(kN));
            float sign = 1.0f;
            for (size_t k = 0; k < kN; ++k) {
                lines_[k].write(d[k] * gains_[k] - house + sign * kInject * a3);
                sign = -sign;
            }

            const float wetL = kTapGain * (d[0] - d[2] + d[4] - d[6]) + kEarlyGain * a2;
            const float wetR = kTapGain * (d[1] - d[3] + d[5] - d[7]) + kEarlyGain * a3;

            // metallic tilt (1 - TILT z^-1), then the damping lowpass
            const float tl = wetL - kTilt * tiltPrevL_;
            tiltPrevL_ = wetL;
            const float tr = wetR - kTilt * tiltPrevR_;
            tiltPrevR_ = wetR;
            const float wl = dampL_.processSample(tl) * wetNorm_;
            const float wr = dampR_.processSample(tr) * wetNorm_;

            const float m = mix_.next();
            l[i] = (1.0f - m) * xl + m * wl;
            r[i] = (1.0f - m) * xr + m * wr;
        }
    }

    void reset() override {
        preL_.clear();
        preR_.clear();
        for (auto& ap : diff_) ap.line.clear();
        for (auto& line : lines_) line.clear();
        dampL_.reset();
        dampR_.reset();
        tiltPrevL_ = tiltPrevR_ = 0.0f;
        for (Smoother* s : {&decay_, &predelay_, &damp_, &mix_}) s->snap(s->target());
        updateDecayGains(decay_.current());
    }

private:
    int msToSamples(float ms) const { return std::max(static_cast<int>(ms * 1e-3f * sr_), 1); }

    // Per-line feedback so the tail hits -60 dB at `decay` seconds, plus the 1/sqrt(decay) wet scale.
    void updateDecayGains(float decaySec) {
        const float t = std::max(decaySec, kMinDecay);
        for (size_t k = 0; k < kN; ++k)
            gains_[k] = std::min(std::pow(10.0f, -3.0f * float(lineLen_[k]) / (sr_ * t)), 0.9995f);
        wetNorm_ = kWetNorm / std::sqrt(t);
    }

    float sr_ = 44100.0f;
    DelayLine preL_, preR_;
    std::array<Allpass, 4> diff_;
    std::array<DelayLine, kN> lines_;
    std::array<int, kN> lineLen_{};
    std::array<float, kN> gains_{};
    float wetNorm_ = kWetNorm;
    float tiltPrevL_ = 0.0f, tiltPrevR_ = 0.0f;
    Svf dampL_{SvfMode::Lowpass}, dampR_{SvfMode::Lowpass};
    Smoother decay_, predelay_, damp_, mix_;
};

}  // namespace

std::unique_ptr<EffectDevice> make_plate() { return std::make_unique<PlateFx>(); }

}  // namespace ddaw::devices
