// `autofilt` effect: port of sf-dsp/src/fx/autofilt.rs, the Tone.AutoFilter composite. A sine LFO drives
// the cutoff of a lowpass (Tone.Filter defaults: -12 dB/oct, Q = 1):
//   cutoff = base + (base*2^3.5 - base) * (1 + depth*sin(2*pi*f*t)) / 2
// (makeEffect fixes octaves = 3.5). Wet/dry is Tone's CrossFade (equal-power cos/sin).
// Rate: rateSwitch knob. rateMode 0 Sync = tempo division `rateSync` retuned from ProcessContext::bpm
// (Rust tick()), 1 Low = `rate` Hz, 2 High = `rateHi` Hz.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>

#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/Lfo.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

enum P : uint16_t { Rate, Depth, Base, Mix, RateMode, RateSync, RateHi };

constexpr float kOctavesPow = 11.313708f;  // 2^3.5

// schema.ts LFO_DIV_TICKS: cycle length in transport ticks per rateSync index (PPQ 96).
constexpr std::array<double, 9> kDivTicks = {3072.0, 1536.0, 768.0, 384.0, 192.0, 96.0, 48.0, 32.0, 24.0};

class AutofiltFx final : public EffectDevice {
public:
    AutofiltFx() {
        depth_.snap(0.7f);  // schema defaults
        base_.snap(350.0f);
        mix_.snap(1.0f);
        lfo_.setShape(0);
        updateStep();
    }

    std::span<const ParamSpec> params() const override { return schema::kFxAutofilt; }

    void prepare(double sr, int) override {
        sr_ = static_cast<float>(std::max(sr, 1.0));
        for (auto* s : {&depth_, &base_, &mix_}) {
            s->prepare(sr, 15.0f);
            s->snap(s->target());
        }
        lfo_.prepare(sr_);
        filtL_.prepare(sr_);
        filtR_.prepare(sr_);
        updateStep();
        lfo_.setPhase(0.0);
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Rate: rateLo_ = v; updateStep(); break;
            case Depth: depth_.setTarget(std::clamp(v, 0.0f, 1.0f)); break;
            case Base: base_.setTarget(std::max(v, 1.0f)); break;
            case Mix: mix_.setTarget(std::clamp(v, 0.0f, 1.0f)); break;
            case RateMode: rateMode_ = std::clamp(static_cast<int>(v), 0, 2); updateStep(); break;
            case RateSync: rateSync_ = std::clamp(static_cast<int>(v), 0, static_cast<int>(kDivTicks.size()) - 1); updateStep(); break;
            case RateHi: rateHi_ = v; updateStep(); break;
            default: break;
        }
    }

    void process(float* l, float* r, int n, const ProcessContext& ctx, const ModInputs&) override {
        // Rust tick(): retune the synced rate when the transport tempo changes.
        if (ctx.bpm > 0.0 && ctx.bpm != bpm_) {
            bpm_ = ctx.bpm;
            if (rateMode_ == 0) updateStep();
        }
        constexpr float kHalfPi = std::numbers::pi_v<float> * 0.5f;
        for (int i = 0; i < n; ++i) {
            const float depth = depth_.next(), base = base_.next(), mix = mix_.next();
            const float dryG = std::cos(mix * kHalfPi), wetG = std::sin(mix * kHalfPi);  // Tone CrossFade
            const float s = lfo_.next();
            // LFO -> cutoff: linear Scale between base and base*2^3.5.
            const float span = base * (kOctavesPow - 1.0f);
            const float cutoff = base + span * 0.5f * (1.0f + depth * s);
            filtL_.setCutoffQ(cutoff, 1.0f);
            filtR_.setCutoffQ(cutoff, 1.0f);
            const float wl = filtL_.processSample(l[i]);
            const float wr = filtR_.processSample(r[i]);
            l[i] = l[i] * dryG + wl * wetG;
            r[i] = r[i] * dryG + wr * wetG;
        }
    }

    void reset() override {
        filtL_.reset();
        filtR_.reset();
        for (auto* s : {&depth_, &base_, &mix_}) s->snap(s->target());
        lfo_.setPhase(0.0);
    }

private:
    // schema.ts rateHzOf(): effective LFO frequency for the stored rate state.
    float rateHz() const {
        switch (rateMode_) {
            case 2: return std::clamp(rateHi_, 301.0f, 1000.0f);
            case 0: {
                const double ticks = kDivTicks[std::min<size_t>(static_cast<size_t>(rateSync_), kDivTicks.size() - 1)];
                return static_cast<float>((std::max(bpm_, 1.0) * 96.0) / (60.0 * ticks));
            }
            default: return std::clamp(rateLo_, 0.01f, 300.0f);
        }
    }
    void updateStep() { lfo_.setFreq(rateHz()); }  // phase-continuous retune: no smoother needed

    float sr_ = 44100.0f;
    double bpm_ = 120.0;
    int rateMode_ = 1;  // schema default: Low
    float rateLo_ = 1.0f, rateHi_ = 440.0f;
    int rateSync_ = 5;
    Lfo lfo_;
    Smoother depth_, base_, mix_;
    Svf filtL_{SvfMode::Lowpass}, filtR_{SvfMode::Lowpass};
};

}  // namespace

std::unique_ptr<EffectDevice> make_autofilt() { return std::make_unique<AutofiltFx>(); }

}  // namespace ddaw::devices
