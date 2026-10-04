// `chorus` effect: port of sf-dsp/src/fx/chorus.rs (the Tone.Chorus composite). A delay line per
// channel, centre 3 ms, modulated by a sine LFO over +-(3 ms * depth); the right LFO runs in
// antiphase (Tone spread 180). Feedback is 0. Wet/dry is Tone's equal-power CrossFade (cos/sin).
// Schema: rate 0.01..300 Hz, depth 0..1, mix 0..1.
// Rate: rateMode 0 Sync (rateSync division at the transport bpm, read per block from ProcessContext),
// 1 Low (`rate` Hz), 2 High (`rateHi` Hz), as schema.ts rateHzOf().
#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>

#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/DelayLine.h"
#include "dsp/Lfo.h"
#include "dsp/Smoother.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

enum P : uint16_t { Rate, Depth, Mix, RateMode, RateSync, RateHi };

constexpr float kCenterSec = 0.003f;           // makeEffect("chorus"): delayTime 3 ms
constexpr float kMaxDelaySec = 2.0f * kCenterSec;
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

class ChorusFx final : public EffectDevice {
public:
    ChorusFx() {
        depth_.snap(0.5f);
        mix_.snap(0.5f);
        rate_.lo = 1.5f;
        applyRate();
    }

    std::span<const ParamSpec> params() const override { return schema::kFxChorus; }

    void prepare(double sr, int) override {
        sr_ = static_cast<float>(std::max(sr, 1.0));
        const int maxDelay = static_cast<int>(kMaxDelaySec * sr_) + 4;
        lineL_.prepare(maxDelay);
        lineR_.prepare(maxDelay);
        depth_.prepare(sr_, 15.0f);
        mix_.prepare(sr_, 15.0f);
        depth_.snap(depth_.target());
        mix_.snap(mix_.target());
        lfo_.prepare(sr_);
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
            case Mix: mix_.setTarget(std::clamp(v, 0.0f, 1.0f)); break;
            default: break;
        }
    }

    void process(float* l, float* r, int n, const ProcessContext& ctx, const ModInputs&) override {
        if (rate_.setBpm(ctx.bpm)) applyRate();
        const float center = kCenterSec * sr_;
        for (int i = 0; i < n; ++i) {
            const float dev = depth_.next() * center;   // LFO deviation in samples (delayTime * depth)
            const float mix = mix_.next();
            const float dryG = std::cos(mix * kHalfPi);
            const float wetG = std::sin(mix * kHalfPi);
            const float s = lfo_.next();
            // readFrac clamps at 1 sample, mirroring Tone's Math.max(min, 0).
            const float wl = lineL_.readFrac(center + dev * s);
            const float wr = lineR_.readFrac(center - dev * s);   // spread 180: antiphase
            lineL_.write(l[i]);
            lineR_.write(r[i]);
            l[i] = l[i] * dryG + wl * wetG;
            r[i] = r[i] * dryG + wr * wetG;
        }
    }

    void reset() override {
        lineL_.clear();
        lineR_.clear();
        depth_.snap(depth_.target());
        mix_.snap(mix_.target());
        lfo_.setPhase(0.0);
    }

private:
    void applyRate() { lfo_.setFreq(rate_.hz()); }
    RateState rate_;
    float sr_ = 44100.0f;
    Lfo lfo_;
    Smoother depth_, mix_;
    DelayLine lineL_, lineR_;
};

}  // namespace

std::unique_ptr<EffectDevice> make_chorus() { return std::make_unique<ChorusFx>(); }

}  // namespace ddaw::devices
