// `comp` effect: port of sf-dsp/src/fx/comp.rs, the Tone.Compressor insert (a native
// DynamicsCompressorNode). The behaviour lives in dsp::CompKernel, shared with the master limiter.
// Schema: thresh -60..0 dB, ratio 1..20, attack 0.001..0.3 s, release 0.02..1 s. Fully wet.
#include <memory>

#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/CompKernel.h"

namespace ddaw::devices {
namespace {

enum P : uint16_t { Thresh, Ratio, Attack, Release };

class CompFx final : public EffectDevice {
public:
    std::span<const ParamSpec> params() const override { return schema::kFxComp; }
    void prepare(double sr, int) override { k_.prepare(sr); }
    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Thresh: k_.setThresh(v); break;
            case Ratio: k_.setRatio(v); break;
            case Attack: k_.setAttack(v); break;
            case Release: k_.setRelease(v); break;
            default: break;
        }
    }
    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs& mod) override { k_.process(l, r, n, mod.keyL, mod.keyR); }
    bool keyable() const noexcept override { return true; }
    void reset() override { k_.reset(); }
    int latencySamples() const override { return k_.latencySamples(); }  // the node's 6 ms lookahead
    float gainReductionDb() const override { return k_.grDb(); }

private:
    dsp::CompKernel k_;
};

}  // namespace

std::unique_ptr<EffectDevice> make_comp() { return std::make_unique<CompFx>(); }

}  // namespace ddaw::devices
