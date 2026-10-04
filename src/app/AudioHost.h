#pragma once
// Device manager glue: owns the JUCE AudioDeviceManager and runs the Engine from its callback.
// The callback allocates nothing, takes no locks, and flushes denormals (ARCH 1).
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h>

#include "engine/Engine.h"
#include "engine/Graph.h"

namespace ddaw::app {

struct HostStats {
    uint64_t callbacks = 0;
    double   meanProcessUs = 0, maxProcessUs = 0;
    double   maxBudgetFraction = 0;       // worst callback: process time / buffer duration
    uint64_t overBudget = 0;              // callbacks where process time exceeded the buffer duration
    uint64_t lateCallbacks = 0;           // inter-callback gap > 1.5 x the buffer duration
    int      deviceXRuns = -1;            // from the driver when it reports them, else -1
    int      bufferSize = 0;
    double   sampleRate = 0;
};

class AudioHost final : public juce::AudioIODeviceCallback {
public:
    using GraphFactory = std::function<std::unique_ptr<engine::Graph>(double sampleRate)>;

    // `factory` builds the graph for the device's actual sample rate; it runs on the thread that
    // starts the device (never the audio thread).
    explicit AudioHost(GraphFactory factory);
    ~AudioHost() override;

    juce::AudioDeviceManager& deviceManager() { return dm_; }
    engine::Engine& engine() { return engine_; }

    // Which device input channels feed the engine's input: both as a stereo pair, or one as mono.
    enum class InputMode : int { Stereo = 0, Left = 1, Right = 2 };
    void setInputMode(InputMode m) { inputMode_.store(int(m)); }
    InputMode inputMode() const { return InputMode(inputMode_.load()); }
    // Opens (or closes) the device's input channels. The first call prompts for microphone access on
    // macOS, so it is made only when a track is armed. Reopens the device (see onDeviceStarted).
    juce::String setInputEnabled(bool on);
    bool inputEnabled() const { return numIn_.load() > 0; }
    int inputChannels() const { return numIn_.load(); }
    // Device-reported latencies in frames (0 until a device is open).
    int inputLatencyFrames() const { return inLat_.load(); }
    int outputLatencyFrames() const { return outLat_.load(); }
    // Called on the message thread after every (re)start of the device, with its sample rate. The Engine
    // has been re-prepared and holds the factory's empty graph: the owner must rebuild and resubmit.
    std::function<void(double)> onDeviceStarted;

    // Opens the default output (no inputs until setInputEnabled). Returns an error string, empty on success.
    juce::String start(int bufferSize = 0, double sampleRate = 0.0);
    void stop();
    HostStats stats() const;

    void audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut,
                                          int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart(juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

private:
    GraphFactory factory_;
    engine::Engine engine_;
    juce::AudioDeviceManager dm_;
    std::vector<float> scratch_;  // second channel for mono devices, and a sink for extra channels
    std::atomic<uint64_t> callbacks_{0}, overBudget_{0}, late_{0};
    std::atomic<double>   sumUs_{0.0}, maxUs_{0.0}, maxFrac_{0.0};
    std::atomic<int64_t>  lastStartNs_{0};
    std::atomic<int>      bufferSize_{0}, inputMode_{0}, numIn_{0}, inLat_{0}, outLat_{0};
    std::atomic<double>   sampleRate_{0.0};
    juce::AudioIODevice*  device_ = nullptr;
};

}  // namespace ddaw::app
