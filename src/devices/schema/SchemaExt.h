#pragma once
// Parameters added to ported devices after the synthyy port. Hand-written - Schema.generated.h is generated from synthyy's
// registry and must not be edited; a table here starts with the generated entries unchanged (the same keys, ranges and
// defaults, so projects and fixtures stay valid) and appends. Header-only so the UI catalog can list them.
#include "core/Device.h"

namespace ddaw::schema {

// `autotune`: the generated four, plus a key of its own. `root` 0..11 (C..B) and `scale` (dsp/Scales.h index) override the
// project's key for this device; -1 (the default) follows the project.
inline constexpr ParamSpec kFxAutotuneKey[] = {
    {0, "amount", 0.0f, 1.0f, 1.0f, Curve::Linear, 15.0f, false},
    {1, "speed", 1.0f, 200.0f, 20.0f, Curve::Exponential, 15.0f, false},
    {2, "mix", 0.0f, 1.0f, 1.0f, Curve::Linear, 15.0f, false},
    {3, "mode", 0.0f, 1.0f, 0.0f, Curve::Stepped, 0.0f, false},
    {4, "root", -1.0f, 11.0f, -1.0f, Curve::Stepped, 0.0f, false},
    {5, "scale", -1.0f, 7.0f, -1.0f, Curve::Stepped, 0.0f, false},
};

// `chordres` (Chord Resonator): steep, high-Q band-pass filters tuned to equal-tempered intervals above the note being played
// (the tonic), boosted and compressed into a chord. Intervals are semitones from the tonic, -12..+36; levels in dB.
//   source    0 Auto (the track's notes when it has an instrument playing them, else the audio), 1 Notes, 2 Audio (FFT pitch)
//   filters   how many of i1..i6 are active (1..6);  tonic: an extra resonance on the tonic itself, level `tonicLevel`
//   boost     how hard the resonances are boosted (dB); more boost lowers the original a little and compresses each resonance harder
//   q, steep  bandwidth (Q) and skirt steepness (stepped: 12, 24, 36, 48 dB/oct)
//   excite    light harmonic distortion ahead of the filters, which gives them harmonics to ring on
//   sat       a saturation-and-compression stage at the end of the chain (0 = off, bit-exact)
//   mix       the original against the resonated sound;  glide: how fast the filters follow a new tonic (ms)
//   thresh    audio mode: the level (dB) below which the pitch tracker holds the last tonic
inline constexpr ParamSpec kFxChordres[] = {
    {0, "source", 0.0f, 2.0f, 0.0f, Curve::Stepped, 0.0f, false},
    {1, "filters", 1.0f, 6.0f, 3.0f, Curve::Stepped, 0.0f, false},
    {2, "tonic", 0.0f, 1.0f, 0.0f, Curve::Stepped, 0.0f, false},
    {3, "tonicLevel", -18.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {4, "i1", -12.0f, 36.0f, 4.0f, Curve::Stepped, 0.0f, false},
    {5, "i2", -12.0f, 36.0f, 7.0f, Curve::Stepped, 0.0f, false},
    {6, "i3", -12.0f, 36.0f, 11.0f, Curve::Stepped, 0.0f, false},
    {7, "i4", -12.0f, 36.0f, 12.0f, Curve::Stepped, 0.0f, false},
    {8, "i5", -12.0f, 36.0f, 16.0f, Curve::Stepped, 0.0f, false},
    {9, "i6", -12.0f, 36.0f, 19.0f, Curve::Stepped, 0.0f, false},
    {10, "l1", -18.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {11, "l2", -18.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {12, "l3", -18.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {13, "l4", -18.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {14, "l5", -18.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {15, "l6", -18.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {16, "boost", 0.0f, 36.0f, 12.0f, Curve::Linear, 15.0f, false},
    {17, "q", 2.0f, 500.0f, 40.0f, Curve::Exponential, 15.0f, false},
    {18, "steep", 0.0f, 3.0f, 1.0f, Curve::Stepped, 0.0f, false},
    {19, "excite", 0.0f, 1.0f, 0.3f, Curve::Linear, 15.0f, false},
    {20, "sat", 0.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},
    {21, "mix", 0.0f, 1.0f, 1.0f, Curve::Linear, 15.0f, false},
    {22, "glide", 1.0f, 500.0f, 25.0f, Curve::Exponential, 15.0f, false},
    {23, "thresh", -80.0f, -20.0f, -55.0f, Curve::Linear, 15.0f, false},
};

}  // namespace ddaw::schema
