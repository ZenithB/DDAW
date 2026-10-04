#pragma once
// Recorder service (ARCH 1: "recording writer", a non-RT service). Starts a take on the Engine's input
// capture, drains the capture ring on a writer thread into a float32 WAV (so a long take never
// accumulates in memory), and finishes it on stop. Control thread only; the audio thread is untouched
// apart from the Engine's own atomics and the ring.
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

#include "engine/Engine.h"

namespace ddaw::engine {

class Recorder {
public:
    explicit Recorder(Engine& e) : engine_(e) {}
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    struct Take {
        bool ok = false;
        std::string path, error;
        double startTick = 0;      // timeline position of the first recorded frame (before latency compensation)
        uint64_t frames = 0;
        double sampleRate = 0;
        uint64_t dropped = 0;      // frames lost because the writer fell behind (a take with drops is unreliable)
        int channels = 2;
    };

    // Opens `path` and asks the engine to capture. The take begins at the first callback in which the
    // transport is playing. `channels` is 1 (left only) or 2.
    bool start(const std::string& path, int channels, double sampleRate, std::string& error);
    bool recording() const { return running_.load(); }
    // True once the engine has actually begun capturing (the transport started).
    bool capturing() const { return engine_.captureInfo().active; }
    uint64_t framesRecorded() const { return engine_.captureInfo().frames; }

    // Stops the take: waits for the engine to acknowledge, drains the ring, finalises the WAV.
    Take stop(int timeoutMs = 3000);

private:
    void writerLoop();
    void drainOnce(bool& any);

    Engine& engine_;
    std::thread writer_;
    std::atomic<bool> running_{false}, finishing_{false};
    FILE* file_ = nullptr;
    std::string path_;
    int channels_ = 2;
    double sr_ = 48000.0;
    uint64_t written_ = 0;
    std::vector<float> bufL_, bufR_, inter_;
};

}  // namespace ddaw::engine
