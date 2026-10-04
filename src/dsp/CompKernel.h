#pragma once
// Blink/WebKit DynamicsCompressorKernel, ported operation-for-operation from synthyy's
// sf-dsp/src/fx/comp.rs. Tone.Compressor and Tone.Limiter are thin wrappers around the browser's
// native DynamicsCompressorNode, so this kernel is the behaviour spec for both the `comp` effect and
// the master limiter used for browser-golden parity.
//
// Static curve: linear below threshold, an exponential soft knee over [thresh, thresh + 30 dB], then
// a constant-ratio dB slope. The node self-compensates with a makeup gain of (1/Saturate(1,k))^0.6,
// so signals below threshold are amplified, exactly as in the browser. Detector: stereo-linked,
// instant attack, ~2.5 ms release, adaptive release polynomial, and a 6 ms lookahead pre-delay.
#include <array>
#include <cstddef>

#include "dsp/Smoother.h"

namespace ddaw::dsp {

class CompKernel {
public:
    static constexpr int kMaxPreDelay = 1024;  // power of two

    CompKernel();  // schema defaults: thresh -20 dB, ratio 4, attack 10 ms, release 200 ms

    // The native node's own parameters, bypassing the `comp` schema clamps (Tone.Limiter needs a
    // 10 ms release, below the schema's 20 ms floor). Clamps: thresh -100..0, ratio 1..20, times >= 1 ms.
    void configureNative(float threshDb, float ratio, float attackSec, float releaseSec);

    void prepare(double sampleRate);
    void reset();

    // Schema-clamped setters (thresh -60..0, ratio 1..20, attack 1 ms..0.3 s, release 20 ms..1 s).
    void setThresh(float db) noexcept { thresh_.setTarget(std::clamp(db, -60.0f, 0.0f)); }
    void setRatio(float r) noexcept { ratio_.setTarget(std::clamp(r, 1.0f, 20.0f)); }
    void setAttack(float s) noexcept { attackSec_ = std::clamp(s, 0.001f, 0.3f); updateTimeCoeffs(); }
    void setRelease(float s) noexcept { releaseSec_ = std::clamp(s, 0.02f, 1.0f); updateTimeCoeffs(); }

    // In place, planar stereo. Real-time safe.
    void process(float* l, float* r, int n) noexcept;

    float grDb() const noexcept { return std::min(meteringGain_, 0.0f); }  // the node's `reduction`
    int latencySamples() const noexcept { return preDelayFrames_; }

private:
    void updateCurve(float threshDb, float ratio) noexcept;
    float saturate(float x) const noexcept;
    void updateTimeCoeffs() noexcept;
    void resetKernel() noexcept;
    void updateDivision() noexcept;

    double sr_ = 44100.0;
    Smoother thresh_, ratio_;
    float attackSec_ = 0.01f, releaseSec_ = 0.2f;

    // static-curve cache
    float curveThreshDb_, curveRatio_;
    float linThresh_ = 0, kneeThresh_ = 0, kneeThreshDb_ = 0, yKneeDb_ = 0, slope_ = 1, k_ = 5, masterLinearGain_ = 1;

    // time-derived coefficients
    float attackFrames_ = 1, satReleaseFrames_ = 1, meteringReleaseK_ = 0;
    float relA_ = 0, relB_ = 0, relC_ = 0, relD_ = 0, relE_ = 0;

    // kernel state
    float detectorAverage_ = 0, compressorGain_ = 1, meteringGain_ = 1;
    float maxAttackCompressionDiffDb_ = -1, scaledDesiredGain_ = 0, envelopeRate_ = 1;
    unsigned divCountdown_ = 0;

    // lookahead pre-delay
    std::array<float, kMaxPreDelay> delayL_{}, delayR_{};
    size_t readIdx_ = 0, writeIdx_ = 0;
    int preDelayFrames_ = 0;
};

}  // namespace ddaw::dsp
