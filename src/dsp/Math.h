#pragma once
// Small DSP math helpers shared by every device. Port of sf-dsp/src/util/mod.rs.
#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace ddaw::dsp {

inline float dbToLin(float db) noexcept { return std::pow(10.0f, db * 0.05f); }

// Floored at -120 dB so silence never yields -inf.
inline float linToDbFloor(float lin, float floorDb) noexcept {
    if (lin <= 0.0f) return floorDb;
    const float db = 20.0f * std::log10(lin);
    return db < floorDb ? floorDb : db;
}
inline float linToDb(float lin) noexcept { return linToDbFloor(lin, -120.0f); }

// MIDI note number to Hz (A4 = 69 = 440 Hz).
inline float midiHz(float pitch) noexcept { return 440.0f * std::pow(2.0f, (pitch - 69.0f) / 12.0f); }

// Equal-power pan: pan in [-1, 1] -> (left gain, right gain). Centre is cos(pi/4) per side.
inline std::pair<float, float> panGains(float pan) noexcept {
    const float p = std::clamp(pan, -1.0f, 1.0f);
    const float a = (p + 1.0f) * float(std::numbers::pi / 4.0);
    return {std::cos(a), std::sin(a)};
}

}  // namespace ddaw::dsp
