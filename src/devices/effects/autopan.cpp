// `autopan` effect: port of sf-dsp/src/fx/autopan.rs (the Tone.AutoPanner composite). A sine LFO
// drives a Web Audio StereoPannerNode: pan(t) = depth * sin(2 pi f t), with the node's *stereo*
// algorithm (the retreating channel folds into the other side through an equal-power cos/sin
// split). No mix knob: always fully wet; pan 0 is exact passthrough. Schema: rate 0.01..300 Hz,
// depth 0..1.
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

enum P : uint16_t { Rate, Depth, RateMode, RateSync, RateHi };

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

class AutopanFx final : public EffectDevice {
public:
    AutopanFx() {
        depth_.snap(0.8f);
        rate_.lo = 1.5f;
        applyRate();
    }

    std::span<const ParamSpec> params() const override { return schema::kFxAutopan; }

    void prepare(double sr, int) override {
        const float srf = static_cast<float>(std::max(sr, 1.0));
        depth_.prepare(srf, 15.0f);
        depth_.snap(depth_.target());
        lfo_.prepare(srf);
        applyRate();
        lfo_.setPhase(0.0);
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Rate: rate_.lo = v; applyRate(); break;
            case RateMode: rate_.mode = std::clamp(static_cast<int>(v), 0, 2); applyRate(); break;
            case RateSync: rate_.sync = std::clamp(static_cast<int>(v), 0, 8); applyRate(); break;
            case RateHi: rate_.hi = v; applyRate(); break;
            case Depth: depth_.setTarget(std::clamp(v, 0.0f, 1.0f)); break;
            default: break;
        }
    }

    void process(float* l, float* r, int n, const ProcessContext& ctx, const ModInputs&) override {
        if (rate_.setBpm(ctx.bpm)) applyRate();
        for (int i = 0; i < n; ++i) {
            const float pan = std::clamp(depth_.next() * lfo_.next(), -1.0f, 1.0f);
            const float ls = l[i], rs = r[i];
            if (pan <= 0.0f) {
                const float x = (pan + 1.0f) * kHalfPi;
                l[i] = ls + rs * std::cos(x);
                r[i] = rs * std::sin(x);
            } else {
                const float x = pan * kHalfPi;
                l[i] = ls * std::cos(x);
                r[i] = rs + ls * std::sin(x);
            }
        }
    }

    void reset() override {
        depth_.snap(depth_.target());
        lfo_.setPhase(0.0);
    }

private:
    void applyRate() { lfo_.setFreq(rate_.hz()); }
    RateState rate_;
    Lfo lfo_;
    Smoother depth_;
};

}  // namespace

std::unique_ptr<EffectDevice> make_autopan() { return std::make_unique<AutopanFx>(); }

}  // namespace ddaw::devices
