#include "app/model/AppModel.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "document/NativeFormat.h"
#include "project/SynthyyImport.h"

namespace ddaw::app {

namespace fs = std::filesystem;

AppModel::AppModel(engine::Engine& engine, double sampleRate) : engine_(engine), svc_(engine, sampleRate) {
    svc_.setSampleBank(bank_);
    recording_ = std::make_unique<RecordingController>(*this, engine_);
    doc_ = std::make_unique<document::Document>();
    session_ = std::make_unique<document::Session>(*doc_, svc_, engine_);
    svc_.start();
    session_->rebuild();
    savedRevision_ = doc_->revision();
}

AppModel::~AppModel() { poly_.reset(); svc_.stop(); }

bool AppModel::apply(const document::Command& c) {
    if (c.kind == "track.insert" && project().tracks.size() >= static_cast<size_t>(engine::kMaxMeterTracks)) {
        lastError_ = "too many tracks: the engine supports " + std::to_string(engine::kMaxMeterTracks);
        return false;
    }
    try {
        session_->apply(c);
        lastError_.clear();
    } catch (const std::exception& e) {
        lastError_ = e.what();
        return false;
    }
    fixSelection();
    notify(ModelEvent::Document);
    return true;
}

bool AppModel::applyGroup(const std::string& label, const std::vector<document::Command>& cs) {
    session_->beginGroup(label);
    try {
        for (auto& c : cs) session_->applyDeferred(c);
    } catch (const std::exception& e) {
        lastError_ = e.what();
        session_->endGroup();
        session_->undo();  // roll back what the group did (it is the newest step)
        return false;
    }
    session_->endGroup();
    lastError_.clear();
    fixSelection();
    notify(ModelEvent::Document);
    return true;
}

void AppModel::undo() { session_->undo(); fixSelection(); notify(ModelEvent::Document); }
void AppModel::redo() { session_->redo(); fixSelection(); notify(ModelEvent::Document); }

void AppModel::beginGesture(const std::string& label) { session_->beginGroup(label); }
void AppModel::endGesture() { session_->endGroup(); notify(ModelEvent::Document); }

void AppModel::fixSelection() {
    const auto& p = project();
    if (recording_) recording_->pruneArmed();
    for (auto it = monitored_.begin(); it != monitored_.end();) { if (!edit::findTrack(project(), *it)) it = monitored_.erase(it); else ++it; }
    if (trackTarget_ && !edit::findTrack(project(), trackTarget_)) { trackTarget_ = 0; }
    if (sel_.track && !edit::findTrack(p, sel_.track)) sel_.track = 0;
    if (sel_.clip.valid() && !edit::findClip(p, sel_.clip)) sel_.clip = {};
    if (!sel_.scene.empty() && std::find(p.scenes.begin(), p.scenes.end(), sel_.scene) == p.scenes.end()) sel_.scene.clear();
    if (sel_.device && !doc_->findDevice(sel_.device)) sel_.device = 0;
    if (!sel_.track && !p.tracks.empty()) sel_.track = p.tracks.front().uid;
}

void AppModel::selectTrack(project::Uid uid) {
    sel_.track = uid;
    sel_.device = 0;
    notify(ModelEvent::Selection);
}
void AppModel::selectClip(const edit::ClipRef& r) {
    sel_.clip = r;
    if (r.valid()) {
        if (r.arrKey.empty()) { sel_.track = r.track; sel_.scene = r.scene; }
        else if (auto it = project().arr.find(r.arrKey); it != project().arr.end())
            if (const auto* t = edit::findTrackById(project(), it->second.trackId)) sel_.track = t->uid;
    }
    notify(ModelEvent::Selection);
}
void AppModel::selectSlot(project::Uid track, const std::string& scene) {
    sel_.track = track;
    sel_.scene = scene;
    sel_.clip = {};
    notify(ModelEvent::Selection);
}
void AppModel::selectDevice(project::Uid uid) { sel_.device = uid; notify(ModelEvent::Selection); }

void AppModel::play(bool arrangement, double fromTicks) {
    arrangementMode_ = arrangement;
    session_->play(arrangement, fromTicks);
    notify(ModelEvent::Transport);
}
void AppModel::stopTransportOnly() { session_->stop(); notify(ModelEvent::Transport); }
void AppModel::stop() {
    if (recording_ && recording_->recording()) {
        std::string msg;
        recording_->stop(msg);
        status_ = msg;
        notify(ModelEvent::Document);
        return;
    }
    stopTransportOnly();
}

void AppModel::launchClip(const std::string& trackId, const std::string& sceneId) {
    const auto& ts = project().tracks;
    for (size_t i = 0; i < ts.size(); ++i)
        if (ts[i].id == trackId) {
            if (!engine_.meters().snapshot().playing) session_->play(false, 0.0), arrangementMode_ = false;
            session_->launchClip(i, sceneId);
            break;
        }
    notify(ModelEvent::Transport);
}
void AppModel::launchScene(const std::string& sceneId) {
    if (!engine_.meters().snapshot().playing) session_->play(false, 0.0), arrangementMode_ = false;
    session_->launchScene(sceneId);
    notify(ModelEvent::Transport);
}
void AppModel::stopTrack(const std::string& trackId) {
    const auto& ts = project().tracks;
    for (size_t i = 0; i < ts.size(); ++i) if (ts[i].id == trackId) session_->stopTrack(i);
    notify(ModelEvent::Transport);
}
void AppModel::stopAllClips() { session_->stopClips(); notify(ModelEvent::Transport); }

void AppModel::togglePlay() {
    if (engine_.meters().snapshot().playing) stop();
    else play(arrangementMode_, arrangementMode_ ? cursor_ : 0.0);
}

void AppModel::audition(project::Uid track, int pitch, bool on, float velocity) {
    const auto& ts = project().tracks;
    for (size_t i = 0; i < ts.size(); ++i) {
        if (ts[i].uid != track) continue;
        Cmd c;
        c.epoch = svc_.publishedEpoch();
        if (on) { c.type = CmdType::NoteOn; c.noteOn = {static_cast<uint16_t>(i), static_cast<uint8_t>(std::clamp(pitch, 0, 127)), velocity, ++auditionId_}; }
        else { c.type = CmdType::NoteOff; c.noteOff = {static_cast<uint16_t>(i), auditionId_}; }
        engine_.commands().push(c);
        return;
    }
}

void AppModel::setTracking(bool on, project::Uid target) {
    tracking_ = on;
    trackTarget_ = on ? target : 0;
    engine_.setTrackerEnabled(on || (recording_ && recording_->recording()));
    syncedEpoch_ = 0;   // re-send the target with the next tick
    syncEngine();
    notify(ModelEvent::Recording);
}

void AppModel::setTrackerRange(float minHz, float maxHz) {
    minHz_ = minHz; maxHz_ = maxHz;
    Cmd c;
    c.type = CmdType::TrackerConfig;
    c.trackerConfig = {minHz, maxHz, 0.15f, 0.5f};
    engine_.commands().push(c);
}

void AppModel::setMonitor(project::Uid track, bool on) {
    if (on) monitored_.insert(track); else monitored_.erase(track);
    syncedEpoch_ = 0;
    syncEngine();
    notify(ModelEvent::Recording);
}

// The engine addresses tracks by index in the live graph; the model by uid. After every published graph the
// indices may have moved, so the tracking target and the monitor flags are re-sent for that graph's epoch.
project::Uid AppModel::liveTarget() const {
    if (const auto armed = recording_ ? recording_->armedNoteTrack() : 0) return armed;
    const auto* t = edit::findTrack(project(), sel_.track);
    return t && (t->kind == project::TrackKind::Synth || t->kind == project::TrackKind::Drum) ? t->uid : 0;
}

void AppModel::syncEngine() {
    const uint32_t epoch = svc_.publishedEpoch();
    if (epoch == 0) return;
    const project::Uid live = liveTarget();
    if (epoch == syncedEpoch_ && live == sentLive_) return;
    sentLive_ = live;
    if (epoch == syncedEpoch_) {   // only the live target moved: no need to resend the rest
        const auto& tracks = project().tracks;
        Cmd lt;
        lt.type = CmdType::LiveTrack;
        lt.epoch = epoch;
        lt.liveTrack = {-1};
        for (size_t i = 0; i < tracks.size(); ++i) if (live && tracks[i].uid == live) lt.liveTrack = {static_cast<int16_t>(i)};
        engine_.commands().push(lt);
        return;
    }
    syncedEpoch_ = epoch;
    const auto& ts = project().tracks;
    Cmd t;
    t.type = CmdType::Tracking;
    t.epoch = epoch;
    t.tracking = {-1};
    for (size_t i = 0; i < ts.size(); ++i) if (trackTarget_ && ts[i].uid == trackTarget_) t.tracking = {static_cast<int16_t>(i)};
    engine_.commands().push(t);
    Cmd lt;
    lt.type = CmdType::LiveTrack;
    lt.epoch = epoch;
    lt.liveTrack = {-1};
    for (size_t i = 0; i < ts.size(); ++i) if (live && ts[i].uid == live) lt.liveTrack = {static_cast<int16_t>(i)};
    engine_.commands().push(lt);
    for (size_t i = 0; i < ts.size(); ++i) {
        Cmd m;
        m.type = CmdType::MonitorInput;
        m.epoch = epoch;
        m.monitorInput = {static_cast<uint16_t>(i), static_cast<uint8_t>(monitored_.count(ts[i].uid) ? 1 : 0)};
        engine_.commands().push(m);
    }
}

void AppModel::applyZones(int lower, int upper) {
    lower = std::clamp(lower, 0, 15);
    upper = std::clamp(upper, 0, 15 - lower);   // the zones cannot overlap
    mpeLower_ = lower;
    mpeUpper_ = upper;
}

void AppModel::midiConfigureMpe(int masterChannel, int members) {
    members = std::clamp(members, 0, 15);
    if (masterChannel == 1) applyZones(members, mpeUpper_.load());
    else if (masterChannel == 16) applyZones(mpeLower_.load(), members);
    else return;
    mpe_ = mpeLower_.load() > 0 || mpeUpper_.load() > 0;
    midiConfigChanged_ = true;
}

void AppModel::midiSetBendRange(bool memberChannel, float semitones) {
    if (memberChannel) mpeRange_ = std::clamp(semitones, 1.0f, 96.0f); else bendRange_ = std::clamp(semitones, 1.0f, 96.0f);
    midiConfigChanged_ = true;
}

void AppModel::tick() {
    if (midiConfigChanged_.exchange(false)) notify(ModelEvent::Recording);   // an MPE / bend-range message arrived: refresh the UI, save the settings
    syncEngine();
    if (recording_) recording_->poll();
}

void AppModel::setPolyInput(bool on) {
    if (!poly_) poly_ = std::make_unique<engine::PolyInput>(engine_);
    if (on) {
        if (recording_ && recording_->env().openInput) {
            const auto err = recording_->env().openInput();
            if (!err.empty()) { reportError(err); return; }
        }
        poly_->start();
    } else {
        poly_->stop();
    }
    notify(ModelEvent::Recording);
}

void AppModel::setMetronome(bool on) {
    metronome_ = on;
    Cmd c;
    c.type = on ? CmdType::MetronomeOn : CmdType::MetronomeOff;
    engine_.commands().push(c);
    notify(ModelEvent::Transport);
}

// ---- files ----

void AppModel::replaceDocument(project::Project p, project::Uid nextUid) {
    session_.reset();
    doc_ = std::make_unique<document::Document>(std::move(p), nextUid);
    session_ = std::make_unique<document::Session>(*doc_, svc_, engine_);
    session_->rebuild();
    savedRevision_ = doc_->revision();
    sel_ = {};
    fixSelection();
}

void AppModel::setSampleRate(double sr) {
    svc_.setSampleRate(sr);
    session_->rebuild();
}

void AppModel::newProject() {
    if (recording_->recording()) { std::string m; recording_->stop(m); }
    session_->stop();
    bank_ = std::make_shared<project::SampleBank>();
    svc_.setSampleBank(bank_);
    sampleNames_.clear();
    replaceDocument(project::Project{}, 1);
    path_.clear();
    notify(ModelEvent::File);
    notify(ModelEvent::Document);
}

bool AppModel::open(const std::string& path, std::string& error) {
    if (recording_->recording()) { std::string m; recording_->stop(m); }   // finish the take before the document goes
    try {
        document::LoadedProject lp;
        auto fresh = std::make_shared<project::SampleBank>();
        if (fs::is_directory(path)) lp = document::loadProjectPackage(path, *fresh);
        else {
            std::ifstream in(path);
            if (!in) throw std::runtime_error("cannot open " + path);
            std::stringstream ss;
            ss << in.rdbuf();
            lp = document::loadProjectJson(ss.str());
        }
        session_->stop();
        // The builder thread reads the bank, so it is replaced, never mutated in place.
        bank_ = fresh;
        svc_.setSampleBank(bank_);
        sampleNames_.clear();
        replaceDocument(std::move(lp.project), lp.nextUid);
        path_ = fs::is_directory(path) ? path : std::string();  // a loose JSON is saved as a new package
        error.clear();
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    notify(ModelEvent::File);
    notify(ModelEvent::Document);
    return true;
}

bool AppModel::save(const std::string& path, std::string& error) {
    try {
        document::saveProjectPackage(path, project(), doc_->nextUid(), *bank_, sampleNames_);
        path_ = path;
        savedRevision_ = doc_->revision();
        error.clear();
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    notify(ModelEvent::File);
    return true;
}

std::string AppModel::title() const {
    std::string t = project().meta.title;
    if (t.empty()) t = path_.empty() ? "Untitled" : fs::path(path_).stem().string();
    return t + (dirty() ? " *" : "");
}

bool AppModel::loadWavSample(const std::string& path, std::string& id, std::string& error, const std::string& displayName) {
    try {
        auto buf = project::loadWavSample(path);
        const std::string base = displayName.empty() ? fs::path(path).stem().string() : displayName;
        id = base;
        for (int n = 2; bank_->contains(id); ++n) id = base + "-" + std::to_string(n);
        auto next = std::make_shared<project::SampleBank>(*bank_);  // copy-on-write: the builder may be reading
        next->put(id, std::move(buf));
        bank_ = std::move(next);
        svc_.setSampleBank(bank_);
        sampleNames_[id] = displayName.empty() ? fs::path(path).filename().string() : displayName;
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

int AppModel::addListener(Listener l) {
    listeners_.emplace_back(nextListener_, std::move(l));
    return nextListener_++;
}
void AppModel::removeListener(int id) {
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(), [&](auto& p) { return p.first == id; }), listeners_.end());
}
void AppModel::notify(ModelEvent e) {
    auto copy = listeners_;  // a listener may add/remove listeners
    for (auto& [id, l] : copy) l(e);
}

}  // namespace ddaw::app
