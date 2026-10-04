#pragma once
// Lookahead brickwall limiter for the master bus. Stereo-linked.
//
// Guarantee: |output| <= ceiling for every sample, whatever the input.
// Method: per-sample target gain g_t = min(1, ceiling/peak); an exponential release is applied
// to it (the result never exceeds g_t); a sliding minimum over the last L+1 samples and a boxcar
// mean of the same length turn that into a gain that is already at or below g_t(n-L) when the
// audio sample x(n-L) reaches the output. Latency is L samples, reported to the graph.
#include <array>
#include <vector>

namespace ddaw::dsp {

class Limiter {
public:
    static constexpr int kMaxLookahead = 256;

    // May allocate.
    void prepare(double sampleRate, float ceilingDb = -1.0f, float lookaheadMs = 1.5f, float releaseMs = 50.0f);
    void reset() noexcept;
    int latencySamples() const noexcept { return L_; }
    float gainReductionDb() const noexcept { return grDb_; }  // most recent block, <= 0

    // In place, planar stereo. Real-time safe.
    void process(float* l, float* r, int n) noexcept;

private:
    int    L_ = 72;
    float  ceiling_ = 0.891f, relCoef_ = 0.0f, grDb_ = 0.0f;
    float  gRel_ = 1.0f;                         // release-smoothed target gain
    std::vector<float> dl_, dr_;                 // audio delay, size L+1 ring
    std::array<float, kMaxLookahead + 1> gmin_{};  // ring of release-smoothed target gains
    std::array<float, kMaxLookahead + 1> gminOut_{};  // ring of min-filtered gains
    int    pos_ = 0, sinceSync_ = 0;
    double boxSum_ = 0.0;                        // running sum of gminOut_ over the last L+1 samples
};

}  // namespace ddaw::dsp
