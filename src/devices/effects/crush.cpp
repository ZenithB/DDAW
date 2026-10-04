// `crush` effect: port of sf-dsp/src/fx/crush.rs (Tone.BitCrusher). Quantises every sample to
// step = 0.5^(bits-1) with round-half-up, 4x oversampled, then an equal-power wet/dry blend. The dry
// path is delayed by the oversampler's measured round-trip latency so partial mixes do not comb.
// Schema: bits 1..16 (stepped, def 8), mix 0..1 (def 1). No sample-rate reduction (as in the schema).
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

#include "core/Constants.h"
#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/DelayLine.h"
#include "dsp/Math.h"
#include "dsp/Oversampler4.h"
#include "dsp/Smoother.h"

namespace ddaw::devices {
namespace {

enum P : uint16_t { Bits, Mix };

inline float quantStep(int bits) noexcept { return std::pow(0.5f, static_cast<float>(bits - 1)); }

class CrushFx final : public EffectDevice {
public:
    CrushFx() {
        mix_.snap(1.0f);
        dryDelayL_.prepare(64);
        dryDelayR_.prepare(64);
        prepare(44100.0, kMaxBlock);
    }

    std::span<const ParamSpec> params() const override { return schema::kFxCrush; }

    void prepare(double sr, int) override {
        mix_.prepare(std::max(sr, 1.0), 15.0f);
        mix_.snap(mix_.target());
        // Measure the oversampler's identity latency once so the dry path can be phase-aligned.
        os_.reset();
        float best = 0.0f;
        int lat = 1;
        for (int blk = 0; blk < 2; ++blk) {
            std::array<float, kMaxBlock> l{}, r{};
            if (blk == 0) { l[0] = 1.0f; r[0] = 1.0f; }
            os_.process(l.data(), r.data(), kMaxBlock, [](float*, float*, int) {});
            for (int i = 0; i < kMaxBlock; ++i) {
                if (std::abs(l[size_t(i)]) > best) { best = std::abs(l[size_t(i)]); lat = blk * kMaxBlock + i; }
            }
        }
        latency_ = std::clamp(lat, 1, dryDelayL_.maxDelay() - 1);
        os_.reset();
        dryDelayL_.clear();
        dryDelayR_.clear();
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            // devices.ts: max(1, v|0); the worklet clamps at 16. Stepped, no smoothing.
            case Bits: step_ = quantStep(std::clamp(static_cast<int>(v), 1, 16)); break;
            case Mix: mix_.setTarget(std::clamp(v, 0.0f, 1.0f)); break;
            default: break;
        }
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        for (int off = 0; off < n; off += kMaxBlock) processBlock(l + off, r + off, std::min(kMaxBlock, n - off));
    }

    void reset() override {
        os_.reset();
        dryDelayL_.clear();
        dryDelayR_.clear();
        mix_.snap(mix_.target());
    }

private:
    void processBlock(float* l, float* r, int n) {
        for (int i = 0; i < n; ++i) {
            dryDelayL_.write(l[i]);
            dryDelayR_.write(r[i]);
            // read(1) is the just-written sample, so a `latency`-sample delay is read(latency + 1).
            dryL_[size_t(i)] = dryDelayL_.read(latency_ + 1);
            dryR_[size_t(i)] = dryDelayR_.read(latency_ + 1);
        }

        const float step = step_;
        const float invStep = 1.0f / step;
        os_.process(l, r, n, [step, invStep](float* hl, float* hr, int n4) {
            for (int i = 0; i < n4; ++i) hl[i] = step * std::floor(hl[i] * invStep + 0.5f);
            for (int i = 0; i < n4; ++i) hr[i] = step * std::floor(hr[i] * invStep + 0.5f);
        });

        for (int i = 0; i < n; ++i) {
            const float mix = mix_.next();
            const auto [dryG, wetG] = dsp::panGains(mix * 2.0f - 1.0f);  // Tone CrossFade
            l[i] = dryL_[size_t(i)] * dryG + l[i] * wetG;
            r[i] = dryR_[size_t(i)] * dryG + r[i] * wetG;
        }
    }

    float step_ = quantStep(8);  // schema defaults: bits 8, mix 1
    dsp::Smoother mix_;
    dsp::Oversampler4 os_;
    dsp::DelayLine dryDelayL_, dryDelayR_;
    int latency_ = 1;  // oversampler round-trip delay, base-rate samples
    std::array<float, kMaxBlock> dryL_{}, dryR_{};
};

}  // namespace

std::unique_ptr<EffectDevice> make_crush() { return std::make_unique<CrushFx>(); }

}  // namespace ddaw::devices
