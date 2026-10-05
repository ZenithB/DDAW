#pragma once
// Everything the UI shares: the document wired to the live engine, the selection, file operations and
// transport state. JUCE-free (so it is unit tested); views register a listener and re-read on change.
// UI thread only.
#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "app/model/Edit.h"
#include "app/model/Recording.h"
#include "engine/PolyInput.h"
#include "document/Document.h"
#include "document/Session.h"
#include "engine/Engine.h"
#include "engine/GraphService.h"
#include "project/SampleBank.h"

namespace ddaw::app {

enum class ModelEvent { Document, Selection, Transport, File, Recording };

struct Selection {
    project::Uid track = 0;
    std::string scene;           // session scene of the selected clip slot (may be empty)
    edit::ClipRef clip;          // the clip open in the editor (invalid: none)
    project::Uid device = 0;
};

class AppModel {
public:
    // Starts the builder thread and submits the (empty) project. The engine must be prepared at `sampleRate`.
    AppModel(engine::Engine& engine, double sampleRate);
    ~AppModel();
    AppModel(const AppModel&) = delete;
    AppModel& operator=(const AppModel&) = delete;

    const project::Project& project() const { return doc_->project(); }
    document::Document& document() { return *doc_; }
    engine::Engine& engine() { return engine_; }
    engine::GraphService& service() { return svc_; }
    std::shared_ptr<project::SampleBank> sampleBank() { return bank_; }

    // ---- editing (every change is a document command, so it is undoable) ----
    bool apply(const document::Command& c);                 // false and lastError() set on a bad command
    bool applyGroup(const std::string& label, const std::vector<document::Command>& cs);
    void undo();
    void redo();
    bool canUndo() const { return doc_->canUndo(); }
    bool canRedo() const { return doc_->canRedo(); }
    const std::string& lastError() const { return lastError_; }
    // A live parameter edit: coalesces a drag into a single undo step per gesture.
    void beginGesture(const std::string& label);
    void endGesture();

    // ---- selection ----
    const Selection& selection() const { return sel_; }
    void selectTrack(project::Uid uid);
    void selectClip(const edit::ClipRef& r);
    void selectDevice(project::Uid uid);
    void selectSlot(project::Uid track, const std::string& scene);  // an empty session slot (no clip open)

    // ---- transport ----
    void play(bool arrangement, double fromTicks = 0.0);
    void stop();                // also ends a take in progress (the clips are placed)
    void stopTransportOnly();   // the engine stop alone (RecordingController uses it)
    void launchClip(const std::string& trackId, const std::string& sceneId);
    void launchScene(const std::string& sceneId);
    void stopTrack(const std::string& trackId);
    void stopAllClips();
    // Arrangement start position (where Play begins in arrangement mode).
    double cursorTicks() const { return cursor_; }
    void setCursor(double ticks) { cursor_ = std::max(0.0, ticks); notify(ModelEvent::Transport); }
    void togglePlay();
    // Sound a note on a track from the UI (piano-roll key, device audition). noteOff must follow.
    void audition(project::Uid track, int pitch, bool on, float velocity = 0.8f);
    void setMetronome(bool on);
    bool metronome() const { return metronome_; }
    bool arrangementMode() const { return arrangementMode_; }
    void setArrangementMode(bool a) { arrangementMode_ = a; }
    engine::MeterSnapshot meters() const { return engine_.meters().snapshot(); }

    // ---- files ----
    // A native project package (directory), a single native/synthyy JSON file, or a synthyy project.
    bool open(const std::string& path, std::string& error);
    bool save(const std::string& path, std::string& error);   // package directory
    void newProject();
    const std::string& path() const { return path_; }
    bool dirty() const { return doc_->revision() != savedRevision_; }
    std::string title() const;
    std::string sampleName(const std::string& id) const { auto it = sampleNames_.find(id); return it == sampleNames_.end() ? id : it->second; }
    std::vector<std::string> sampleIds() const { return bank_->ids(); }
    // `displayName` empty: the file name.
    bool loadWavSample(const std::string& path, std::string& sampleId, std::string& error, const std::string& displayName = {});

    // The device (re)started at this rate: later builds use it, and the project is rebuilt for it.
    void setSampleRate(double sr);

