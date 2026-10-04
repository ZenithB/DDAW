#pragma once
// `follow`: a monophonic synthesiser played by the performance tracker (B2) - it sings what the input
// sings. Parameters live here, not in the generated synthyy schema, because the device is native to DDAW.
#include "core/Device.h"

namespace ddaw::schema {

inline constexpr ParamSpec kInstFollow[] = {
    {0, "wave", 0.0f, 3.0f, 0.0f, Curve::Stepped, 0.0f, false},          // saw, square, triangle, sine
    {1, "octave", -2.0f, 2.0f, 0.0f, Curve::Stepped, 0.0f, false},      // transposition of the tracked pitch
    {2, "glide", 1.0f, 500.0f, 30.0f, Curve::Exponential, 15.0f, false}, // ms to slide between pitches
    {3, "level", 0.0f, 1.5f, 0.8f, Curve::Linear, 15.0f, false},
    {4, "gate", -70.0f, -20.0f, -55.0f, Curve::Linear, 15.0f, false},    // loudness (dB) below which the voice closes
    {5, "release", 5.0f, 1000.0f, 120.0f, Curve::Exponential, 15.0f, false},
};

}  // namespace ddaw::schema
