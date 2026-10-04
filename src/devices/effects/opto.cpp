// `opto` effect: port of sf-dsp/src/fx/opto.rs, an LA-2A-style optical compressor (devices.ts
// OptoComp). The core is a port of Blink's DynamicsCompressor with the fixed per-mode curves of the
// browser device (Comp: ratio 3, knee 30 dB, attack 10 ms, release 450 ms; Limit: ratio 10, knee
// 12 dB, attack 5 ms, release 300 ms) and Peak Reduction lowering the threshold to -40 dB. It cannot
// reuse dsp::CompKernel: that kernel hard-wires a 30 dB knee, a schema-driven ratio/threshold curve
// and a different detector/metering path, whereas opto solves the knee per mode in threshold-
// normalised form and adds the opto-cell slow envelope (two-stage, program-dependent release).
// Schema: reduction 0..1, gain -12..24 dB, mode 0/1. Fully wet; 6 ms lookahead pre-delay.
#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

#include "core/Device.h"
#include "devices/schema/Schema.generated.h"
#include "dsp/Math.h"
#include "dsp/Smoother.h"

namespace ddaw::devices {
namespace {

using namespace ddaw::dsp;

enum P : uint16_t { Reduction, Gain, Mode };

constexpr float kRatio[2] = {3.0f, 10.0f};
constexpr float kKneeDb[2] = {30.0f, 12.0f};
constexpr float kAttackS[2] = {0.010f, 0.005f};
constexpr float kReleaseS[2] = {0.45f, 0.30f};

constexpr float kSlowAttackS = 0.5f;
constexpr float kSlowReleaseMul = 6.0f;
constexpr float kStageMix = 0.5f;
constexpr float kDetFloorDb = -96.0f;

constexpr float kSatReleaseS = 0.0025f;
constexpr float kPreDelayS = 0.006f;
constexpr unsigned kDivisionFrames = 32;
constexpr float kSpacingDb = 5.0f;
constexpr float kReleaseZones[4] = {0.09f, 0.16f, 0.42f, 0.98f};
constexpr float kHalfPi = float(std::numbers::pi / 2.0);

float tauCoeff(float tau, float sr) noexcept { return 1.0f - std::exp(-1.0f / (std::max(tau, 1e-4f) * sr)); }

// Blink KneeCurve with linear threshold 1: identity below, exponential approach above.
float kneeCurve(float x, float k) noexcept { return x < 1.0f ? x : 1.0f + (1.0f - std::exp(-k * (x - 1.0f))) / k; }

float kneeSlopeAt(float x, float k) noexcept {
    if (x < 1.0f) return 1.0f;
    const float x2 = x * 1.001f;
    const float xDb = linToDb(x), x2Db = linToDb(x2);
    const float yDb = linToDb(kneeCurve(x, k)), y2Db = linToDb(kneeCurve(x2, k));
    return (y2Db - yDb) / (x2Db - xDb);
}

// Blink KAtSlope: bisect k so the dB slope at the knee end (kneeDb above threshold) is desiredSlope.
float kAtSlope(float desiredSlope, float kneeDb) noexcept {
    const float x = dbToLin(kneeDb);
    float minK = 0.1f, maxK = 10000.0f, k = 5.0f;
    for (int i = 0; i < 15; ++i) {
        if (kneeSlopeAt(x, k) < desiredSlope) maxK = k; else minK = k;
        k = std::sqrt(minK * maxK);
    }
    return k;
}

class OptoFx final : public EffectDevice {
public:
    OptoFx() {
        thresh_.prepare(sr_, 15.0f); thresh_.snap(-40.0f * 0.4f);  // schema default reduction 0.4
        makeup_.prepare(sr_, 15.0f); makeup_.snap(1.0f);
        updateMode();
        resizeDelay();
    }

    std::span<const ParamSpec> params() const override { return schema::kFxOpto; }

    void prepare(double sr, int) override {
        sr_ = static_cast<float>(std::max(sr, 1.0));
        thresh_.prepare(sr_, 15.0f);
        makeup_.prepare(sr_, 15.0f);
        updateMode();
        resizeDelay();
        resetKernel();
    }

