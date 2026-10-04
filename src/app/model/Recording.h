#pragma once
// Recording workflow (JUCE-free): which tracks are armed, starting a take (with an optional count-in),
// and turning the finished take into a latency-compensated audio clip on every armed audio track, as a
// single undoable edit. The audio thread side is Engine::processIO + engine::Recorder.
#include <functional>
#include <set>
#include <vector>
#include <string>

#include "engine/Recorder.h"
#include "project/Project.h"

namespace ddaw::app {

class AppModel;

struct RecordingSettings {
    double offsetMs = 0.0;     // extra compensation for interfaces that misreport their latency (or Bluetooth)
    int countInBars = 0;       // 0..2: the transport starts this many bars before the playhead
    bool monitor = false;      // hear the input through the master while armed
    bool recordNotes = true;   // armed synth / drum tracks record the notes played live (MIDI, keyboard)
    bool recordCurves = false; // while recording, write the performance routes marked `record` into automation lanes
};

// What the host (audio device) provides; all may be left empty in tests.
struct RecordingEnv {
    std::function<std::string()> openInput;        // open the device input; returns an error ("" = ok)
    std::function<int()> inputLatencyFrames;
    std::function<int()> outputLatencyFrames;
    std::function<int()> inputChannels;            // 1 (mono: left only is recorded) or 2
    std::function<double()> sampleRate;
    std::function<int()> inputMode;                // 0 stereo pair, 1 left, 2 right
    std::function<void(int)> setInputMode;
    std::function<std::string()> deviceInfo;       // a line for the options panel (device, latencies)
};

class RecordingController {
public:
    RecordingController(AppModel& m, engine::Engine& e) : model_(m), engine_(e), recorder_(e) {}
    void setEnv(RecordingEnv env) { env_ = std::move(env); }
    const RecordingEnv& env() const { return env_; }
    // Hear the input through each armed track's effects (the monitoring path of the engine, not a bypass).
    void setMonitor(bool on);
    RecordingSettings& settings() { return settings_; }
    const RecordingSettings& settings() const { return settings_; }

    // ---- arming: audio tracks record sound, synth and drum tracks record the notes played live ----
    void arm(project::Uid track, bool on);
    bool armed(project::Uid track) const { return armed_.count(track) != 0; }
    bool anyArmed() const { return !armed_.empty(); }
    // The armed track that live notes play, if a synth or drum track is armed (else the model uses the selection).
    project::Uid armedNoteTrack() const;
    void pruneArmed();                       // drop tracks that no longer exist

    // ---- the take ----
    bool recording() const { return recording_; }
    // Starts the transport (arrangement, from the cursor, after the count-in) and the capture.
    bool start(std::string& error);
    // Stops, writes the take and places the clips. `message` describes what happened (also on failure).
    bool stop(std::string& message);
    // True when a take would record something: an armed audio track, or curve recording with a route to record.
    bool canRecord(std::string* why = nullptr) const;
    // Drain the performance frames into the take (UI timer; also called by stop()).
    void poll();
    size_t curveFrames() const { return curve_.size(); }
    // The timeline position the recording has reached, and how long it has been going (UI display).
    double recordedSeconds() const;
    float inputPeak() const { return engine_.inputPeak(); }

    // Compensation in frames for a take made now: device round trip + the engine's own latency + the user offset.
    int compensationFrames() const;

private:
    AppModel& model_;
    engine::Engine& engine_;
    engine::Recorder recorder_;
    RecordingEnv env_;
    RecordingSettings settings_;
    std::set<project::Uid> armed_;
    bool recording_ = false, metroWas_ = false, audioTake_ = false, trackerWas_ = false;
    std::vector<engine::Engine::TimedPerf> curve_;
    std::vector<engine::Engine::NoteRecord> notes_;
    bool midiTake_ = false;
    int takeCounter_ = 0;
    std::string takePath_;
    int compAtStart_ = 0;
};

}  // namespace ddaw::app
