#include "devices/effects/stub_gain.h"

#include <cmath>

#include "core/Constants.h"

namespace ddaw {

namespace {
constexpr ParamSpec kParams[] = {
    {StubGain::Gain, "gain", 0.0f, 2.0f, 1.0f, Curve::Linear,
     static_cast<float>(kDefaultSmoothingMs), false},
};
}

std::span<const ParamSpec> StubGain::params() const { return kParams; }

void StubGain::prepare(double sampleRate, int) {
    coeff_ = 1.0f - std::exp(-1.0f / (static_cast<float>(kDefaultSmoothingMs) * 0.001f *
                                      static_cast<float>(sampleRate)));
    reset();
}

void StubGain::setParam(uint16_t index, float value) {
    if (index == Gain) target_ = value;
}

void StubGain::process(float* l, float* r, int numFrames,
                       const ProcessContext&, const ModInputs&) {
    for (int i = 0; i < numFrames; ++i) {
        current_ += coeff_ * (target_ - current_);
        l[i] *= current_;
        r[i] *= current_;
    }
}

void StubGain::reset() { current_ = target_; }

}  // namespace ddaw
