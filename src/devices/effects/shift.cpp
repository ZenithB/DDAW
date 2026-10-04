// `shift` effect: port of sf-dsp/src/fx/shift.rs, the Tone.FrequencyShifter insert.
// Single-sideband frequency shifter (every partial moves by the same +-Hz, inharmonic): the input
// goes through Tone's PhaseShiftAllpass (Niemitalo IIR Hilbert pair: two 4-section allpass
// cascades, the in-phase one followed by a one-sample delay), is ring-modulated by a sin/cos
// oscillator at the shift frequency and recombined so one sideband cancels. Wet/dry is Tone's
// CrossFade (equal-power cos/sin). Schema: amt -400..400 Hz, mix 0..1, both ~15 ms smoothed.
// No lookahead: the allpass group delay is part of the algorithm, latencySamples() stays 0.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>

#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/Smoother.h"

namespace ddaw::devices {
namespace {

enum P : uint16_t { Amt, Mix };

// Tone PhaseShiftAllpass bank 1 (in-phase reference) and bank 2 (leads it by ~90 degrees).
constexpr std::array<double, 4> kBankI = {0.6923878, 0.9360654322959, 0.988229522686, 0.9987488452737};
constexpr std::array<double, 4> kBankQ = {0.4021921162426, 0.856171088242, 0.9722909545651, 0.9952884791278};

// H(z) = (a - z^-2) / (1 - a z^-2), a = c^2 (the WebAudio IIRFilter Tone builds per coefficient).
struct Allpass2 {
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    double run(double a, double x) noexcept {
        const double y = a * (x + y2) - x2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

struct Hilbert {
    std::array<Allpass2, 4> iBank{}, qBank{};
    double iZ1 = 0;
    // Returns (i, q); q leads i by ~90 degrees.
    void run(double x, double& i, double& q) noexcept {
        double a = x;
        for (size_t k = 0; k < 4; ++k) a = iBank[k].run(kBankI[k] * kBankI[k], a);
        double b = x;
        for (size_t k = 0; k < 4; ++k) b = qBank[k].run(kBankQ[k] * kBankQ[k], b);
        const double delayed = iZ1;
        iZ1 = a;
        i = delayed;
        q = b;
    }
    void clear() noexcept { *this = Hilbert{}; }
};

class ShiftFx final : public EffectDevice {
public:
    ShiftFx() {
        amt_.snap(60.0f);
        mix_.snap(0.4f);
    }

    std::span<const ParamSpec> params() const override { return schema::kFxShift; }

    void prepare(double sr, int) override {
        sr_ = static_cast<float>(std::max(sr, 1.0));
        amt_.prepare(sr_, 15.0f);
        mix_.prepare(sr_, 15.0f);
        reset();
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Amt: amt_.setTarget(std::clamp(v, -400.0f, 400.0f)); break;
            case Mix: mix_.setTarget(std::clamp(v, 0.0f, 1.0f)); break;
            default: break;
        }
        if (fresh_) { amt_.snap(amt_.target()); mix_.snap(mix_.target()); }  // pre-audio: no glide from defaults
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        fresh_ = false;
        const double invSr = 1.0 / static_cast<double>(sr_);
        constexpr float kHalfPi = std::numbers::pi_v<float> / 2.0f;
        for (int k = 0; k < n; ++k) {
            const float f = amt_.next();
            const float m = mix_.next();
            const float dryG = std::cos(m * kHalfPi);
            const float wetG = std::sin(m * kHalfPi);
            const double th = phase_ * (2.0 * std::numbers::pi);
            const double sinTh = std::sin(th), cosTh = std::cos(th);
            phase_ += static_cast<double>(f) * invSr;  // negative shift runs the oscillator backwards
            phase_ -= std::floor(phase_);
            double il, ql, ir, qr;
            left_.run(static_cast<double>(l[k]), il, ql);
            right_.run(static_cast<double>(r[k]), ir, qr);
            const float wetL = static_cast<float>(il * cosTh + ql * sinTh);
            const float wetR = static_cast<float>(ir * cosTh + qr * sinTh);
            l[k] = l[k] * dryG + wetL * wetG;
            r[k] = r[k] * dryG + wetR * wetG;
        }
    }

    void reset() override {
        left_.clear();
        right_.clear();
        phase_ = 0.0;
        amt_.snap(amt_.target());
        mix_.snap(mix_.target());
        fresh_ = true;
    }

private:
    float sr_ = 44100.0f;
    dsp::Smoother amt_, mix_;
    double phase_ = 0.0;
    Hilbert left_, right_;
    bool fresh_ = true;
};

}  // namespace

std::unique_ptr<EffectDevice> make_shift() { return std::make_unique<ShiftFx>(); }

}  // namespace ddaw::devices
