#pragma once
#include "core/Device.h"

namespace ddaw {

// M0 stub effect: smoothed linear gain. Exists to prove the harness end to end.
class StubGain final : public EffectDevice {
public:
    enum Param : uint16_t { Gain = 0 };

    std::span<const ParamSpec> params() const override;
    void prepare(double sampleRate, int maxBlock) override;
    void setParam(uint16_t index, float value) override;
    void process(float* l, float* r, int numFrames,
                 const ProcessContext&, const ModInputs&) override;
    void reset() override;

private:
    float target_  = 1.0f;
    float current_ = 1.0f;
    float coeff_   = 0.0f;  // one-pole smoothing coefficient
};

}  // namespace ddaw
