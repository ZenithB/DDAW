#pragma once
// YIN pitch estimator (de Cheveigne and Kawahara, 2002) on a block of samples: difference function,
// cumulative mean normalisation, absolute threshold, parabolic refinement. Allocation-free after prepare().
#include <vector>

namespace ddaw::dsp {

struct YinResult {
    float f0Hz = 0.0f;         // 0 when no period was found
    float confidence = 0.0f;   // 1 - the normalised difference at the chosen lag (0..1)
};

class Yin {
public:
    // sr: the analysis rate; the estimator looks for periods between maxHz and minHz.
    void prepare(double sr, float minHz, float maxHz, float threshold = 0.15f);
    // Samples needed in the window: twice the longest period. The estimate belongs to the middle.
    int windowSamples() const noexcept { return 2 * tauMax_; }
    int latencySamples() const noexcept { return tauMax_; }        // analysis window / 2
    // `x` points at windowSamples() contiguous samples.
    YinResult estimate(const float* x) noexcept;

private:
    double sr_ = 16000.0;
    int tauMin_ = 2, tauMax_ = 100;
    float threshold_ = 0.15f;
    std::vector<float> d_, cmnd_;
};

}  // namespace ddaw::dsp