    void setParam(uint16_t index, float v) override {
        switch (index) {
            case Reduction: thresh_.setTarget(-40.0f * std::clamp(v, 0.0f, 1.0f)); break;
            case Gain: makeup_.setTarget(dbToLin(std::clamp(v, -12.0f, 24.0f))); break;
            case Mode: {
                const int m = v >= 0.5f ? 1 : 0;
                if (m != mode_) { mode_ = m; updateMode(); }
                break;
            }
            default: break;
        }
    }

    void process(float* l, float* r, int n, const ProcessContext&, const ModInputs&) override {
        const size_t len = delayL_.size();
        for (int i = 0; i < n; ++i) {
            const float thresh = thresh_.next();
            const float makeup = makeup_.next();
            if (divPos_ == 0) computeDivisionRates();
            divPos_ = (divPos_ + 1) % kDivisionFrames;

            // Detector: linear attenuation of the stereo peak through the static curve, instant
            // attack, sat-release.
            const float level = std::max(std::abs(l[i]), std::abs(r[i]));
            float attenuation, attenuationDb;
            if (level <= 1e-4f) {
                attenuation = 1.0f; attenuationDb = 2.0f;
            } else {
                const float over = linToDbFloor(level, kDetFloorDb) - thresh;
                const float gr = grTarget(over);
                attenuation = dbToLin(gr);
                attenuationDb = std::max(-gr, 2.0f);
            }
            if (attenuation > detAvg_) {
                const float satRate = dbToLin(attenuationDb / satReleaseFrames_) - 1.0f;
                detAvg_ += (attenuation - detAvg_) * satRate;
                detAvg_ = std::min(detAvg_, 1.0f);
            } else {
                detAvg_ = attenuation;
            }

            // Envelope: exponential approach on attack, constant-dB-rate climb on release.
            if (envRate_ < 1.0f) compGain_ += (scaledDesired_ - compGain_) * envRate_;
            else compGain_ = std::min(compGain_ * envRate_, 1.0f);

            const float warped = std::sin(kHalfPi * compGain_);
            const float fastGr = linToDbFloor(warped, kDetFloorDb);

            // Opto slow cell: charges toward the kernel GR over ~0.5 s, discharges at 6x the release.
            if (fastGr < envSlow_) envSlow_ += (fastGr - envSlow_) * attSlow_;
            else envSlow_ += (fastGr - envSlow_) * relSlow_;
            const float gr = kStageMix * fastGr + (1.0f - kStageMix) * envSlow_;
            gr_ = gr;

            const float g = dbToLin(gr + autoMakeupDb(thresh)) * makeup;
            const float dl = delayL_[delayIdx_], dr = delayR_[delayIdx_];
            delayL_[delayIdx_] = l[i];
            delayR_[delayIdx_] = r[i];
            delayIdx_ = (delayIdx_ + 1) % len;
            l[i] = dl * g;
            r[i] = dr * g;
        }
    }

    void reset() override {
        resetKernel();
        std::fill(delayL_.begin(), delayL_.end(), 0.0f);
        std::fill(delayR_.begin(), delayR_.end(), 0.0f);
        delayIdx_ = 0;
        thresh_.snap(thresh_.target());
        makeup_.snap(makeup_.target());
    }

    int latencySamples() const override { return static_cast<int>(delayL_.size()); }  // 6 ms lookahead
    float gainReductionDb() const override { return std::min(gr_, 0.0f); }

private:
    void resetKernel() noexcept {
        detAvg_ = 0.0f; compGain_ = 1.0f; maxAttackDiffDb_ = -1.0f; envRate_ = 1.0f;
        scaledDesired_ = 1.0f; divPos_ = 0; envSlow_ = 0.0f; gr_ = 0.0f;
    }

    void resizeDelay() {
        const size_t frames = std::max<size_t>(static_cast<size_t>(kPreDelayS * sr_), 1);
        delayL_.assign(frames, 0.0f);
        delayR_.assign(frames, 0.0f);
        delayIdx_ = 0;
    }

    void updateMode() noexcept {
        const size_t m = static_cast<size_t>(mode_);
        slope_ = 1.0f / kRatio[m] - 1.0f;
        knee_ = kKneeDb[m];
        kKnee_ = kAtSlope(1.0f / kRatio[m], knee_);
        yKneeDb_ = linToDb(kneeCurve(dbToLin(knee_), kKnee_));
        invAttackFrames_ = 1.0f / (std::max(kAttackS[m], 0.001f) * sr_);
        releasePoly(kReleaseS[m] * sr_);
        satReleaseFrames_ = kSatReleaseS * sr_;
        attSlow_ = tauCoeff(kSlowAttackS, sr_);
        relSlow_ = tauCoeff(kReleaseS[m] * kSlowReleaseMul, sr_);
    }