    // ---- tracking (B2) and monitoring ----
    // Run the input tracker and send its frames to a track's instrument (0 = nobody: frames still drive
    // performance routes). `target` is a track uid.
    void setTracking(bool on, project::Uid target = 0);
    bool tracking() const { return tracking_; }
    project::Uid trackingTarget() const { return trackTarget_; }
    void setTrackerRange(float minHz, float maxHz);
    float trackerMinHz() const { return minHz_; }
    // Route the live input through a track's effects (audio tracks).
    void setMonitor(project::Uid track, bool on);
    bool monitored(project::Uid track) const { return monitored_.count(track) != 0; }
    // ---- live note input ----
    // The track live notes play: the armed synth / drum track, else the selected one. Re-sent to the engine
    // whenever it changes or the graph is rebuilt.
    project::Uid liveTarget() const;
    // Play a note on it now (a key, a pad). Safe from any thread: forwards to the engine's live input.
    void noteOn(int pitch, float velocity = 0.8f) { engine_.liveNote(engine::Engine::LiveKind::NoteOn, pitch, velocity); }
    void noteOff(int pitch) { engine_.liveNote(engine::Engine::LiveKind::NoteOff, pitch); }
    void sustain(bool down) { engine_.liveNote(down ? engine::Engine::LiveKind::SustainDown : engine::Engine::LiveKind::SustainUp); }
    void allNotesOff() { engine_.liveNote(engine::Engine::LiveKind::AllOff); }
    // Per-note expression (dimension 0 slide, 1 pressure, 2 bend in semitones) and an all-notes bend, from any thread.
    void noteExpression(int pitch, int dimension, float value) { engine_.liveExpression(pitch, dimension, value); }
    void bend(float semitones) { engine_.liveBend(semitones); }
    // MPE (B5): with it on, each note on a member channel (2-16) carries its own pitch bend (range `mpeRange`, 48 semitones by
    // default), slide (CC 74) and pressure; channel 1 and non-MPE controllers bend every note by `bendRange` (2 semitones).
    // Read by the MIDI thread, hence atomics.
    void setMpe(bool on) { mpe_ = on; if (on && mpeLower_.load() == 0 && mpeUpper_.load() == 0) mpeLower_ = 15; notify(ModelEvent::Recording); }
    bool mpe() const { return mpe_.load(std::memory_order_relaxed); }
    // MPE zones: the lower zone has master channel 1 and member channels 2..1+n, the upper zone master 16 and members
    // 15 down to 16-n (n members each; 15 lower members and no upper zone is the default "all channels" layout).
    void setMpeZones(int lower, int upper) { applyZones(lower, upper); notify(ModelEvent::Recording); }
    int mpeLowerMembers() const { return mpeLower_.load(std::memory_order_relaxed); }
    int mpeUpperMembers() const { return mpeUpper_.load(std::memory_order_relaxed); }
    // A member channel carries one note's own expression; with MPE off there are none.
    bool mpeMember(int ch) const noexcept {
        if (!mpe()) return false;
        const int lo = mpeLower_.load(std::memory_order_relaxed), up = mpeUpper_.load(std::memory_order_relaxed);
        return (lo > 0 && ch >= 2 && ch <= 1 + lo) || (up > 0 && ch <= 15 && ch >= 16 - up);
    }
    // From the MIDI thread (an MPE configuration message, a pitch-bend-sensitivity RPN): takes effect at once, the UI
    // learns of it on its next tick(). `members` 0 disables that zone.
    void midiConfigureMpe(int masterChannel, int members);
    void midiSetBendRange(bool memberChannel, float semitones);
    void setBendRange(float semitones) { bendRange_ = std::clamp(semitones, 1.0f, 96.0f); notify(ModelEvent::Recording); }
    float bendRange() const { return bendRange_.load(std::memory_order_relaxed); }
    void setMpeRange(float semitones) { mpeRange_ = std::clamp(semitones, 1.0f, 96.0f); notify(ModelEvent::Recording); }
    float mpeRange() const { return mpeRange_.load(std::memory_order_relaxed); }
    // Polyphonic audio in: chords played into the input become notes on the live target. Opens the input.
    void setPolyInput(bool on);
    bool polyInput() const { return poly_ && poly_->running(); }
    int polyLatencyFrames() const { return poly_ ? poly_->latencyFrames() : 0; }
    // UI timer (~30 Hz): keeps the engine's track indices right after rebuilds and drains the performance queue.
    void tick();

    // ---- controllers (B5): gamepad axes / buttons and MIDI CCs -> morph sticks and macros, via project bindings ----
    // `value` is the control's position 0..1 (sticks 0.5 at rest). UI thread. True when a binding (or learn mode) used it.
    bool controllerInput(const std::string& source, double value);
    // Learn: the next control moved by a clear amount is bound to `target` (see app/model/Controllers.h).
    void startLearn(const std::string& target);
    void cancelLearn();
    const std::string& learnTarget() const { return learn_; }
    // Apply an edit that is not an undo step (values streaming from a controller).
    bool applyTransient(const document::Command& c);

    // ---- recording ----
    RecordingController& recording() { return *recording_; }
    // A message for the user from the model layer (the app shows it in the status line); also stored.
    void reportError(const std::string& m) { lastError_ = m; status_ = m; notify(ModelEvent::Recording); }
    const std::string& status() const { return status_; }
    void setStatus(const std::string& s) { status_ = s; }

    // ---- change notification ----
    using Listener = std::function<void(ModelEvent)>;
    int addListener(Listener l);
    void removeListener(int id);
    void notify(ModelEvent e);

private:
    void applyBinding(const project::ControlBinding& b, double v);
    std::string learn_;
    void applyZones(int lower, int upper);
    std::atomic<bool> mpe_{false};
    std::atomic<int> mpeLower_{15}, mpeUpper_{0};
    std::atomic<bool> midiConfigChanged_{false};
    std::atomic<float> bendRange_{2.0f}, mpeRange_{48.0f};
    std::map<std::string, double> learnBase_;   // each source's first value while learning
    void replaceDocument(project::Project p, project::Uid nextUid);
    void fixSelection();

    engine::Engine& engine_;
    engine::GraphService svc_;
    std::unique_ptr<document::Document> doc_;
    std::unique_ptr<document::Session> session_;
    std::shared_ptr<project::SampleBank> bank_ = std::make_shared<project::SampleBank>();
    std::map<std::string, std::string> sampleNames_;
    bool tracking_ = false;
    project::Uid trackTarget_ = 0;
    float minHz_ = 70.0f, maxHz_ = 1500.0f;
    std::set<project::Uid> monitored_;
    uint32_t syncedEpoch_ = 0;
    project::Uid sentLive_ = ~project::Uid(0);
    void syncEngine();
    std::unique_ptr<RecordingController> recording_;
    std::unique_ptr<engine::PolyInput> poly_;   // after everything it feeds: stopped first on destruction
    std::string status_;
    Selection sel_;
    std::string lastError_, path_;
    uint64_t savedRevision_ = 0;
    bool metronome_ = false, arrangementMode_ = false;
    double cursor_ = 0.0;
    uint32_t auditionId_ = 0x80000000u;
    std::vector<std::pair<int, Listener>> listeners_;
    int nextListener_ = 1;
};

}  // namespace ddaw::app
