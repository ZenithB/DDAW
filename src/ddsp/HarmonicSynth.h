#pragma once
// Harmonic oscillator bank, streaming, matching ddsp.synths.Harmonic (sample rate 16 kHz,
// exp_sigmoid scaling, Nyquist normalisation, Hann-window amplitude interpolation,
// linear frequency interpolation, exact phase accumulation).
//
// Frame-to-sample rules (derived from ddsp.core.resample / upsample_with_windows):
//   interval f spans samples [f*hop, (f+1)*hop). With p = sample offset in the interval,
//   f0(s)  = lerp(f0[f], f0[f+1], p/hop)                       (bilinear, last frame held)
//   amp_k(s) = A[f]*(1-w[p]) + A[f+1]*w[p], w = periodic Hann of length 2*hop
// so interval f needs frame f+1: the synth runs one frame (4 ms) behind the decoder.
#include "ddsp/Decoder.h"

namespace ddaw::ddsp {

// ddsp.core.exp_sigmoid(x): 2 * sigmoid(x)^ln(10) + 1e-7, the scaling the model's controls go through.
float expSigmoid(float x) noexcept;

struct HarmonicFrame {
    float f0Hz = 0.0f;
    float amp = 0.0f;                 // exp_sigmoid(raw amps)
    float hd[kNumHarmonics] = {};     // normalised, Nyquist-masked distribution
};

// Build one frame's synth controls from raw decoder output (126 floats) and f0.
void makeHarmonicFrame(const float* raw126, float f0Hz, int sampleRate, HarmonicFrame& out) noexcept;

class HarmonicSynth {
public:
    void prepare(int sampleRate, int hop) noexcept { sampleRate_ = sampleRate; hop_ = hop; reset(); }
    void reset() noexcept { phase_ = 0.0; }
    // Render interval [prev -> next] into out[0..hop). Real-time safe.
    void renderInterval(const HarmonicFrame& prev, const HarmonicFrame& next, float* out) noexcept;

private:
    int    sampleRate_ = 16000, hop_ = 64;
    double phase_ = 0.0;  // f0 phase in radians, wrapped to [0, 2*pi)
};

}  // namespace ddaw::ddsp
