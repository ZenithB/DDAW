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

}  // namespace ddaw::schema
