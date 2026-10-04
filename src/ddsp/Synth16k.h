#pragma once
// The whole DDSP solo-instrument synthesis chain at the model rate (16 kHz), streaming:
//   harmonic oscillator bank + filtered noise  ->  add  ->  learned reverb (FIR, 48000 taps, dry kept)
// exactly as the Magenta checkpoints' processor graph (ProcessorGroup dag: filtered_noise, harmonic, add,
// reverb). Frames arrive one per 64 samples; output block g is produced when frame g+1 has arrived (the
// harmonic interpolation and the noise filter's overlap both need it), so the chain runs one frame behind.
// Real-time safe after prepare(): no allocation, locks or exceptions.
#include <vector>

#include "ddsp/Decoder.h"
#include "ddsp/HarmonicSynth.h"
#include "dsp/Fft.h"

namespace ddaw::ddsp {

// FilteredNoise with window_size = 0: each frame's 65 magnitudes become a 128-tap Hann-windowed zero-phase
// FIR (frequency sampling), applied to that frame's 64 noise samples; blocks overlap-add; the output is
// advanced by the filter's delay compensation (62 samples), as ddsp.core.fft_convolve does.
class NoiseFilter {
public:
    static constexpr int kBins = kNumNoise, kTaps = 2 * (kNumNoise - 1), kHop = kHopSamples;
    void prepare();
    void reset();
    // Add frame f: its raw (pre-scale) noise magnitudes and the 64 white-noise samples of its block.
    void push(const float* rawMags65, const float* noise64) noexcept;
    // The block that frame f+1's arrival completed (call once after every push but the first).
    void pop(float* out64) noexcept;

private:
    std::vector<float> cosTab_, window_, ola_;   // cos[k][n], zero-phase window, ring of 256
    std::vector<float> ir_;
    int frame_ = 0;                               // frames pushed
    int readBlock_ = 0;                           // blocks popped
};

// Uniform-partition FFT convolution of 64-sample blocks with a long impulse response; the dry signal
// is added, the first tap masked (ddsp.effects.Reverb: ir[0] = 0, add_dry = True).
class ReverbConv {
public:
    void prepare(const float* ir, size_t length);
    void reset();
    void process(const float* in64, float* out64) noexcept;   // out may alias in
    bool ready() const noexcept { return parts_ > 0; }

private:
    static constexpr int kB = kHopSamples, kN = 2 * kHopSamples, kBinsN = kN / 2 + 1;
    dsp::Fft fft_;
    size_t parts_ = 0, head_ = 0;
    std::vector<double> H_, X_;                   // [parts][bins][re,im]; X_ is the frequency-domain delay line
    std::vector<double> prev_, re_, im_, accRe_, accIm_;
};

class Synth16k {
public:
    // Allocates. `ir` is the model's reverb impulse response (null/0: no reverb).
    void prepare(const float* reverbIr, size_t irLen);
    void reset();
    void setReverb(bool on) noexcept { reverbOn_ = on; }
    // Replace the reverb with one prepared elsewhere (a model change): returns the previous one, which the
    // caller must not free on the audio thread. The Synth does not own an installed reverb.
    ReverbConv* swapReverb(ReverbConv* r) noexcept { ReverbConv* old = reverb_; reverb_ = r; return old; }
    void setNoiseGain(float g) noexcept { noiseGain_ = g; }
    // Push frame f. Returns true and fills out64 for interval f-1 once a previous frame exists.
    // noise64: the 64 white samples (uniform -1..1) of this frame's block.
    bool push(const float* raw126, float f0Hz, const float* noise64, float* out64) noexcept;

private:
    HarmonicSynth harm_;
    NoiseFilter noise_;
    ReverbConv ownReverb_;       // used by prepare(ir, len)
    ReverbConv* reverb_ = nullptr;
    HarmonicFrame prev_, cur_;
    bool havePrev_ = false, reverbOn_ = true;
    float noiseGain_ = 1.0f;
    std::vector<float> h_, n_;
};

}  // namespace ddaw::ddsp
