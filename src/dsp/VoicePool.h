#pragma once
// Voice choice for the fixed-pool instruments: a voice still held on the same pitch is reused (a retrigger), else a free
// voice, else the oldest. V needs `held`, `pitch`, `serial` and `active()`.
#include <array>
#include <cstdint>

namespace ddaw::dsp {

template <class V, size_t N>
int pickVoice(const std::array<V, N>& voices, uint8_t pitch) noexcept {
    int same = -1, freeV = -1, oldest = 0;
    uint64_t oldestSerial = UINT64_MAX;
    for (size_t i = 0; i < N; ++i) {
        const V& v = voices[i];
        if (v.held && v.pitch == pitch) { same = int(i); break; }
        if (!v.active() && freeV < 0) freeV = int(i);
        if (v.serial < oldestSerial) { oldestSerial = v.serial; oldest = int(i); }
    }
    return same >= 0 ? same : (freeV >= 0 ? freeV : oldest);
}

}  // namespace ddaw::dsp
