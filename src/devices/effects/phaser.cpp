// `phaser` effect: port of sf-dsp/src/fx/phaser.rs (the Tone.Phaser composite). Per channel, a
// serial chain of 10 second-order allpass stages (Q = 10) sharing one centre frequency that a sine
// LFO sweeps linearly in Hz from 350 Hz up to 350 * 2^octaves; the right channel is in antiphase.
// The stages run on Simper's trapezoidal SVF core (allpass = v0 - 2k*v1) because a direct-form
// biquad is unstable under per-sample coefficient modulation; the (g, k) coefficients are shared by
// all stages of a channel. Wet/dry is Tone's equal-power CrossFade (the notches come from dry+wet).
// Schema: rate 0.01..300 Hz, octaves 1..6 (stepped), mix 0..1.
// Rate: rateMode 0 Sync (rateSync division at the transport bpm, read per block from ProcessContext),
// 1 Low (`rate` Hz), 2 High (`rateHi` Hz), as schema.ts rateHzOf().
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>

#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/Lfo.h"
#include "dsp/Smoother.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

enum P : uint16_t { Rate, Octaves, Mix, RateMode, RateSync, RateHi };

constexpr float kBaseFreq = 350.0f;
constexpr int kStages = 10;
constexpr float kQ = 10.0f;
constexpr float kK = 1.0f / kQ;
constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kHalfPi = kPi * 0.5f;

struct Allpass {
    float ic1 = 0.0f, ic2 = 0.0f;
    float run(float v0, float a1, float a2, float a3) noexcept {
        const float v3 = v0 - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return v0 - 2.0f * kK * v1;
    }
    void clear() noexcept { ic1 = ic2 = 0.0f; }
};

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

class PhaserFx final : public EffectDevice {
public:
    PhaserFx() {
        mix_.snap(0.5f);
        rate_.lo = 0.8f;
        applyRate();
    }

    std::span<const ParamSpec> params() const override { return schema::kFxPhaser; }

    void prepare(double sr, int) override {
        sr_ = static_cast<float>(std::max(sr, 1.0));
        mix_.prepare(sr_, 15.0f);
        mix_.snap(mix_.target());
        lfo_.prepare(sr_);
        applyRate();
        clearState();
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Rate: rate_.lo = v; applyRate(); break;
            case RateMode: rate_.mode = std::clamp(static_cast<int>(v), 0, 2); applyRate(); break;
            case RateSync: rate_.sync = std::clamp(static_cast<int>(v), 0, 8); applyRate(); break;
            case RateHi: rate_.hi = v; applyRate(); break;
            case Octaves: {
                // devices.ts truncates (p.octaves | 0); Tone's LFO max is base * 2^octaves.
                const int oct = std::clamp(static_cast<int>(v), 1, 6);
                span_ = kBaseFreq * (static_cast<float>(1u << oct) - 1.0f);
                break;
            }
            case Mix: mix_.setTarget(std::clamp(v, 0.0f, 1.0f)); break;
            default: break;
        }
    }

    void process(float* l, float* r, int n, const ProcessContext& ctx, const ModInputs&) override {
        if (rate_.setBpm(ctx.bpm)) applyRate();
        for (int i = 0; i < n; ++i) {
            const float mix = mix_.next();
            const float dryG = std::cos(mix * kHalfPi);
            const float wetG = std::sin(mix * kHalfPi);
            const float s = lfo_.next();
            const float fL = kBaseFreq + (0.5f + 0.5f * s) * span_;
            const float fR = kBaseFreq + (0.5f - 0.5f * s) * span_;
            float a1, a2, a3;
            coeffs(fL, a1, a2, a3);
            float wl = l[i];
            for (auto& st : stagesL_) wl = st.run(wl, a1, a2, a3);
            coeffs(fR, a1, a2, a3);
            float wr = r[i];
            for (auto& st : stagesR_) wr = st.run(wr, a1, a2, a3);
            l[i] = l[i] * dryG + wl * wetG;
            r[i] = r[i] * dryG + wr * wetG;
        }
    }

    void reset() override {
        mix_.snap(mix_.target());
        clearState();
    }

private:
    void applyRate() { lfo_.setFreq(rate_.hz()); }
    RateState rate_;
    // Clamp just below Nyquist to keep the prewarp finite (the browser ran at 2x the rate).
    void coeffs(float freq, float& a1, float& a2, float& a3) const noexcept {
        const float f = std::clamp(freq, 1.0f, 0.49f * sr_);
        const float g = std::tan(kPi * f / sr_);
        a1 = 1.0f / (1.0f + g * (g + kK));
        a2 = g * a1;
        a3 = g * a2;
    }
    void clearState() {
        for (auto& s : stagesL_) s.clear();
        for (auto& s : stagesR_) s.clear();
        lfo_.setPhase(0.0);
    }

    float sr_ = 44100.0f;
    float span_ = kBaseFreq * (8.0f - 1.0f);   // default octaves 3
    Lfo lfo_;
    Smoother mix_;
    std::array<Allpass, kStages> stagesL_, stagesR_;
};

}  // namespace

std::unique_ptr<EffectDevice> make_phaser() { return std::make_unique<PhaserFx>(); }

}  // namespace ddaw::devices