    // Blink's 4th-order fit through the four release zones (kABase..kEBase rows as f32 literals).
    void releasePoly(float releaseFrames) noexcept {
        const float y1 = releaseFrames * kReleaseZones[0], y2 = releaseFrames * kReleaseZones[1],
                    y3 = releaseFrames * kReleaseZones[2], y4 = releaseFrames * kReleaseZones[3];
        relPoly_[0] = 1.0f * y1 + 1.8432219e-16f * y2 - 1.9373394e-16f * y3 + 8.824516e-18f * y4;
        relPoly_[1] = -1.578832f * y1 + 2.3305838f * y2 - 0.9141194f * y3 + 0.16236775f * y4;
        relPoly_[2] = 0.5334143f * y1 - 1.2727368f * y2 + 0.9258856f * y3 - 0.1865631f * y4;
        relPoly_[3] = 0.087834634f * y1 - 0.1694163f * y2 + 0.08588058f * y3 - 0.004298914f * y4;
        relPoly_[4] = -0.04241688f * y1 + 0.11156938f * y2 - 0.097646765f * y3 + 0.028494263f * y4;
    }

    // Desired GR in dB (<= 0) for a level `over` dB above threshold.
    float grTarget(float over) const noexcept {
        if (over <= 0.0f) return 0.0f;
        if (over < knee_) return linToDb(kneeCurve(dbToLin(over), kKnee_)) - over;
        return yKneeDb_ - knee_ + slope_ * (over - knee_);
    }

    // The native node's built-in makeup: -0.6 x the static-curve GR at 0 dBFS.
    float autoMakeupDb(float threshDb) const noexcept { return -0.6f * grTarget(-threshDb); }

    // Blink "calculate desired gain / deal with envelopes", once per 32-frame division.
    void computeDivisionRates() noexcept {
        const float desired = std::clamp(detAvg_, 0.0f, 1.0f);
        scaledDesired_ = std::asin(desired) / kHalfPi;
        const bool isReleasing = scaledDesired_ > compGain_;
        float diffDb;
        if (scaledDesired_ == 0.0f) diffDb = isReleasing ? -1.0f : 1.0f;
        else diffDb = linToDb(compGain_ / scaledDesired_);
        if (!std::isfinite(diffDb)) diffDb = isReleasing ? -1.0f : 1.0f;

        if (isReleasing) {
            maxAttackDiffDb_ = -1.0f;
            const float x = 0.25f * (std::clamp(diffDb, -12.0f, 0.0f) + 12.0f);
            const float x2 = x * x;
            const float releaseFrames = relPoly_[0] + relPoly_[1] * x + relPoly_[2] * x2 + relPoly_[3] * x2 * x + relPoly_[4] * x2 * x2;
            envRate_ = dbToLin(kSpacingDb / releaseFrames);
        } else {
            if (maxAttackDiffDb_ == -1.0f || maxAttackDiffDb_ < diffDb) maxAttackDiffDb_ = diffDb;
            const float effDiff = std::max(maxAttackDiffDb_, 0.5f);
            envRate_ = 1.0f - std::pow(0.25f / effDiff, invAttackFrames_);
        }
    }

    float sr_ = 44100.0f;
    int mode_ = 0;
    Smoother thresh_, makeup_;
    float slope_ = 0, knee_ = 0, kKnee_ = 1, yKneeDb_ = 0;
    float invAttackFrames_ = 0, relPoly_[5] = {};
    float satReleaseFrames_ = 1, attSlow_ = 0, relSlow_ = 0;
    float detAvg_ = 0, compGain_ = 1, maxAttackDiffDb_ = -1, envRate_ = 1, scaledDesired_ = 1;
    unsigned divPos_ = 0;
    std::vector<float> delayL_, delayR_;
    size_t delayIdx_ = 0;
    float envSlow_ = 0, gr_ = 0;
};

}  // namespace

std::unique_ptr<EffectDevice> make_opto() { return std::make_unique<OptoFx>(); }

}  // namespace ddaw::devices
