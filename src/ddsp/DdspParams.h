#pragma once
// Parameters of the `ddsp` instrument (the Magenta solo-instrument models). Header-only so the UI catalog can
// list the device without linking the neural runtime.
#include "core/Device.h"

namespace ddaw::schema {

inline constexpr ParamSpec kInstDdsp[] = {
    {0, "model", 0.0f, 3.0f, 0.0f, Curve::Stepped, 0.0f, false},          // violin, flute, tenor sax, trumpet
    {1, "transpose", -24.0f, 24.0f, 0.0f, Curve::Stepped, 0.0f, false},   // semitones, applied to tracked pitch and to notes
    {2, "level", -40.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},       // dB
    {3, "reverb", 0.0f, 1.0f, 1.0f, Curve::Stepped, 0.0f, false},         // the model's learned room
    {4, "noise", 0.0f, 2.0f, 1.0f, Curve::Linear, 15.0f, false},          // breath / bow noise amount
    {5, "attack", 5.0f, 500.0f, 40.0f, Curve::Exponential, 15.0f, false}, // ms, notes
    {6, "release", 20.0f, 2000.0f, 250.0f, Curve::Exponential, 15.0f, false}, // ms, notes
    {7, "vibrato", 0.0f, 100.0f, 12.0f, Curve::Linear, 15.0f, false},     // cents, notes
};

}  // namespace ddaw::schema
