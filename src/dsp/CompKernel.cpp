#include "dsp/CompKernel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "dsp/Math.h"

namespace ddaw::dsp {

namespace {

constexpr float kKneeDb = 30.0f;            // DynamicsCompressorNode default; devices never set it
constexpr float kPreDelaySec = 0.006f;      // dynamics_compressor.cc: kParamPreDelay
constexpr unsigned kDivisionFrames = 32;    // the kernel processes its envelope in 32-frame divisions
constexpr float kSatReleaseSec = 0.0025f;
constexpr float kSpacingDb = 5.0f;          // adaptive release spacing
constexpr float kMeteringReleaseSec = 0.325f;
constexpr float kReleaseZones[4] = {0.09f, 0.16f, 0.42f, 0.98f};
constexpr float kHalfPi = float(std::numbers::pi / 2.0);
constexpr size_t kPreDelayMask = CompKernel::kMaxPreDelay - 1;

// Blink KneeCurve: linear up to the threshold, then an exponential approach with sharpness k.
float kneeCurve(float linThresh, float x, float k) noexcept {
    return x < linThresh ? x : linThresh + (1.0f - std::exp(-k * (x - linThresh))) / k;
}

// Blink SlopeAt: finite-difference dB/dB slope of the knee curve.
float slopeAt(float linThresh, float x, float k) noexcept {
    if (x < linThresh) return 1.0f;
    const float x2 = x * 1.001f;
    const float xDb = linToDb(x), x2Db = linToDb(x2);
    const float yDb = linToDb(kneeCurve(linThresh, x, k)), y2Db = linToDb(kneeCurve(linThresh, x2, k));
    return (y2Db - yDb) / (x2Db - xDb);
}

// Blink KAtSlope: bisect k so the knee's slope at the knee end equals desiredSlope (= 1/ratio).
float kAtSlope(float linThresh, float threshDb, float desiredSlope) noexcept {
    const float x = dbToLin(threshDb + kKneeDb);
    float minK = 0.1f, maxK = 10000.0f, k = 5.0f;
    for (int i = 0; i < 15; ++i) {
        if (slopeAt(linThresh, x, k) < desiredSlope) maxK = k; else minK = k;  // higher k asymptotes faster to slope 0
        k = std::sqrt(minK * maxK);
    }
    return k;
}

}  // namespace

CompKernel::CompKernel() : curveThreshDb_(std::numeric_limits<float>::quiet_NaN()), curveRatio_(std::numeric_limits<float>::quiet_NaN()) {
    thresh_.prepare(sr_, 15.0f); thresh_.snap(-20.0f);
    ratio_.prepare(sr_, 15.0f);  ratio_.snap(4.0f);
    updateCurve(thresh_.target(), ratio_.target());
    updateTimeCoeffs();
    resetKernel();
}

void CompKernel::configureNative(float threshDb, float ratio, float attackSec, float releaseSec) {
    thresh_.snap(std::clamp(threshDb, -100.0f, 0.0f));
    ratio_.snap(std::clamp(ratio, 1.0f, 20.0f));
    attackSec_ = std::clamp(attackSec, 0.001f, 1.0f);
    releaseSec_ = std::clamp(releaseSec, 0.001f, 1.0f);
    updateCurve(thresh_.target(), ratio_.target());
    updateTimeCoeffs();
    resetKernel();
}

// Blink UpdateStaticCurveParameters plus the makeup-gain lines of Process.
void CompKernel::updateCurve(float threshDb, float ratio) noexcept {
    curveThreshDb_ = threshDb;
    curveRatio_ = ratio;
    linThresh_ = dbToLin(threshDb);
    slope_ = 1.0f / ratio;
    k_ = kAtSlope(linThresh_, threshDb, slope_);
    kneeThreshDb_ = threshDb + kKneeDb;
    kneeThresh_ = dbToLin(kneeThreshDb_);
    yKneeDb_ = linToDb(kneeCurve(linThresh_, kneeThresh_, k_));
    // The node self-compensates: fullRangeGain = Saturate(1, k); makeup = (1/fullRangeGain)^0.6.
    masterLinearGain_ = std::pow(1.0f / saturate(1.0f), 0.6f);
}

// Blink Saturate: the full static curve (linear / knee / constant ratio).
float CompKernel::saturate(float x) const noexcept {
    if (x < kneeThresh_) return kneeCurve(linThresh_, x, k_);
    return dbToLin(yKneeDb_ + slope_ * (linToDb(x) - kneeThreshDb_));  // 1st-derivative matched at the knee end
}

// Adaptive-release coefficients are digit-for-digit the literals in dynamics_compressor_kernel.cc.
void CompKernel::updateTimeCoeffs() noexcept {
    const float sr = static_cast<float>(sr_);
    attackFrames_ = std::max(attackSec_, 0.001f) * sr;
    satReleaseFrames_ = kSatReleaseSec * sr;
    meteringReleaseK_ = 1.0f - std::exp(-1.0f / (sr * kMeteringReleaseSec));
    const float releaseFrames = sr * releaseSec_;
    const float y1 = releaseFrames * kReleaseZones[0], y2 = releaseFrames * kReleaseZones[1],
                y3 = releaseFrames * kReleaseZones[2], y4 = releaseFrames * kReleaseZones[3];
    relA_ = 0.9999999999999998f * y1 + 1.843222e-16f * y2 - 1.9373394e-16f * y3 + 8.824516e-18f * y4;
    relB_ = -1.5788320352845888f * y1 + 2.3305837032074286f * y2 - 0.9141194204840429f * y3 + 0.1623677525612032f * y4;
    relC_ = 0.5334142869106424f * y1 - 1.272736789213631f * y2 + 0.9258856042207512f * y3 - 0.18656310191776226f * y4;
    relD_ = 0.08783463138207234f * y1 - 0.1694162967925622f * y2 + 0.08588057951595272f * y3 - 0.00429891410546283f * y4;
    relE_ = -0.042416883008123074f * y1 + 0.1115693827987602f * y2 - 0.09764676325265872f * y3 + 0.028494263462021576f * y4;
}

// Blink Reset plus SetPreDelayTime: clear detector/gain state and the lookahead line.
void CompKernel::resetKernel() noexcept {
    detectorAverage_ = 0.0f;
    compressorGain_ = 1.0f;
    meteringGain_ = 1.0f;
    maxAttackCompressionDiffDb_ = -1.0f;
    scaledDesiredGain_ = 0.0f;
    envelopeRate_ = 1.0f;
    divCountdown_ = 0;
    delayL_.fill(0.0f);
    delayR_.fill(0.0f);
    preDelayFrames_ = std::min(static_cast<int>(kPreDelaySec * static_cast<float>(sr_)), kMaxPreDelay - 1);
    readIdx_ = 0;
    writeIdx_ = static_cast<size_t>(preDelayFrames_);
}

void CompKernel::prepare(double sr) {
    sr_ = std::max(sr, 1.0);
    const float t = thresh_.target(), r = ratio_.target();
    thresh_.prepare(sr_, 15.0f); thresh_.snap(t);
    ratio_.prepare(sr_, 15.0f);  ratio_.snap(r);
    updateCurve(t, r);
    updateTimeCoeffs();
    resetKernel();
}

void CompKernel::reset() {
    thresh_.snap(thresh_.target());
    ratio_.snap(ratio_.target());
    updateCurve(thresh_.target(), ratio_.target());
    resetKernel();
}

// Envelope work done once per 32-frame division: pick the slew rate toward the detector's desired
// gain (adaptive release / peak-tracked attack).
void CompKernel::updateDivision() noexcept {
    const float desiredGain = detectorAverage_;
    const float scaled = std::asin(desiredGain) / kHalfPi;  // pre-warp so we land on desiredGain after sin()
    scaledDesiredGain_ = scaled;
    const bool isReleasing = scaled > compressorGain_;
    float diffDb = linToDb(compressorGain_ / scaled);
    if (isReleasing) {
        maxAttackCompressionDiffDb_ = -1.0f;
        if (!std::isfinite(diffDb)) diffDb = -1.0f;  // gremlins
        // Adaptive release: heavier compression releases faster. Map -12..0 dB to x in 0..3.
        const float x = 0.25f * (std::clamp(diffDb, -12.0f, 0.0f) + 12.0f);
        const float x2 = x * x;
        const float releaseFrames = relA_ + relB_ * x + relC_ * x2 + relD_ * x2 * x + relE_ * x2 * x2;
        envelopeRate_ = dbToLin(kSpacingDb / releaseFrames);  // > 1
    } else {
        if (!std::isfinite(diffDb)) diffDb = 1.0f;
        if (maxAttackCompressionDiffDb_ == -1.0f || maxAttackCompressionDiffDb_ < diffDb) maxAttackCompressionDiffDb_ = diffDb;
        const float effAttenDiffDb = std::max(maxAttackCompressionDiffDb_, 0.5f);
        const float x = 0.25f / effAttenDiffDb;
        envelopeRate_ = 1.0f - std::pow(x, 1.0f / attackFrames_);  // < 1
    }
}

void CompKernel::process(float* l, float* r, int n) noexcept {
    for (int i = 0; i < n; ++i) {
        const float t = thresh_.next();
        const float ratio = ratio_.next();
        if (divCountdown_ == 0) {
            // Params are k-rate in the kernel: re-solve the curve only when they actually moved.
            if (std::abs(t - curveThreshDb_) > 1e-3f || std::abs(ratio - curveRatio_) > 1e-3f) updateCurve(t, ratio);
            updateDivision();
            divCountdown_ = kDivisionFrames;
        }
        --divCountdown_;

        // Write into the lookahead line; detect on the UNDELAYED input (stereo-linked channel max).
        delayL_[writeIdx_] = l[i];
        delayR_[writeIdx_] = r[i];
        const float absInput = std::max(std::abs(l[i]), std::abs(r[i]));

        // Shaped power through the static curve gives the instantaneous attenuation.
        const float shaped = saturate(absInput);
        const float attenuation = absInput <= 1e-4f ? 1.0f : shaped / absInput;
        const float attenuationDb = std::max(-linToDb(attenuation), 2.0f);
        const float satReleaseRate = dbToLin(attenuationDb / satReleaseFrames_) - 1.0f;
        const float rate = attenuation > detectorAverage_ ? satReleaseRate : 1.0f;  // instant attack
        detectorAverage_ += (attenuation - detectorAverage_) * rate;
        detectorAverage_ = std::min(detectorAverage_, 1.0f);
        if (!std::isfinite(detectorAverage_)) detectorAverage_ = 1.0f;

        // Exponential approach to the desired gain.
        if (envelopeRate_ < 1.0f) compressorGain_ += (scaledDesiredGain_ - compressorGain_) * envelopeRate_;
        else compressorGain_ = std::min(compressorGain_ * envelopeRate_, 1.0f);

        // Warp with sin() to smooth sharp exponential transition points.
        const float postWarp = std::sin(kHalfPi * compressorGain_);
        const float totalGain = masterLinearGain_ * postWarp;

        // Metering (the node's `reduction`): instant on peaks, slow release, excludes makeup.
        const float dbRealGain = linToDb(postWarp);
        if (dbRealGain < meteringGain_) meteringGain_ = dbRealGain;
        else meteringGain_ += (dbRealGain - meteringGain_) * meteringReleaseK_;

        l[i] = delayL_[readIdx_] * totalGain;
        r[i] = delayR_[readIdx_] * totalGain;
        readIdx_ = (readIdx_ + 1) & kPreDelayMask;
        writeIdx_ = (writeIdx_ + 1) & kPreDelayMask;
    }
}

}  // namespace ddaw::dsp
