#pragma once
// Multi-pitch estimation (several simultaneous notes) from a window of audio at 16 kHz, in the spirit of
// iterative spectral-peak salience methods (Klapuri): peak-pick a Hann-windowed spectrum, score every
// fundamental hypothesis by how many of its harmonics are present, take the best, weaken the partials it
// explains, repeat. Plus a note tracker that turns a stream of such frames into note on / off events.
// Not real-time (it allocates and runs on its own thread).
#include <vector>

#include "dsp/Fft.h"

namespace ddaw::dsp {

struct PolyCandidate {
    float midi = 0.0f;       // fractional MIDI note number
    float hz = 0.0f;
    float salience = 0.0f;   // relative to the strongest candidate of the frame (1 = strongest)
    float level = 0.0f;      // amplitude of the fundamental's partial group (linear)
};

struct PolyPitchConfig {
    int window = 2048;            // samples analysed per frame (128 ms at 16 kHz); 4096 resolves closer low notes
    int maxVoices = 6;
    float minHz = 65.0f, maxHz = 1500.0f;
    float floorDb = -48.0f;       // peaks more than this far below the loudest are ignored
    float minSalience = 0.30f;    // a further note must score at least this fraction of the first
    int harmonics = 10;
};

class PolyPitch {
public:
    void prepare(const PolyPitchConfig& cfg = {});
    const PolyPitchConfig& config() const { return cfg_; }
    // `x` points at config().window samples. Returns the notes found, strongest first.
    std::vector<PolyCandidate> estimate(const float* x);

private:
    PolyPitchConfig cfg_;
    Fft fft_;
    int fftSize_ = 8192;
    std::vector<double> win_, re_, im_;
};

struct PolyNoteEvent {
    int pitch = 0;
    bool on = false;
    float velocity = 0.0f;
};

// Hysteresis over frames: a note starts when it was found in `confirm` of the last frames and ends after it
// has been missing for `release` frames. Pitches are rounded to the nearest semitone (within 50 cents).
class PolyNoteTracker {
public:
    void configure(int confirmFrames = 2, int releaseFrames = 3) { confirm_ = confirmFrames; release_ = releaseFrames; }
    void reset();
    // Feed one frame's candidates; appends the events it caused.
    void update(const std::vector<PolyCandidate>& found, std::vector<PolyNoteEvent>& events);
    bool active(int pitch) const { return pitch >= 0 && pitch < 128 && state_[pitch].active; }

private:
    struct S { int seen = 0, missing = 0; bool active = false; float level = 0; };
    S state_[128];
    int confirm_ = 2, release_ = 3;
};

}  // namespace ddaw::dsp
