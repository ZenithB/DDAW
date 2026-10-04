// `dist` effect: port of sf-dsp/src/fx/dist.rs (Tone.Distortion). 4x-oversampled waveshaper
// f(x) = ((3 + k) * x * 20deg) / (pi + k * |x|), k = amt * 100, input clamped to [-1, 1], then an
// equal-power (Tone.CrossFade) wet/dry blend done inside the oversampled block so wet and dry share
// the resampler latency. Schema: amt 0..1 (def 0.4), mix 0..1 (def 1).
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

enum P : uint16_t { Amt, Mix };

constexpr float kOsFactor = 4.0f;
constexpr float kHalfPi = std::numbers::pi_v<float> / 2.0f;
constexpr float kPi = std::numbers::pi_v<float>;

// Tone.Distortion transfer curve. 20 * pi/180 = pi/9.
inline float shape(float x, float k) noexcept {
    constexpr float kGain = kPi / 9.0f;
    const float xc = std::clamp(x, -1.0f, 1.0f);
    return ((3.0f + k) * xc * kGain) / (kPi + k * std::abs(xc));
}

class DistFx final : public EffectDevice {
public:
    DistFx() {
        drive_.snap(0.4f);
        mix_.snap(1.0f);
    }

    std::span<const ParamSpec> params() const override { return schema::kFxDist; }

    void prepare(double sr, int) override {
        // Both smoothers are stepped in the 4x domain so the 15 ms time constant holds there.
        const double sr4 = std::max(sr, 1.0) * kOsFactor;
        drive_.prepare(sr4, 15.0f);
        mix_.prepare(sr4, 15.0f);
        os_.reset();
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Amt: drive_.setTarget(std::clamp(v, 0.0f, 1.0f)); break;
            case Mix: mix_.setTarget(std::clamp(v, 0.0f, 1.0f)); break;
            default: break;
        }
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        for (int off = 0; off < n; off += kMaxBlock) {
            const int len = std::min(kMaxBlock, n - off);
            os_.process(l + off, r + off, len, [this](float* hl, float* hr, int n4) {
                for (int i = 0; i < n4; ++i) {
                    const float k = drive_.next() * 100.0f;
                    const float mix = mix_.next();
                    // Tone.CrossFade equal-power law.
                    const float wetG = std::sin(mix * kHalfPi);
                    const float dryG = std::cos(mix * kHalfPi);
                    hl[i] = wetG * shape(hl[i], k) + dryG * hl[i];
                    hr[i] = wetG * shape(hr[i], k) + dryG * hr[i];
                }
            });
        }
    }

    void reset() override {
        os_.reset();
        drive_.snap(drive_.target());
        mix_.snap(mix_.target());
    }

private:
    dsp::Oversampler4 os_;
    dsp::Smoother drive_, mix_;
};

}  // namespace

std::unique_ptr<EffectDevice> make_dist() { return std::make_unique<DistFx>(); }

}  // namespace ddaw::devices
