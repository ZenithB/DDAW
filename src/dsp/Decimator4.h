#pragma once
// Streaming 4x -> 1x decimator for oversampled synthesis: the down half of Oversampler4 (an 11-tap halfband
// at 2x, then a 23-tap halfband at the base rate), one output sample per four input samples. A device that
// generates audio at four times the host rate (FM/AM operators, whose sidebands would otherwise fold back)
// runs this once on its summed output. Allocation-free after construction. Group delay: see latency().
#include <cmath>

#include "dsp/Oversampler4.h"

namespace ddaw::dsp {

class Decimator4 {
public:
    // Tap counts are odd; the defaults match Oversampler4. Longer filters reject more near the Nyquist (and delay more).
    explicit Decimator4(int sharpTaps = Oversampler4::kSharpTaps, int relaxTaps = Oversampler4::kRelaxTaps)
        : dn2_(relaxTaps), dn1_(sharpTaps), latency_(int(std::lround((sharpTaps - 1) / 4.0 + (relaxTaps - 1) / 8.0))) {}
    void reset() { dn2_.reset(); dn1_.reset(); }
    // Four consecutive high-rate samples in, one base-rate sample out.
    float push(float a, float b, float c, float d) noexcept {
        const float m0 = dn2_.pushPair(a, b);
        const float m1 = dn2_.pushPair(c, d);
        return dn1_.pushPair(m0, m1);
    }
    // Delay in base-rate samples: the peak of the impulse response.
    int latency() const noexcept { return latency_; }

private:
    detail::Down2 dn2_, dn1_;
    int latency_;
};

}  // namespace ddaw::dsp
