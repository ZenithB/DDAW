// `gate` effect: port of sf-dsp/src/fx/gate.rs (the sf-gate AudioWorklet).
// key select -> key filter (HP->LP) -> detector (peak|RMS) -> dB -> hysteresis state machine (open at
// thresh+up, close at thresh+dn) -> expander curve (ratio, soft knee, clamped by range) -> attack/hold/
// release ramp (linear in dB) -> applied to the lookahead-delayed audio -> mix -> output gain.
// Like the worklet, parameters are raw (not smoothed) and the derived values are recomputed at the next
// block boundary after a change.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>

#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/DelayLine.h"
#include "dsp/Math.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

// Index in schema::kFxGate.
enum P : uint16_t { Thresh, Up, Dn, Range, Ratio, Knee, Attack, Hold, Release, Look, Hpf, Lpf, Key, Det, Listen, Inverse, Mix, Gain };

constexpr float kDbFloor = -160.0f;
constexpr float kRmsWinSec = 0.01f;
constexpr float kPeakRelSec = 0.005f;
constexpr float kMaxLookMs = 10.0f;
constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kInvSqrt2 = 0.70710678118654752f;

inline float poleOf(float sec, float sr) { return sec > 0.0f ? std::exp(-1.0f / (sec * sr)) : 0.0f; }

