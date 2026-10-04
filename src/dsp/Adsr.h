#pragma once
// ADSR matching Tone.js Envelope semantics: linear attack to 1 from the current level (so
// retriggers do not click), exponential decay to sustain, exponential release to 0 from wherever
// the level is. Port of sf-dsp/src/util/adsr.rs.
#include <algorithm>
#include <cmath>

namespace ddaw::dsp {

class Adsr {
public:
    Adsr() { update(); }

    void prepare(float sampleRate) noexcept { sr_ = std::max(sampleRate, 1.0f); update(); }
    void setAdsr(float attackS, float decayS, float sustain, float releaseS) noexcept {
        attackS_ = std::max(attackS, 0.0f);
        decayS_ = std::max(decayS, 0.0f);
        sustain_ = std::clamp(sustain, 0.0f, 1.0f);
        releaseS_ = std::max(releaseS, 0.0f);
        update();
    }
    void noteOn() noexcept { stage_ = Stage::Attack; }
    void noteOff() noexcept { if (stage_ != Stage::Idle) stage_ = Stage::Release; }
    void reset() noexcept { stage_ = Stage::Idle; value_ = 0.0f; }
    bool isActive() const noexcept { return stage_ != Stage::Idle; }
    float value() const noexcept { return value_; }

    float next() noexcept {
        switch (stage_) {
            case Stage::Idle: break;
            case Stage::Attack:
                value_ += attackStep_;
                if (value_ >= 1.0f) { value_ = 1.0f; stage_ = Stage::Decay; }
                break;
            case Stage::Decay:
                value_ = sustain_ + (value_ - sustain_) * decayCoeff_;
                if (value_ - sustain_ < kIdleFloor) {
                    value_ = sustain_;
                    stage_ = Stage::Sustain;
                    if (sustain_ <= 0.0f) { stage_ = Stage::Idle; value_ = 0.0f; }
                }
                break;
            case Stage::Sustain: value_ = sustain_; break;
            case Stage::Release:
                value_ *= releaseCoeff_;
                if (value_ < kIdleFloor) { value_ = 0.0f; stage_ = Stage::Idle; }
                break;
        }
        return value_;
    }

private:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };
    // Exp stages approach their target asymptotically; tau = time / 6.2 puts the level ~99.8% of
    // the way there when the nominal stage time has elapsed.
    static constexpr float kExpTimeMult = 6.2f;
    static constexpr float kIdleFloor = 1e-4f;  // about -80 dB: release is over

    static float expCoeff(float timeS, float sr) noexcept {
        const float tau = timeS * sr / kExpTimeMult;
        return tau < 1.0f ? 0.0f : std::exp(-1.0f / tau);
    }
    void update() noexcept {
        const float a = attackS_ * sr_;
        attackStep_ = a < 1.0f ? 1.0f : 1.0f / a;
        decayCoeff_ = expCoeff(decayS_, sr_);
        releaseCoeff_ = expCoeff(releaseS_, sr_);
    }

    Stage stage_ = Stage::Idle;
    float value_ = 0.0f, attackStep_ = 0.0f, decayCoeff_ = 0.0f, releaseCoeff_ = 0.0f;
    float sustain_ = 0.5f, sr_ = 44100.0f, attackS_ = 0.01f, decayS_ = 0.1f, releaseS_ = 0.3f;
};

}  // namespace ddaw::dsp
