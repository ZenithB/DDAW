// `cheby` effect: port of sf-dsp/src/fx/cheby.rs (Tone.Chebyshev). 4x-oversampled polynomial
// waveshaper from Tone's recurrence with T0 := 0 (C1 = x, Cn = 2x*C(n-1) - C(n-2)), input clamped to
// [-1, 1], plus an equal-power wet/dry blend inside the oversampled block. Schema: order 2..14
// (stepped, def 3; devices.ts max(1, v|0)), mix 0..1 (def 0.35).
#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>

#include "core/Constants.h"
#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/Oversampler4.h"
#include "dsp/Smoother.h"

namespace ddaw::devices {
namespace {

enum P : uint16_t { Order, Mix };

constexpr float kOsFactor = 4.0f;
constexpr float kHalfPi = std::numbers::pi_v<float> / 2.0f;
constexpr int kMaxOrder = 14;  // schema max; also caps the recurrence loop

inline float shape(float x, int order) noexcept {
    const float xc = std::clamp(x, -1.0f, 1.0f);  // WaveShaperNode curve domain
    if (order <= 1) return xc;
    float prev2 = 0.0f, prev1 = xc;
    for (int i = 2; i <= order; ++i) {
        const float cur = 2.0f * xc * prev1 - prev2;
        prev2 = prev1;
        prev1 = cur;
    }
    return prev1;
}

class ChebyFx final : public EffectDevice {
public:
    ChebyFx() { mix_.snap(0.35f); }

    std::span<const ParamSpec> params() const override { return schema::kFxCheby; }

    void prepare(double sr, int) override {
        mix_.prepare(std::max(sr, 1.0) * kOsFactor, 15.0f);
        os_.reset();
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            // `v | 0` truncates toward zero; floor 1, cap at the schema max.
            case Order: order_ = std::clamp(static_cast<int>(v), 1, kMaxOrder); break;
            case Mix: mix_.setTarget(std::clamp(v, 0.0f, 1.0f)); break;
            default: break;
        }
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        const int order = order_;
        for (int off = 0; off < n; off += kMaxBlock) {
            const int len = std::min(kMaxBlock, n - off);
            os_.process(l + off, r + off, len, [this, order](float* hl, float* hr, int n4) {
                for (int i = 0; i < n4; ++i) {
                    const float mix = mix_.next();
                    const float wetG = std::sin(mix * kHalfPi);
                    const float dryG = std::cos(mix * kHalfPi);
                    hl[i] = wetG * shape(hl[i], order) + dryG * hl[i];
                    hr[i] = wetG * shape(hr[i], order) + dryG * hr[i];
                }
            });
        }
    }

    void reset() override {
        os_.reset();
        mix_.snap(mix_.target());
    }

private:
    dsp::Oversampler4 os_;
    int order_ = 3;
    dsp::Smoother mix_;
};

}  // namespace

std::unique_ptr<EffectDevice> make_cheby() { return std::make_unique<ChebyFx>(); }

}  // namespace ddaw::devices
