#pragma once
// Fixed-maximum interpolated delay line (port of sf-dsp/src/util/delay.rs). Allocates once in
// prepare(); write/read are real-time safe.
//   read(1) / readFrac(1.0) is the sample passed to the most recent write().
#include <algorithm>
#include <cstddef>
#include <vector>

namespace ddaw::dsp {

class DelayLine {
public:
    // May allocate. maxSamples is rounded up to the next power of two.
    void prepare(int maxSamples) {
        size_t len = 2;
        while (len < static_cast<size_t>(std::max(maxSamples, 2))) len <<= 1;
        buf_.assign(len, 0.0f);
        mask_ = len - 1;
        writePos_ = 0;
    }
    int  maxDelay() const noexcept { return static_cast<int>(buf_.size()) - 1; }
    void clear() noexcept { std::fill(buf_.begin(), buf_.end(), 0.0f); writePos_ = 0; }
    void write(float x) noexcept { buf_[writePos_] = x; writePos_ = (writePos_ + 1) & mask_; }

    // Linearly interpolated read, `delay` samples behind the latest write. Clamped to [1, maxDelay].
    float readFrac(float delay) const noexcept {
        const float d = std::clamp(delay, 1.0f, static_cast<float>(maxDelay()));
        const size_t di = static_cast<size_t>(d);
        const float frac = d - static_cast<float>(di);
        const size_t i0 = (writePos_ + buf_.size() - di) & mask_;
        const size_t i1 = (i0 + buf_.size() - 1) & mask_;
        return buf_[i0] + (buf_[i1] - buf_[i0]) * frac;
    }
    // Integer-delay read, no interpolation. Clamped to [1, maxDelay].
    float read(int delay) const noexcept {
        const size_t d = static_cast<size_t>(std::clamp(delay, 1, maxDelay()));
        return buf_[(writePos_ + buf_.size() - d) & mask_];
    }

private:
    std::vector<float> buf_;
    size_t mask_ = 1, writePos_ = 0;
};

}  // namespace ddaw::dsp
