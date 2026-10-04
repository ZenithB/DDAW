// `widen` effect: port of sf-dsp/src/fx/widen.rs (Tone.StereoWidener). Mid/side width control:
// mid *= 2*(1-width), side *= 2*width on a sqrt(1/2)-normalised split/merge, which folds into a
// single x0.5 per branch. width 0.5 is unity, 0 is mono, 1 is side only. Schema: width 0..1 (def 0.8).
#include <algorithm>
#include <memory>

#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/Smoother.h"

namespace ddaw::devices {
namespace {

enum P : uint16_t { Width };

class WidenFx final : public EffectDevice {
public:
    WidenFx() { width_.snap(0.8f); }

    std::span<const ParamSpec> params() const override { return schema::kFxWiden; }

    void prepare(double sr, int) override {
        width_.prepare(std::max(sr, 1.0), 15.0f);
        width_.snap(width_.target());
    }

    void setParam(uint16_t index, float v) override {
        if (index == Width) width_.setTarget(std::clamp(v, 0.0f, 1.0f));
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        for (int i = 0; i < n; ++i) {
            const float w = width_.next();
            const float mid = (l[i] + r[i]) * (1.0f - w);  // 0.5 * 2*(1-w)
            const float side = (l[i] - r[i]) * w;          // 0.5 * 2*w
            l[i] = mid + side;
            r[i] = mid - side;
        }
    }

    void reset() override { width_.snap(width_.target()); }

private:
    dsp::Smoother width_;
};

}  // namespace

std::unique_ptr<EffectDevice> make_widen() { return std::make_unique<WidenFx>(); }

}  // namespace ddaw::devices
