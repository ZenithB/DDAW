// `eq7` effect: port of sf-dsp/src/fx/eq7.rs, the Eq7 Tone composite: a series chain of seven Web Audio
// biquads (low shelf, five peaking bands, high shelf) with flat b{i}_(freq|gain|q) params. Coefficients
// follow the Web Audio spec (RBJ cookbook): shelves ignore Q (fixed S = 1), peaking uses Q directly
// (browser clamps Q at 0.05). Coefficients are refreshed once per block from the smoothed params.
// Fully wet. The biquads are device-local, as in the Rust file.
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

using namespace ddaw::dsp;

constexpr int kBands = 7;
constexpr std::array<float, kBands> kFreqs = {60.0f, 150.0f, 400.0f, 1000.0f, 2500.0f, 6000.0f, 12000.0f};

enum class Kind { LowShelf, Peaking, HighShelf };
constexpr Kind kindOf(int i) { return i == 0 ? Kind::LowShelf : (i == kBands - 1 ? Kind::HighShelf : Kind::Peaking); }

// One stereo biquad band (transposed direct form II per channel).
struct Band {
    Kind kind = Kind::Peaking;
    Smoother freq, gain, q;
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;  // normalised (a0 divided out)
    float zl1 = 0, zl2 = 0, zr1 = 0, zr2 = 0;

    void init(int i) {
        kind = kindOf(i);
        freq.snap(kFreqs[static_cast<size_t>(i)]);
        gain.snap(0.0f);
        q.snap(1.0f);
    }
    void prepare(double sr) {
        freq.prepare(sr, 15.0f); gain.prepare(sr, 15.0f); q.prepare(sr, 15.0f);
        snapAll();
        resetState();
        updateCoeffs(static_cast<float>(sr));
    }
    void snapAll() { freq.snap(freq.target()); gain.snap(gain.target()); q.snap(q.target()); }
    void resetState() { zl1 = zl2 = zr1 = zr2 = 0.0f; }

    void advance(int n) {
        for (int i = 0; i < n; ++i) { freq.next(); gain.next(); q.next(); }
    }

    void updateCoeffs(float sr) {
        sr = std::max(sr, 1.0f);
        const float f = std::clamp(freq.current(), 10.0f, 0.49f * sr);  // w0 strictly inside (0, pi)
        const float g = std::clamp(gain.current(), -24.0f, 24.0f);
        const float qv = std::max(q.current(), 0.05f);

        const float w0 = 2.0f * std::numbers::pi_v<float> * f / sr;
        const float cw = std::cos(w0), sw = std::sin(w0);
        const float a = std::pow(10.0f, g / 40.0f);

        float nb0, nb1, nb2, a0, na1, na2;
        switch (kind) {
            case Kind::Peaking: {
                const float alpha = sw / (2.0f * qv);
                nb0 = 1.0f + alpha * a; nb1 = -2.0f * cw; nb2 = 1.0f - alpha * a;
                a0 = 1.0f + alpha / a;  na1 = -2.0f * cw; na2 = 1.0f - alpha / a;
                break;
            }
            case Kind::LowShelf: {
                const float aa = sw * std::sqrt(2.0f * a);  // 2*sqrt(A)*alpha, alpha = sin/2*sqrt2
                nb0 = a * ((a + 1.0f) - (a - 1.0f) * cw + aa);
                nb1 = 2.0f * a * ((a - 1.0f) - (a + 1.0f) * cw);
                nb2 = a * ((a + 1.0f) - (a - 1.0f) * cw - aa);
                a0 = (a + 1.0f) + (a - 1.0f) * cw + aa;
                na1 = -2.0f * ((a - 1.0f) + (a + 1.0f) * cw);
                na2 = (a + 1.0f) + (a - 1.0f) * cw - aa;
                break;
            }
            case Kind::HighShelf: default: {
                const float aa = sw * std::sqrt(2.0f * a);
                nb0 = a * ((a + 1.0f) + (a - 1.0f) * cw + aa);
                nb1 = -2.0f * a * ((a - 1.0f) + (a + 1.0f) * cw);
                nb2 = a * ((a + 1.0f) + (a - 1.0f) * cw - aa);
                a0 = (a + 1.0f) - (a - 1.0f) * cw + aa;
                na1 = 2.0f * ((a - 1.0f) - (a + 1.0f) * cw);
                na2 = (a + 1.0f) - (a - 1.0f) * cw - aa;
                break;
            }
        }
        const float inv = 1.0f / a0;
        b0 = nb0 * inv; b1 = nb1 * inv; b2 = nb2 * inv; a1 = na1 * inv; a2 = na2 * inv;
    }

    void tickStereo(float& l, float& r) {
        const float yl = b0 * l + zl1;
        zl1 = b1 * l - a1 * yl + zl2;
        zl2 = b2 * l - a2 * yl;
        const float yr = b0 * r + zr1;
        zr1 = b1 * r - a1 * yr + zr2;
        zr2 = b2 * r - a2 * yr;
        l = yl;
        r = yr;
    }
};

class Eq7Fx final : public EffectDevice {
public:
    Eq7Fx() {
        for (int i = 0; i < kBands; ++i) bands_[static_cast<size_t>(i)].init(i);
    }

    std::span<const ParamSpec> params() const override { return schema::kFxEq7; }

    void prepare(double sr, int) override {
        sr_ = static_cast<float>(std::max(sr, 1.0));
        for (auto& b : bands_) b.prepare(sr_);
    }

    // Table layout: band i occupies indices 3i (freq), 3i+1 (gain), 3i+2 (q).
    void setParam(uint16_t index, float v) override {
        if (index >= kBands * 3) return;
        auto& b = bands_[index / 3];
        switch (index % 3) {
            case 0: b.freq.setTarget(std::clamp(v, 10.0f, 20000.0f)); break;
            case 1: b.gain.setTarget(std::clamp(v, -24.0f, 24.0f)); break;
            default: b.q.setTarget(std::max(v, 0.05f)); break;
        }
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        if (n <= 0) return;
        for (auto& b : bands_) { b.advance(n); b.updateCoeffs(sr_); }
        for (int i = 0; i < n; ++i) {
            float sl = l[i], sr = r[i];
            for (auto& b : bands_) b.tickStereo(sl, sr);
            l[i] = sl;
            r[i] = sr;
        }
    }

    // The engine calls reset() after loading params to snap smoothers to them.
    void reset() override {
        for (auto& b : bands_) { b.snapAll(); b.resetState(); b.updateCoeffs(sr_); }
    }

private:
    float sr_ = 44100.0f;
    std::array<Band, kBands> bands_{};
};

}  // namespace

std::unique_ptr<EffectDevice> make_eq7() { return std::make_unique<Eq7Fx>(); }

}  // namespace ddaw::devices
