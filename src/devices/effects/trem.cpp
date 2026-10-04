// `trem` effect: port of sf-dsp/src/fx/trem.rs (the Tone.Tremolo composite). Per-channel gain
// g(t) = (1 - depth * sin(2 pi f t + phi)) / 2, phi_L = 60 deg, phi_R = 120 deg (Tone spread 60),
// so the wet path swings around a 0.5 midpoint (between (1-depth)/2 and (1+depth)/2). Wet/dry is
// Tone's equal-power CrossFade: out = x*cos + x*g*sin. The LFO keeps running at depth 0 (as in the
// Rust port). Schema: rate 0.01..300 Hz, depth 0..1, mix 0..1.
// Rate: rateMode 0 Sync (rateSync division at the transport bpm, read per block from ProcessContext),
// 1 Low (`rate` Hz), 2 High (`rateHi` Hz), as schema.ts rateHzOf().
#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>

#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/Lfo.h"
#include "dsp/Smoother.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

enum P : uint16_t { Rate, Depth, Mix, RateMode, RateSync, RateHi };

constexpr float kPhaseL = 60.0f / 360.0f;    // cycles
constexpr float kPhaseR = 120.0f / 360.0f;
constexpr float kHalfPi = std::numbers::pi_v<float> * 0.5f;

// schema.ts LFO_DIV_TICKS: cycle length in transport ticks per rateSync index (PPQ 96).
constexpr double kDivTicks[9] = {3072.0, 1536.0, 768.0, 384.0, 192.0, 96.0, 48.0, 32.0, 24.0};

// schema.ts rateHzOf(): effective LFO frequency for the stored rateMode/rate/rateHi/rateSync.
struct RateState {
    int mode = 1;          // 0 Sync, 1 Low (rate), 2 High (rateHi)
    int sync = 5;
    float lo = 1.0f, hi = 440.0f;
    double bpm = 120.0;
    float hz() const noexcept {
        if (mode == 2) return std::clamp(hi, 301.0f, 1000.0f);
        if (mode == 0) return static_cast<float>((std::max(bpm, 1.0) * 96.0) / (60.0 * kDivTicks[std::clamp(sync, 0, 8)]));
        return std::clamp(lo, 0.01f, 300.0f);
    }
    // Block-boundary tempo update (Rust tick()). Returns true when the sync rate changed.
    bool setBpm(double b) noexcept {
        if (!(b > 0.0) || b == bpm) return false;
        bpm = b;
        return mode == 0;
    }
};

class TremFx final : public EffectDevice {
public:
    TremFx() {
        depth_.snap(0.6f);
        mix_.snap(1.0f);
        lfoL_.setPhaseOffset(kPhaseL);
        lfoR_.setPhaseOffset(kPhaseR);
        rate_.lo = 5.0f;
        applyRate();
    }

    std::span<const ParamSpec> params() const override { return schema::kFxTrem; }

    void prepare(double sr, int) override {
        const float sr_ = static_cast<float>(std::max(sr, 1.0));
        depth_.prepare(sr_, 15.0f);
        mix_.prepare(sr_, 15.0f);
        depth_.snap(depth_.target());
        mix_.snap(mix_.target());
        lfoL_.prepare(sr_);
        lfoR_.prepare(sr_);
        applyRate();
        lfoL_.setPhase(0.0);
        lfoR_.setPhase(0.0);
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Rate: rate_.lo = v; applyRate(); break;
            case RateMode: rate_.mode = std::clamp(static_cast<int>(v), 0, 2); applyRate(); break;
            case RateSync: rate_.sync = std::clamp(static_cast<int>(v), 0, 8); applyRate(); break;
            case RateHi: rate_.hi = v; applyRate(); break;
            case Depth: depth_.setTarget(std::clamp(v, 0.0f, 1.0f)); break;
            case Mix: mix_.setTarget(std::clamp(v, 0.0f, 1.0f)); break;
            default: break;
        }
    }

    void process(float* l, float* r, int n, const ProcessContext& ctx, const ModInputs&) override {
        if (rate_.setBpm(ctx.bpm)) applyRate();
        for (int i = 0; i < n; ++i) {
            const float depth = depth_.next();
            const float mix = mix_.next();
            const float dryG = std::cos(mix * kHalfPi);
            const float wetG = std::sin(mix * kHalfPi);
            const float gl = 0.5f * (1.0f - depth * lfoL_.next());
            const float gr = 0.5f * (1.0f - depth * lfoR_.next());
            l[i] = l[i] * dryG + l[i] * gl * wetG;
            r[i] = r[i] * dryG + r[i] * gr * wetG;
        }
    }

    void reset() override {
        depth_.snap(depth_.target());
        mix_.snap(mix_.target());
        lfoL_.setPhase(0.0);
        lfoR_.setPhase(0.0);
    }

private:
    void applyRate() { const float hz = rate_.hz(); lfoL_.setFreq(hz); lfoR_.setFreq(hz); }

    RateState rate_;
    Lfo lfoL_, lfoR_;
    Smoother depth_, mix_;
};

}  // namespace

std::unique_ptr<EffectDevice> make_trem() { return std::make_unique<TremFx>(); }

}  // namespace ddaw::devices
