#pragma once
// Loudness exactly as Magenta's DDSP computes it for its training features (ddsp.spectral_ops.
// compute_loudness, v3.7): 16 kHz audio, 512-point Hann-windowed FFT every 64 samples (250 frames per
// second, frames centred on k*64 with zero padding before the start), A-weighted power averaged over the
// 257 bins, converted to dB with an 80 dB floor. The decoder's inputs depend on this being identical.
#include <vector>

#include "dsp/Fft.h"

namespace ddaw::dsp {

class LoudnessFrame {
public:
    static constexpr int kFft = 512, kHop = 64, kRangeDb = 80;
    void prepare();
    // `x` points at kFft contiguous samples centred on the frame time (x[256] is the centre sample).
    float compute(const float* x) noexcept;

private:
    Fft fft_;
    std::vector<double> window_, weight_, re_, im_;
};

}  // namespace ddaw::dsp
