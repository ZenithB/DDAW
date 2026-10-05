#pragma once
// Per-note expression state for a voice (MPE): the three dimensions of InstrumentDevice::noteExpression - 0 slide (0..1),
// 1 pressure (0..1), 2 pitch bend in semitones - as set by the controller and as heard (smoothed, so a controller's steps do
// not zipper). A neutral voice (all zero) costs one compare per sample and changes nothing about the sound.
#include <algorithm>
#include <cmath>

namespace ddaw::dsp {

struct NoteExpr {
    float bend = 0.0f, slide = 0.0f, pressure = 0.0f;      // as set
    float bendS = 0.0f, slideS = 0.0f, pressS = 0.0f;      // as heard

    void clear() { bend = slide = pressure = bendS = slideS = pressS = 0.0f; }
    void set(int dimension, float v) noexcept {
        if (dimension == 0) slide = std::clamp(v, 0.0f, 1.0f);
        else if (dimension == 1) pressure = std::clamp(v, 0.0f, 1.0f);
        else if (dimension == 2) bend = std::clamp(v, -96.0f, 96.0f);
    }
    // One smoothing step (k per step: 0.012 is about 2 ms per sample at 44.1 kHz). Settles exactly.
    void step(float k = 0.012f) noexcept {
        glide(bendS, bend, k); glide(slideS, slide, k); glide(pressS, pressure, k);
    }
    float pitchFactor() const noexcept { return bendS == 0.0f ? 1.0f : std::exp2(bendS * (1.0f / 12.0f)); }
    float gain() const noexcept { return 1.0f + 0.5f * pressS; }   // pressure: up to +50% level
    bool neutral() const noexcept { return bendS == 0.0f && slideS == 0.0f && pressS == 0.0f; }

private:
    static void glide(float& s, float target, float k) noexcept {
        s += (target - s) * k;
        if (std::abs(target - s) < 1e-6f) s = target;
    }
};

}  // namespace ddaw::dsp
