#include "devices/instruments/stub_tone.h"

#include <cmath>
#include <numbers>

namespace ddaw {

namespace {
constexpr ParamSpec kParams[] = {
    {StubTone::Level, "level", 0.0f, 1.0f, 0.25f, Curve::Linear, 0.0f, false},
};
}

std::span<const ParamSpec> StubTone::params() const { return kParams; }

void StubTone::prepare(double sampleRate, int) {
    sampleRate_ = sampleRate;
    reset();
}

void StubTone::setParam(uint16_t index, float value) {
    if (index == Level) level_ = value;
}

void StubTone::noteOn(uint8_t pitch, float velocity, uint32_t noteId) {
    Voice* v = nullptr;
    for (auto& c : voices_) if (!c.active) { v = &c; break; }
    if (!v) {  // steal oldest
        v = &voices_[0];
        for (auto& c : voices_) if (c.age < v->age) v = &c;
    }
    const double hz = 440.0 * std::pow(2.0, (double(pitch) - 69.0) / 12.0);
    *v = Voice{};
    v->active = true;
    v->noteId = noteId;
    v->age = ++counter_;
    v->inc = 2.0 * std::numbers::pi * hz / sampleRate_;
    v->amp = velocity * level_;
}

void StubTone::noteOff(uint32_t noteId) {
    for (auto& v : voices_)
        if (v.active && !v.releasing && v.noteId == noteId) v.releasing = true;
}

void StubTone::process(float* l, float* r, int numFrames, const ProcessContext&, const ModInputs&) {
    for (auto& v : voices_) {
        if (!v.active) continue;
        for (int i = 0; i < numFrames; ++i) {
            float env = 1.0f;
            if (v.releasing) {
                env = 1.0f - float(v.relPos) / float(kReleaseSamples);
                if (++v.relPos > kReleaseSamples) { v.active = false; break; }
            }
            const float s = v.amp * env * static_cast<float>(std::sin(v.phase));
            v.phase += v.inc;
            l[i] += s;
            r[i] += s;
        }
    }
}

void StubTone::reset() {
    for (auto& v : voices_) v = Voice{};
    counter_ = 0;
}

}  // namespace ddaw
