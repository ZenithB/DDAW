// `eq` effect: port of sf-dsp/src/fx/eq.rs, the Tone.EQ3 composite: a 3-band split (MultibandSplit at
// 400 Hz / 2500 Hz, 12 dB/oct filters, Q = 1 dB) into per-band gains, summed. Schema: low/mid/high band
// gains in dB, -12..+12. The crossovers are Tone.EQ3 defaults and not exposed. Fully wet.
#include <array>
#include <memory>

#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/Math.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

enum P : uint16_t { Low, Mid, High };

constexpr float kLowXoverHz = 400.0f;
constexpr float kHighXoverHz = 2500.0f;
// MultibandSplit default Q is 1; Web Audio LP/HP Q is in dB, so the filter Q is 10^(1/20).
constexpr float kXoverQ = 1.1220185f;

// One channel of the band split: low = LP(400); mid = HP(400) -> LP(2500); high = HP(2500).
struct SplitChannel {
    Svf lowLp{SvfMode::Lowpass}, midHp{SvfMode::Highpass}, midLp{SvfMode::Lowpass}, highHp{SvfMode::Highpass};

    void prepare(float sr) {
        lowLp.prepare(sr); midHp.prepare(sr); midLp.prepare(sr); highHp.prepare(sr);
        lowLp.setCutoffQ(kLowXoverHz, kXoverQ);
        midHp.setCutoffQ(kLowXoverHz, kXoverQ);
        midLp.setCutoffQ(kHighXoverHz, kXoverQ);
        highHp.setCutoffQ(kHighXoverHz, kXoverQ);
    }
    void reset() { lowLp.reset(); midHp.reset(); midLp.reset(); highHp.reset(); }

    float processSample(float x, float gLow, float gMid, float gHigh) {
        const float low = lowLp.processSample(x);
        const float mid = midLp.processSample(midHp.processSample(x));
        const float high = highHp.processSample(x);
        return low * gLow + mid * gMid + high * gHigh;
    }
};

class EqFx final : public EffectDevice {
public:
    EqFx() {
        for (auto* s : {&low_, &mid_, &high_}) s->snap(1.0f);
    }

    std::span<const ParamSpec> params() const override { return schema::kFxEq; }

    void prepare(double sr, int) override {
        const float fs = static_cast<float>(std::max(sr, 1.0));
        left_.prepare(fs);
        right_.prepare(fs);
        for (auto* s : {&low_, &mid_, &high_}) {
            s->prepare(sr, 15.0f);
            s->snap(s->target());
        }
    }

    // Tone.EQ3 applies the raw dB value with no clamp; the schema range is enforced upstream.
    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Low: low_.setTarget(dbToLin(v)); break;
            case Mid: mid_.setTarget(dbToLin(v)); break;
            case High: high_.setTarget(dbToLin(v)); break;
            default: break;
        }
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        for (int i = 0; i < n; ++i) {
            const float gl = low_.next(), gm = mid_.next(), gh = high_.next();
            l[i] = left_.processSample(l[i], gl, gm, gh);
            r[i] = right_.processSample(r[i], gl, gm, gh);
        }
    }

    void reset() override {
        left_.reset();
        right_.reset();
        for (auto* s : {&low_, &mid_, &high_}) s->snap(s->target());
    }

private:
    SplitChannel left_, right_;
    Smoother low_, mid_, high_;  // smoothed linear band gains (dB -> lin at the block boundary)
};

}  // namespace

std::unique_ptr<EffectDevice> make_eq() { return std::make_unique<EqFx>(); }

}  // namespace ddaw::devices
