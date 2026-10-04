#pragma once
// PolyInput: polyphonic audio -> notes. A service thread reads the engine's mono input copy, resamples it to
// 16 kHz, and every 32 ms asks the multi-pitch estimator which notes are sounding; a note tracker turns that
// into note-on / note-off, and those go to the engine's live-note path, so a played chord becomes several notes
// on the live target track (and can be recorded like any live notes). Latency is the analysis window: about
// 130 ms. Not real-time: the audio thread only copies samples into a ring.
#include <atomic>
#include <thread>

#include "dsp/PolyPitch.h"
#include "engine/Engine.h"

namespace ddaw::engine {

class PolyInput {
public:
    explicit PolyInput(Engine& e) : engine_(e) {}
    ~PolyInput() { stop(); }
    PolyInput(const PolyInput&) = delete;
    PolyInput& operator=(const PolyInput&) = delete;

    void start(const dsp::PolyPitchConfig& cfg = {});
    void stop();                 // releases every note it is holding
    bool running() const { return running_.load(); }
    // Host frames from a sound starting to its note-on (window / 2 + the confirmation frames + the resampler).
    int latencyFrames() const { return latency_; }
    uint64_t framesAnalysed() const { return analysed_.load(); }
    uint64_t notesStarted() const { return started_.load(); }

private:
    void loop(dsp::PolyPitchConfig cfg);
    Engine& engine_;
    std::thread thread_;
    std::atomic<bool> running_{false}, stop_{false};
    std::atomic<uint64_t> analysed_{0}, started_{0};
    int latency_ = 0;
};

}  // namespace ddaw::engine
