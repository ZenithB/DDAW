#pragma once
#include <array>

#include "core/Device.h"

namespace ddaw {

// M0 stub instrument: polyphonic sine, fixed 16-voice pool, oldest-voice
// stealing, 64-sample linear release. Reference spec for the independent
// golden generator (tests/fixtures/make_stub_golden.py).
class StubTone final : public InstrumentDevice {
public:
    enum Param : uint16_t { Level = 0 };
    static constexpr int kVoices = 16;
    static constexpr int kReleaseSamples = 64;

    std::span<const ParamSpec> params() const override;
    void prepare(double sampleRate, int maxBlock) override;
    void setParam(uint16_t index, float value) override;
    void noteOn(uint8_t pitch, float velocity, uint32_t noteId) override;
    void noteOff(uint32_t noteId) override;
    void process(float* l, float* r, int numFrames,
                 const ProcessContext&, const ModInputs&) override;
    void reset() override;

private:
    struct Voice {
        bool     active = false, releasing = false;
        uint32_t noteId = 0;
        uint64_t age = 0;
        double   phase = 0, inc = 0;
        float    amp = 0;
        int      relPos = 0;
    };
    std::array<Voice, kVoices> voices_{};
    double   sampleRate_ = 44100;
    float    level_ = 0.25f;
    uint64_t counter_ = 0;
};

}  // namespace ddaw
