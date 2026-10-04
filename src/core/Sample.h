#pragma once
// Decoded PCM sample storage shared between the control side (SampleBank) and the sample-based
// instruments (port of sf-dsp sample.rs). Buffers are immutable once built and handed to devices as
// shared pointers on the control path; the audio thread only ever reads.
#include <cstddef>
#include <memory>
#include <vector>

namespace ddaw {

// Planar float PCM at the SOURCE sample rate (devices resample on read). `r` empty means mono:
// readers use the left channel for both sides.
struct SampleBuf {
    float sampleRate = 44100.0f;
    std::vector<float> l, r;

    size_t frames() const noexcept { return l.size(); }  // the left channel is authoritative
    double duration() const noexcept { return sampleRate > 0.0f ? double(l.size()) / double(sampleRate) : 0.0; }

    // Linear-interpolated stereo read at fractional frame `pos`. Out-of-range positions read as silence.
    // Real-time safe: never throws, never allocates.
    void readLin(double pos, float& outL, float& outR) const noexcept {
        const size_t n = l.size();
        if (n == 0 || pos < 0.0) { outL = outR = 0.0f; return; }
        const size_t i = static_cast<size_t>(pos);
        if (i >= n) { outL = outR = 0.0f; return; }
        const float frac = static_cast<float>(pos - double(i));
        const size_t i2 = i + 1 < n ? i + 1 : i;
        outL = l[i] + (l[i2] - l[i]) * frac;
        outR = r.empty() ? outL : r[i] + (r[i2] - r[i]) * frac;
    }
};

using SamplePtr = std::shared_ptr<const SampleBuf>;

}  // namespace ddaw
