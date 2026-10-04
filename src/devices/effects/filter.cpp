// `filter` effect: port of sf-dsp/src/fx/filter.rs, the browser Tone.Filter device: an LP/HP/BP filter
// whose `slope` picks a -12/-24/-48 dB/oct rolloff by cascading 1/2/4 identical two-pole sections that
// share frequency and Q. The raw schema `q` (0..12) is a dB resonance for LP/HP (Web Audio) and a
// dimensionless Q for BP. Fully wet.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/Smoother.h"
#include "dsp/Svf.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

enum P : uint16_t { Ftype, Freq, Q, Slope };

constexpr int kMaxStages = 4;  // slope index 2 = 48 dB/oct
constexpr float kFreqMin = 40.0f, kFreqMax = 18000.0f, kQMin = 0.0f, kQMax = 12.0f;

int stagesForSlope(int slope) { return slope <= 0 ? 1 : (slope == 1 ? 2 : 4); }
SvfMode modeForFtype(int t) { return t == 1 ? SvfMode::Highpass : (t == 2 ? SvfMode::Bandpass : SvfMode::Lowpass); }

class FilterFx final : public EffectDevice {
public:
    FilterFx() {
        freq_.snap(2000.0f);  // schema defaults
        q_.snap(1.0f);
        applyCoeffs(freq_.current(), q_.current());
    }

    std::span<const ParamSpec> params() const override { return schema::kFxFilter; }

    void prepare(double sr, int) override {
        freq_.prepare(sr, 15.0f);
        q_.prepare(sr, 15.0f);
        for (auto& ch : stages_) for (auto& s : ch) s.prepare(static_cast<float>(std::max(sr, 1.0)));
        applyCoeffs(freq_.current(), q_.current());
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Ftype: {
                const SvfMode m = modeForFtype(static_cast<int>(v));
                if (m != mode_) {  // Tone switches node.type in place: filter state carries over
                    mode_ = m;
                    applyCoeffs(appliedFreq_, q_.current());
                }
                break;
            }
            case Freq: freq_.setTarget(std::clamp(v, kFreqMin, kFreqMax)); break;
            case Q: q_.setTarget(std::clamp(v, kQMin, kQMax)); break;
            case Slope: {
                const int n = stagesForSlope(static_cast<int>(v));
                if (n > nStages_)  // clear stale state in the sections being switched in
                    for (auto& ch : stages_) for (int s = nStages_; s < n; ++s) ch[static_cast<size_t>(s)].reset();
                nStages_ = n;
                break;
            }
            default: break;
        }
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        for (int i = 0; i < n; ++i) {
            const float f = freq_.next(), q = q_.next();
            // refresh coefficients only while the smoothers are still moving
            if (std::abs(f - appliedFreq_) > 1e-3f || std::abs(q - appliedQ_) > 1e-5f) applyCoeffs(f, q);
            float sl = l[i], sr = r[i];
            for (int s = 0; s < nStages_; ++s) {
                sl = stages_[0][static_cast<size_t>(s)].processSample(sl) * stageGain_;
                sr = stages_[1][static_cast<size_t>(s)].processSample(sr) * stageGain_;
            }
            l[i] = sl;
            r[i] = sr;
        }
    }

    void reset() override {
        for (auto& ch : stages_) for (auto& s : ch) s.reset();
        freq_.snap(freq_.target());
        q_.snap(q_.target());
        applyCoeffs(freq_.current(), q_.current());
    }

private:
    // Web Audio: Q in dB for LP/HP, dimensionless for BP.
    static float linearQ(float qParam, SvfMode m) {
        return m == SvfMode::Bandpass ? std::max(qParam, 0.025f) : std::pow(10.0f, qParam * (1.0f / 20.0f));
    }

    void applyCoeffs(float freq, float qParam) {
        const float q = linearQ(qParam, mode_);
        // Svf's bandpass tap has centre gain Q; Web Audio's biquad is unity at centre, so scale by 1/Q.
        stageGain_ = mode_ == SvfMode::Bandpass ? 1.0f / q : 1.0f;
        for (auto& ch : stages_)
            for (auto& s : ch) { s.setMode(mode_); s.setCutoffQ(freq, q); }
        appliedFreq_ = freq;
        appliedQ_ = qParam;
    }

    static std::array<Svf, kMaxStages> makeChannel() { return {Svf(SvfMode::Lowpass), Svf(SvfMode::Lowpass), Svf(SvfMode::Lowpass), Svf(SvfMode::Lowpass)}; }
    std::array<std::array<Svf, kMaxStages>, 2> stages_{makeChannel(), makeChannel()};  // [channel][stage], identical coefficients
    int nStages_ = 1;                                       // schema default slope 0 (12 dB)
    SvfMode mode_ = SvfMode::Lowpass;
    Smoother freq_, q_;
    float appliedFreq_ = 2000.0f, appliedQ_ = 1.0f, stageGain_ = 1.0f;
};

}  // namespace

std::unique_ptr<EffectDevice> make_filter() { return std::make_unique<FilterFx>(); }

}  // namespace ddaw::devices
