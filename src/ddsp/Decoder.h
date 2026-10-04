#pragma once
// Magenta DDSP solo-instrument decoder (RnnFcDecoder, ld+f0 inputs, GRU 512).
// One frame in, 126 raw controls out: amps(1) | harmonic_distribution(60) | noise_magnitudes(65).
// All four pretrained instruments share this architecture (checked at export).
#include <memory>
#include <string>

namespace ddaw::ddsp {

constexpr int kHidden = 512;
constexpr int kNumAmps = 1, kNumHarmonics = 60, kNumNoise = 65;
constexpr int kDecoderOut = kNumAmps + kNumHarmonics + kNumNoise;  // 126
constexpr int kModelSampleRate = 16000, kFrameRate = 250, kHopSamples = kModelSampleRate / kFrameRate;  // 64

// Input scaling used by the model's F0LoudnessPreprocessor.
float scaleLoudnessDb(float db);  // db/80 + 1
float scaleF0Hz(float hz);        // hz_to_midi(hz)/127, 0 Hz -> 0

class Decoder {
public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Non-real-time: allocates and throws. Loads a .ddspw file and zeroes the GRU state.
    void load(const std::string& weightsPath);
    bool loaded() const;

    // Real-time safe (no allocation, locks or exceptions): clears the GRU state.
    void reset() noexcept;
    // Real-time safe: one 250 Hz frame. `out` receives kDecoderOut floats.
    // Inference never runs on the audio thread (PLAN 1); this is called on the inference thread.
    void step(float ldScaled, float f0Scaled, float* out) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ddaw::ddsp
