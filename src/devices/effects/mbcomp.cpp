// `mbcomp` effect: port of sf-dsp/src/fx/mbcomp.rs (the sf-mbdyn AudioWorklet).
// 3-band Linkwitz-Riley (LR4) crossover -> per-band compressor or downward expander -> makeup -> sum.
// LR4 = two cascaded RBJ Butterworth biquads per slope, so adjacent bands sum flat. Crossover
// coefficients follow the smoothed xlo/xhi at block rate (like the worklet's block-boundary recompute).
// Per-band gain reduction (windowed max, ~30 ms cadence) is kept in gr_report_; the EffectDevice
// interface has only one meter, so gainReductionDb() returns the deepest band.
// Schema: mode (comp/expand), xlo 60..1000, xhi 1000..12000, attack, release, per band thresh/ratio/gain.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>

#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/Math.h"
#include "dsp/Smoother.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

// Index in schema::kFxMbcomp.
enum P : uint16_t { Mode, Xlo, Xhi, Attack, Release, B0Thresh, B0Ratio, B0Gain, B1Thresh, B1Ratio, B1Gain, B2Thresh, B2Ratio, B2Gain };

constexpr float kDbToNat = 0.115129250f;  // ln(10)/20: exp(-grDb * k) = 10^(-grDb/20)
constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kInvSqrt2 = 0.70710678118654752f;

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
    const float cw = std::cos(w0), sw = std::sin(w0);
    const float alpha = sw / (2.0f * kInvSqrt2);
    const float a0 = 1.0f + alpha;
    return {((1.0f - cw) * 0.5f) / a0, (1.0f - cw) / a0, ((1.0f - cw) * 0.5f) / a0, (-2.0f * cw) / a0, (1.0f - alpha) / a0};
}
std::array<float, 5> hpCoeffs(float f, float sr) {
    const float w0 = 2.0f * kPi * std::min(f, sr * 0.49f) / sr;
    const float cw = std::cos(w0), sw = std::sin(w0);
    const float alpha = sw / (2.0f * kInvSqrt2);
    const float a0 = 1.0f + alpha;
    return {((1.0f + cw) * 0.5f) / a0, (-(1.0f + cw)) / a0, ((1.0f + cw) * 0.5f) / a0, (-2.0f * cw) / a0, (1.0f - alpha) / a0};
}

class MbcompFx final : public EffectDevice {
public:
    MbcompFx() {
        xlo_.snap(250.0f);
        xhi_.snap(2500.0f);
        for (size_t b = 0; b < 3; ++b) { thresh_[b].snap(-24.0f); ratio_[b].snap(2.0f); makeup_[b].snap(1.0f); }
        updateEnvCoeffs();
        recomputeFilters(250.0f, 2500.0f);
    }

    std::span<const ParamSpec> params() const override { return schema::kFxMbcomp; }

