// `duck` effect: port of sf-dsp/src/fx/duck.rs, the sidechain ducker (devices.ts makeEffect("duck")).
// A single gain pumped by an envelope, in two modes:
//   tempo (default): the gain dips at every division boundary and recovers over the cycle; the phase
//     comes from the transport tick position (ProcessContext::positionTicks, advanced per sample).
//   trigger: a source track's note calls trigger(); the dip starts there and recovers over one Rate
//     cycle. Recovery shape rec = phase^(0.35 + curve*1.6), g = (1 - amount) + amount * rec, written
//     through a 4 ms one-pole (setTargetAtTime(g, now, 0.004)).
// Fully wet: no mix param, the device IS the gain.
//
// Sidechain: when the project gives the duck a `srcTrack`, the graph calls setSidechain(true) (the
// "srcOn" flag in Rust) and the engine calls trigger() whenever a note fires on that track (optionally
// only for one drum pad). Without a source the device stays in tempo mode. The two non-static hooks at
// the bottom of this file are test-only access to the same methods.
//
// Rust parity note: the golden fx-duck.wav contains no ducking at all (Tone's Offline() restores the
// online context before rendering, so setTargetAtTime lands far in the future). The port follows the
// browser's live tick path, as the Rust device does, so the fixture is not a depth reference.
#include <algorithm>
#include <cmath>
#include <memory>

#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/Smoother.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

enum P : uint16_t { Rate, Amount, Curve };

// schema.ts DUCK_DIV_TICKS (PPQ 96): 1/2, 1/4, 1/4T, 1/8, 1/8T, 1/16.
constexpr double kDivTicks[6] = {192.0, 96.0, 64.0, 48.0, 32.0, 24.0};
constexpr float kGainTcMs = 4.0f;

class DuckFx final : public EffectDevice {
public:
    DuckFx() {
        amount_.prepare(sr_, 15.0f); amount_.snap(0.7f);   // schema defaults: rate 1, amount 0.7, curve 0.5
        curve_.prepare(sr_, 15.0f);  curve_.snap(0.5f);
        gain_.prepare(sr_, kGainTcMs); gain_.snap(1.0f);
    }

    std::span<const ParamSpec> params() const override { return schema::kFxDuck; }

    void prepare(double sr, int) override {
        sr_ = static_cast<float>(std::max(sr, 1.0));
        amount_.prepare(sr_, 15.0f); amount_.snap(amount_.target());
        curve_.prepare(sr_, 15.0f);  curve_.snap(curve_.target());
        gain_.prepare(sr_, kGainTcMs); gain_.snap(1.0f);
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Rate: rateIdx_ = static_cast<size_t>(std::clamp(static_cast<int>(v), 0, 5)); break;
            case Amount: amount_.setTarget(std::clamp(v, 0.0f, 1.0f)); break;
            case Curve: curve_.setTarget(std::clamp(v, 0.1f, 1.0f)); break;
            default: break;
        }
    }

    // Source-note hook: restarts the dip. Only meaningful in trigger mode.
    void trigger() noexcept override { trigActive_ = true; trigElapsed_ = 0.0; }
    void setSidechain(bool on) noexcept override { srcOn_ = on; }
    void setTriggerMode(bool on) noexcept { srcOn_ = on; }

    void process(float* l, float* r, int n, const ProcessContext& ctx, const ModInputs&) override {
        posTicks_ = ctx.positionTicks;
        playing_ = ctx.playing;
        if (ctx.bpm > 0.0) bpm_ = ctx.bpm;

        const double ticksPerSample = std::max(bpm_, 1.0) * 96.0 / (60.0 * static_cast<double>(sr_));
        const double cyc = kDivTicks[rateIdx_];
        const double cycSamples = cyc / ticksPerSample;  // recovery window in samples (trigger mode)
        for (int i = 0; i < n; ++i) {
            const float amount = amount_.next();
            const float curve = curve_.next();
            float target;
            if (srcOn_) {
                if (trigActive_) {
                    const float phase = static_cast<float>(std::min(trigElapsed_ / cycSamples, 1.0));
                    trigElapsed_ += 1.0;
                    target = gainOf(phase, amount, curve);
                } else {
                    target = 1.0f;
                }
            } else {
                double m = std::fmod(posTicks_, cyc);
                if (m < 0.0) m += cyc;  // rem_euclid
                const float phase = static_cast<float>(m / cyc);
                if (playing_) posTicks_ += ticksPerSample;
                target = gainOf(phase, amount, curve);
            }
            gain_.setTarget(target);
            const float g = gain_.next();
            l[i] *= g;
            r[i] *= g;
        }
    }

    void reset() override {
        posTicks_ = 0.0;
        playing_ = false;
        trigActive_ = false;
        trigElapsed_ = 0.0;
        amount_.snap(amount_.target());
        curve_.snap(curve_.target());
        gain_.snap(1.0f);
    }

private:
    // rec = phase^(0.35 + curve*1.6); g = (1 - amount) + amount * rec.
    static float gainOf(float phase, float amount, float curve) noexcept {
        const float rec = std::pow(std::clamp(phase, 0.0f, 1.0f), 0.35f + curve * 1.6f);
        return std::max((1.0f - amount) + amount * rec, 0.0f);
    }

    float sr_ = 44100.0f;
    double bpm_ = 120.0;
    bool playing_ = false;
    double posTicks_ = 0.0;
    size_t rateIdx_ = 1;
    bool srcOn_ = false;
    bool trigActive_ = false;
    double trigElapsed_ = 0.0;
    Smoother amount_, curve_, gain_;
};

}  // namespace

std::unique_ptr<EffectDevice> make_duck() { return std::make_unique<DuckFx>(); }

// Test-only access to the trigger path (see the TODO(A3) above). Non-static so the unit test can
// declare and call them; they do nothing on a device that is not a DuckFx.
void duck_test_trigger(EffectDevice& d) {
    if (auto* p = dynamic_cast<DuckFx*>(&d)) p->trigger();
}
void duck_test_set_trigger_mode(EffectDevice& d, bool on) {
    if (auto* p = dynamic_cast<DuckFx*>(&d)) p->setTriggerMode(on);
}

}  // namespace ddaw::devices
