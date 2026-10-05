#pragma once
// Parameters of the devices added after the synthyy port (B4). Hand-written - Schema.generated.h is generated from
// synthyy's registry and must not be edited. Header-only so the UI catalog can list the devices.
#include "core/Device.h"

namespace ddaw::schema {

// `fmop`: a four-operator FM/AM synth. Operators 1-4 with frequency ratios r1-r4 and levels l1-l4; `algo` picks how
// they are wired (see devices/instruments/fmop.cpp). Parameters with the last field true accept audio-rate
// modulation; their A-rate ordinals (the index into ModInputs::audioRate) are, in order: index 0, pitch 1, amp 2.
inline constexpr ParamSpec kInstFmop[] = {
    {0, "algo", 0.0f, 5.0f, 0.0f, Curve::Stepped, 0.0f, false},
    {1, "r1", 0.25f, 16.0f, 1.0f, Curve::Exponential, 15.0f, false},
    {2, "r2", 0.25f, 16.0f, 1.0f, Curve::Exponential, 15.0f, false},
    {3, "r3", 0.25f, 16.0f, 2.0f, Curve::Exponential, 15.0f, false},
    {4, "r4", 0.25f, 16.0f, 3.0f, Curve::Exponential, 15.0f, false},
    {5, "l1", 0.0f, 1.0f, 1.0f, Curve::Linear, 15.0f, false},
    {6, "l2", 0.0f, 1.0f, 0.6f, Curve::Linear, 15.0f, false},
    {7, "l3", 0.0f, 1.0f, 0.4f, Curve::Linear, 15.0f, false},
    {8, "l4", 0.0f, 1.0f, 0.3f, Curve::Linear, 15.0f, false},
    {9, "index", 0.0f, 10.0f, 2.0f, Curve::Linear, 15.0f, true},       // overall modulation index (radians per unit level)
    {10, "pitch", -24.0f, 24.0f, 0.0f, Curve::Linear, 15.0f, true},    // semitones, added to every operator
    {11, "amp", 0.0f, 1.0f, 1.0f, Curve::Linear, 15.0f, true},         // output amplitude (AM)
    {12, "fb", 0.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},         // operator 4 self-feedback
    {13, "attack", 0.001f, 2.0f, 0.005f, Curve::Exponential, 15.0f, false},
    {14, "decay", 0.01f, 4.0f, 0.4f, Curve::Exponential, 15.0f, false},
    {15, "sustain", 0.0f, 1.0f, 0.7f, Curve::Linear, 15.0f, false},
    {16, "release", 0.01f, 4.0f, 0.3f, Curve::Exponential, 15.0f, false},
    {17, "iDecay", 0.01f, 4.0f, 0.5f, Curve::Exponential, 15.0f, false},   // the modulation index falls from 1 to iSus over this
    {18, "iSus", 0.0f, 1.0f, 0.3f, Curve::Linear, 15.0f, false},
    {19, "velIdx", 0.0f, 1.0f, 0.5f, Curve::Linear, 15.0f, false},     // how much velocity scales the index
};

}  // namespace ddaw::schema