    void prepare(double sr, int) override {
        sr_ = static_cast<float>(std::max(sr, 1.0));
        repEvery_ = static_cast<int>(std::max(sr_ * 0.03f, 256.0f));
        xlo_.prepare(sr_, 15.0f);
        xhi_.prepare(sr_, 15.0f);
        for (size_t b = 0; b < 3; ++b) {
            thresh_[b].prepare(sr_, 15.0f);
            ratio_[b].prepare(sr_, 15.0f);
            makeup_[b].prepare(sr_, 15.0f);
        }
        updateEnvCoeffs();
        reset();
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Mode: mode_ = static_cast<int>(v); break;
            case Xlo: xlo_.setTarget(std::clamp(v, 60.0f, 1000.0f)); break;
            case Xhi: xhi_.setTarget(std::clamp(v, 1000.0f, 12000.0f)); break;
            case Attack: attackSec_ = std::clamp(v, 0.001f, 0.3f); updateEnvCoeffs(); break;
            case Release: releaseSec_ = std::clamp(v, 0.02f, 1.0f); updateEnvCoeffs(); break;
            case B0Thresh: thresh_[0].setTarget(std::clamp(v, -60.0f, 0.0f)); break;
            case B1Thresh: thresh_[1].setTarget(std::clamp(v, -60.0f, 0.0f)); break;
            case B2Thresh: thresh_[2].setTarget(std::clamp(v, -60.0f, 0.0f)); break;
            case B0Ratio: ratio_[0].setTarget(std::clamp(v, 1.0f, 20.0f)); break;
            case B1Ratio: ratio_[1].setTarget(std::clamp(v, 1.0f, 20.0f)); break;
            case B2Ratio: ratio_[2].setTarget(std::clamp(v, 1.0f, 20.0f)); break;
            case B0Gain: makeup_[0].setTarget(dbToLin(std::clamp(v, -24.0f, 24.0f))); break;
            case B1Gain: makeup_[1].setTarget(dbToLin(std::clamp(v, -24.0f, 24.0f))); break;
            case B2Gain: makeup_[2].setTarget(dbToLin(std::clamp(v, -24.0f, 24.0f))); break;
            default: break;
        }
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        if (n <= 0) return;
        const float curLo = xlo_.current(), curHi = xhi_.current();
        if (std::abs(curLo - appliedXlo_) > 0.01f || std::abs(curHi - appliedXhi_) > 0.01f) recomputeFilters(curLo, curHi);
        const bool comp = mode_ == 0;
        for (int i = 0; i < n; ++i) {
            xlo_.next();
            xhi_.next();
            float th[3], ra[3], mk[3];
            for (size_t b = 0; b < 3; ++b) { th[b] = thresh_[b].next(); ra[b] = ratio_[b].next(); mk[b] = makeup_[b].next(); }
            float bl[3], br[3];
            split(bq_[0], l[i], bl);
            split(bq_[1], r[i], br);
            float sl = 0.0f, sr = 0.0f;
            for (size_t b = 0; b < 3; ++b) {
                const float rect = std::max(std::abs(bl[b]), std::abs(br[b]));
                const float c = rect > env_[b] ? attC_ : relC_;
                env_[b] = rect + (env_[b] - rect) * c;
                const float lvlDb = 20.0f * std::log10(env_[b] + 1e-9f);
                const float ratio = std::max(ra[b], 1.0f);
                float grDb = 0.0f;
                if (comp) {
                    const float over = lvlDb - th[b];
                    if (over > 0.0f) grDb = over * (1.0f - 1.0f / ratio);
                } else {
                    const float under = th[b] - lvlDb;
                    if (under > 0.0f) grDb = std::min(under * (ratio - 1.0f), 48.0f);
                }
                if (grDb > grMax_[b]) grMax_[b] = grDb;
                const float gain = std::exp(-grDb * kDbToNat) * mk[b];
                sl += bl[b] * gain;
                sr += br[b] * gain;
            }
            l[i] = sl;
            r[i] = sr;
        }
        rep_ += n;
        if (rep_ >= repEvery_) {
            rep_ = 0;
            grReport_ = grMax_;
            grMax_ = {0.0f, 0.0f, 0.0f};
        }
    }

    // Overall meter: the deepest band's reduction (dB <= 0). Per-band values are in grReport_
    // (Rust gr_bands()); the EffectDevice interface has no per-band meter API yet.
    float gainReductionDb() const override { return -std::max({grReport_[0], grReport_[1], grReport_[2]}); }

    void reset() override {
        xlo_.snap(xlo_.target());
        xhi_.snap(xhi_.target());
        for (size_t b = 0; b < 3; ++b) { thresh_[b].snap(thresh_[b].target()); ratio_[b].snap(ratio_[b].target()); makeup_[b].snap(makeup_[b].target()); }
        for (auto& ch : bq_) for (auto& q : ch) q.resetState();
        recomputeFilters(xlo_.current(), xhi_.current());
        env_ = {0.0f, 0.0f, 0.0f};
        grMax_ = {0.0f, 0.0f, 0.0f};
        grReport_ = {0.0f, 0.0f, 0.0f};
        rep_ = 0;
    }

private:
    void updateEnvCoeffs() {
        attC_ = std::exp(-1.0f / (std::max(attackSec_, 0.0005f) * sr_));
        relC_ = std::exp(-1.0f / (std::max(releaseSec_, 0.005f) * sr_));
    }

    void recomputeFilters(float xlo, float xhi) {
        const float lo = std::min(xlo, xhi - 20.0f);  // keep crossovers ordered
        const float hi = std::max(xhi, xlo + 20.0f);
        const auto lp = lpCoeffs(lo, sr_), hp = hpCoeffs(lo, sr_), lp2 = lpCoeffs(hi, sr_), hp2 = hpCoeffs(hi, sr_);
        for (auto& ch : bq_) {
            ch[0].c = lp; ch[1].c = lp;      // xlo low
            ch[2].c = hp; ch[3].c = hp;      // xlo high
            ch[4].c = lp2; ch[5].c = lp2;    // xhi mid
            ch[6].c = hp2; ch[7].c = hp2;    // xhi high
        }
        appliedXlo_ = xlo;
        appliedXhi_ = xhi;
    }

    static void split(std::array<Biquad, 8>& bank, float x, float* out) noexcept {
        out[0] = bank[1].proc(bank[0].proc(x));
        const float h = bank[3].proc(bank[2].proc(x));
        out[1] = bank[5].proc(bank[4].proc(h));
        out[2] = bank[7].proc(bank[6].proc(h));
    }

    float sr_ = 44100.0f;
    int mode_ = 0;
    Smoother xlo_, xhi_;
    float attackSec_ = 0.02f, releaseSec_ = 0.18f, attC_ = 0.0f, relC_ = 0.0f;
    std::array<Smoother, 3> thresh_, ratio_, makeup_;
    std::array<std::array<Biquad, 8>, 2> bq_;
    std::array<float, 3> env_{}, grMax_{}, grReport_{};
    float appliedXlo_ = 0.0f, appliedXhi_ = 0.0f;
    int rep_ = 0, repEvery_ = 1323;
};

}  // namespace

std::unique_ptr<EffectDevice> make_mbcomp() { return std::make_unique<MbcompFx>(); }

}  // namespace ddaw::devices
