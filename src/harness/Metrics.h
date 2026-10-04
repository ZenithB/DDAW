#pragma once
// Parity metrics, mirroring synthyy's harness (ARCH 13).
#include <span>
#include <vector>

namespace ddaw::harness {

double rms(std::span<const float> x);
double toDb(double linear);  // floored at -160 dB

// 20*log10( rms(a-b) / rms(b) ) over the overlapping length. b is the reference.
// Returns -160 when a == b exactly; +inf-ish (capped at +160) if b is silent and a is not.
double rmsNullDb(std::span<const float> a, std::span<const float> b);

// Mean cosine similarity of log-magnitude spectra over Hann frames
// (default 2048 / hop 1024). Returns 0 when the signals are shorter than one frame.
double spectralSimilarity(std::span<const float> a, std::span<const float> b,
                          int fftLen = 2048, int hop = 1024);

std::vector<float> toMono(std::span<const float> l, std::span<const float> r);

// Drop the first `n` samples (golden lead-in alignment).
std::span<const float> skip(std::span<const float> x, size_t n);

bool allFinite(std::span<const float> x);

}  // namespace ddaw::harness