struct Biquad {
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    std::array<float, 5> c{1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    void resetState() { x1 = x2 = y1 = y2 = 0.0f; }
    float proc(float x) noexcept {
        const float y = c[0] * x + c[1] * x1 + c[2] * x2 - c[3] * y1 - c[4] * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

std::array<float, 5> lpCoeffs(float f, float sr) {
    const float w0 = 2.0f * kPi * std::min(f, sr * 0.49f) / sr;
    const float cw = std::cos(w0);
    const float alpha = std::sin(w0) / (2.0f * kInvSqrt2);
    const float a0 = 1.0f + alpha;
    return {((1.0f - cw) * 0.5f) / a0, (1.0f - cw) / a0, ((1.0f - cw) * 0.5f) / a0, (-2.0f * cw) / a0, (1.0f - alpha) / a0};
}
std::array<float, 5> hpCoeffs(float f, float sr) {
    const float w0 = 2.0f * kPi * std::min(f, sr * 0.49f) / sr;
    const float cw = std::cos(w0);
    const float alpha = std::sin(w0) / (2.0f * kInvSqrt2);
    const float a0 = 1.0f + alpha;
    return {((1.0f + cw) * 0.5f) / a0, (-(1.0f + cw)) / a0, ((1.0f + cw) * 0.5f) / a0, (-2.0f * cw) / a0, (1.0f - alpha) / a0};
}

struct Params {
    float thresh = -40.0f, up = 0.0f, dn = -6.0f, range = -60.0f, ratio = 20.0f, knee = 3.0f;
    float attack = 0.002f, hold = 0.05f, release = 0.15f, look = 0.0f, hpf = 20.0f, lpf = 20000.0f;
    float key = 0.0f, det = 0.0f, listen = 0.0f, inverse = 0.0f, mix = 1.0f, gain = 0.0f;
};

class GateFx final : public EffectDevice {
public:
    GateFx() {
        allocLines();
        recompute();
    }

    std::span<const ParamSpec> params() const override { return schema::kFxGate; }

    void prepare(double sr, int) override {
        sr_ = static_cast<float>(std::max(sr, 1.0));
        allocLines();
        recompute();
        reset();
    }

    void setParam(uint16_t index, float v) override {
        if (!std::isfinite(v)) return;
        switch (index) {
            case Thresh: p_.thresh = v; break;
            case Up: p_.up = v; break;
            case Dn: p_.dn = v; break;
            case Range: p_.range = v; break;
            case Ratio: p_.ratio = v; break;
            case Knee: p_.knee = v; break;
            case Attack: p_.attack = v; break;
            case Hold: p_.hold = v; break;
            case Release: p_.release = v; break;
            case Look: p_.look = v; break;
            case Hpf: p_.hpf = v; break;
            case Lpf: p_.lpf = v; break;
            case Key: p_.key = v; break;
            case Det: p_.det = v; break;
            case Listen: p_.listen = v; break;
            case Inverse: p_.inverse = v; break;
            case Mix: p_.mix = v; break;
            case Gain: p_.gain = v; break;
            default: return;
        }
        dirty_ = true;
    }

    bool keyable() const noexcept override { return true; }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs& mod) override {
        if (dirty_) recompute();
        const Params p = p_;
        const int mode = static_cast<int>(p.key);
        const bool rms = p.det >= 0.5f;
        const bool listen = p.listen >= 0.5f;
        const bool stereoKey = mode == 0;
        const float openAt = p.thresh + p.up;
        const float closeAt = p.thresh + p.dn;
        const float dry = 1.0f - mixWet_;
        const float sg = sign_;

        for (int i = 0; i < n; ++i) {
            const float xl = l[i], xr = r[i];
            // the detector's input: the sidechain key when there is one (a mono key on both sides), else the signal itself
            const float cl = mod.keyL ? mod.keyL[i] : xl, cr = mod.keyL ? (mod.keyR ? mod.keyR[i] : mod.keyL[i]) : xr;

            float k0, k1 = 0.0f;
            switch (mode) {
                case 0: k0 = cl; k1 = cr; break;
                case 1: k0 = cl; break;
                case 2: k0 = cr; break;
                case 3: k0 = (cl + cr) * 0.5f; break;
                default: k0 = (cl - cr) * 0.5f; break;
            }

            k0 = lp_[0].proc(hp_[0].proc(k0));
            if (stereoKey) k1 = lp_[1].proc(hp_[1].proc(k1));

            float lvl;
            if (rms) {
                msEnv_[0] += (k0 * k0 - msEnv_[0]) * (1.0f - rmsPole_);
                float ms = msEnv_[0];
                if (stereoKey) {
                    msEnv_[1] += (k1 * k1 - msEnv_[1]) * (1.0f - rmsPole_);
                    if (msEnv_[1] > ms) ms = msEnv_[1];
                }
                lvl = std::sqrt(std::max(ms, 0.0f));
            } else {
                const float a0 = std::abs(k0);
                peakEnv_[0] = a0 > peakEnv_[0] ? a0 : a0 + (peakEnv_[0] - a0) * peakRel_;
                lvl = peakEnv_[0];
                if (stereoKey) {
                    const float a1 = std::abs(k1);
                    peakEnv_[1] = a1 > peakEnv_[1] ? a1 : a1 + (peakEnv_[1] - a1) * peakRel_;
                    if (peakEnv_[1] > lvl) lvl = peakEnv_[1];
                }
            }
            const float lvlDb = linToDbFloor(lvl, kDbFloor);

            // Hysteresis state machine; `sg` mirrors the comparison for Inverse.
            if (open_) {
                if (sg * lvlDb < sg * closeAt) {
                    if (holdLeft_ > 0) --holdLeft_;
                    else open_ = false;
                } else {
                    holdLeft_ = holdSamples_;
                }
            } else if (sg * lvlDb > sg * openAt) {
                open_ = true;
                holdLeft_ = holdSamples_;
            }

            const float targetDb = open_ ? 0.0f : -curveRed(sg * (lvlDb - p.thresh));

            if (targetDb > gainDb_) gainDb_ = std::min(targetDb, gainDb_ + attStep_);
            else if (targetDb < gainDb_) gainDb_ = std::max(targetDb, gainDb_ - relStep_);
            const float g = dbToLin(gainDb_);

            lineL_.write(xl);
            lineR_.write(xr);
            const float dl = lineL_.read(delay_ + 1);
            const float dr = lineR_.read(delay_ + 1);

            if (listen) {
                const float m = stereoKey ? (k0 + k1) * 0.5f : k0;
                l[i] = m * outLin_;
                r[i] = m * outLin_;
            } else {
                l[i] = (dl * g * mixWet_ + dl * dry) * outLin_;
                r[i] = (dr * g * mixWet_ + dr * dry) * outLin_;
            }
        }
    }

    float gainReductionDb() const override { return std::min(gainDb_, 0.0f); }

    void reset() override {
        for (size_t c = 0; c < 2; ++c) { hp_[c].resetState(); lp_[c].resetState(); }
        peakEnv_ = {0.0f, 0.0f};
        msEnv_ = {0.0f, 0.0f};
        open_ = false;
        holdLeft_ = 0;
        gainDb_ = 0.0f;
        lineL_.clear();
        lineR_.clear();
    }

    // Latency is intentionally 0: `look` delays the audio against the detector as a creative control
    // (the Rust device does the same), it is not reported for PDC.

private:
    void allocLines() {
        const int max = static_cast<int>((kMaxLookMs / 1000.0f) * sr_) + 2;
        lineL_.prepare(max);
        lineR_.prepare(max);
    }

    void recompute() {
        dirty_ = false;
        const Params p = p_;
        const float sr = sr_;
        const float hpF = std::min(p.hpf, p.lpf * 0.98f);
        const float lpF = std::max(p.lpf, p.hpf * 1.02f);
        const auto hc = hpCoeffs(hpF, sr), lc = lpCoeffs(lpF, sr);
        for (size_t c = 0; c < 2; ++c) { hp_[c].c = hc; lp_[c].c = lc; }

        peakRel_ = poleOf(kPeakRelSec, sr);
        rmsPole_ = poleOf(kRmsWinSec, sr);
        const float span = std::max(6.0f, std::abs(p.range));
        attStep_ = span / (std::max(p.attack, 0.0001f) * sr);
        relStep_ = span / (std::max(p.release, 0.0005f) * sr);
        holdSamples_ = static_cast<uint32_t>(std::max(p.hold, 0.0f) * sr);
        delay_ = std::min(static_cast<int>((p.look / 1000.0f) * sr), lineL_.maxDelay() - 1);
        delay_ = std::max(delay_, 0);
        mixWet_ = std::clamp(p.mix, 0.0f, 1.0f);
        outLin_ = dbToLin(p.gain);
        rangeDb_ = std::min(p.range, 0.0f);
        ratio_ = std::max(p.ratio, 1.0f);
        infinite_ = ratio_ >= 19.95f;
        knee_ = std::max(p.knee, 0.0f);
        sign_ = p.inverse >= 0.5f ? -1.0f : 1.0f;
    }

    // Reduction (dB >= 0) at x dB past the threshold.
    float curveRed(float x) const noexcept {
        if (infinite_) return -rangeDb_;
        const float k = knee_;
        const float slope = ratio_ - 1.0f;
        float red;
        if (x >= k * 0.5f) red = 0.0f;
        else if (x <= -k * 0.5f) red = slope * -x;
        else {
            const float d = x - k * 0.5f;
            red = (slope * d * d) / (2.0f * k);
        }
        return std::min(red, -rangeDb_);
    }

    float sr_ = 44100.0f;
    Params p_;
    bool dirty_ = true;
    std::array<Biquad, 2> hp_, lp_;
    std::array<float, 2> peakEnv_{}, msEnv_{};
    bool open_ = false;
    uint32_t holdLeft_ = 0;
    float gainDb_ = 0.0f;
    float peakRel_ = 0.0f, rmsPole_ = 0.0f, attStep_ = 1.0f, relStep_ = 1.0f;
    uint32_t holdSamples_ = 0;
    int delay_ = 0;
    float mixWet_ = 1.0f, outLin_ = 1.0f, rangeDb_ = -60.0f, ratio_ = 20.0f, knee_ = 3.0f, sign_ = 1.0f;
    bool infinite_ = true;
    DelayLine lineL_, lineR_;
};

}  // namespace

std::unique_ptr<EffectDevice> make_gate() { return std::make_unique<GateFx>(); }

}  // namespace ddaw::devices
