#pragma once
// Minimal WAV I/O for the fixture harness (PCM16 and float32, mono/stereo).
#include <string>
#include <vector>

namespace ddaw::harness {

struct Audio {
    double sampleRate = 0.0;
    std::vector<float> l, r;  // equal length; mono is duplicated into both
    size_t frames() const { return l.size(); }
};

// Throws std::runtime_error on failure. Harness code only; never audio-thread.
Audio readWav(const std::string& path);
void  writeWavPcm16(const std::string& path, const Audio& a);

}  // namespace ddaw::harness
