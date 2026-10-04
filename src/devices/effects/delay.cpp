// `delay` effect: port of sf-dsp/src/fx/delay.rs, the Tone.FeedbackDelay composite.
// Per-channel delay line with the wet output fed back to the delay input; equal-power (cos/sin)
// wet/dry crossfade. `time` is a stepped note-division index into kFractions (fractions of a whole
// note, whole = 240/bpm s) read from ProcessContext::bpm; the delay time in samples glides over
// 50 ms (the Rust rampTo), so division or tempo changes never click.
#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>

#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/DelayLine.h"
#include "dsp/Smoother.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

enum P : uint16_t { Time, Fb, Mix };

constexpr float kFractions[6] = {1.0f / 32.0f, 1.0f / 16.0f, 1.0f / 8.0f, 3.0f / 16.0f, 1.0f / 4.0f, 1.0f / 2.0f};
constexpr float kMaxDelaySec = 4.0f;  // Tone.FeedbackDelay { maxDelay: 4 }
constexpr float kTimeRampMs = 50.0f;  // devices.ts: delayTime.rampTo(..., 0.05)

class DelayFx final : public EffectDevice {
public:
    DelayFx() {
        delaySmp_.snap(delaySamples());
        fb_.snap(0.35f);
        mix_.snap(0.35f);
        prepare(44100.0, 128);
    }

    std::span<const ParamSpec> params() const override { return schema::kFxDelay; }

    void prepare(double sr, int) override {
        sr_ = std::max(static_cast<float>(sr), 1.0f);
        const int maxSamples = static_cast<int>(kMaxDelaySec * sr_) + 4;
        lineL_.prepare(maxSamples);
        lineR_.prepare(maxSamples);
        delaySmp_.prepare(sr_, kTimeRampMs);
        fb_.prepare(sr_, 15.0f);
        mix_.prepare(sr_, 15.0f);
        delaySmp_.snap(delaySamples());
        fb_.snap(fb_.target());
        mix_.snap(mix_.target());
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Time:  // the glide happens on the resulting time in samples, not on the index
                timeIdx_ = std::clamp(static_cast<int>(v), 0, 5);
                delaySmp_.setTarget(delaySamples());
                break;
            case Fb: fb_.setTarget(std::clamp(v, 0.0f, 0.9f)); break;  // devices.ts: min(0.9, v)
            case Mix: mix_.setTarget(std::clamp(v, 0.0f, 1.0f)); break;
            default: break;
        }
    }

    void process(float* l, float* r, int n, const ProcessContext& ctx, const ModInputs&) override {
        if (ctx.bpm > 0.0 && ctx.bpm != bpm_) {
            bpm_ = ctx.bpm;
            delaySmp_.setTarget(delaySamples());
        }
        constexpr float kHalfPi = std::numbers::pi_v<float> * 0.5f;
        for (int i = 0; i < n; ++i) {
            const float d = delaySmp_.next();
            const float fb = fb_.next();
            const float mix = mix_.next();
            const float dryG = std::cos(mix * kHalfPi);  // Tone CrossFade: equal power
            const float wetG = std::sin(mix * kHalfPi);
            const float wl = lineL_.readFrac(d);
            const float wr = lineR_.readFrac(d);
            lineL_.write(l[i] + wl * fb);
            lineR_.write(r[i] + wr * fb);
            l[i] = l[i] * dryG + wl * wetG;
            r[i] = r[i] * dryG + wr * wetG;
        }
    }

    void reset() override {
        lineL_.clear();
        lineR_.clear();
        delaySmp_.snap(delaySamples());
        fb_.snap(fb_.target());
        mix_.snap(mix_.target());
    }

private:
    // devices.ts delaySeconds(idx): whole note = 240/bpm seconds.
    float delaySamples() const {
        const float whole = static_cast<float>(240.0 / std::max(bpm_, 1.0));
        const float secs = std::min(whole * kFractions[timeIdx_], kMaxDelaySec);
        return std::max(secs * sr_, 1.0f);
    }

    float sr_ = 44100.0f;
    double bpm_ = 120.0;
    int timeIdx_ = 2;
    Smoother delaySmp_, fb_, mix_;
    DelayLine lineL_, lineR_;
};

}  // namespace

std::unique_ptr<EffectDevice> make_delay() { return std::make_unique<DelayFx>(); }

}  // namespace ddaw::devices
