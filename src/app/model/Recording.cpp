#include "app/model/Recording.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <thread>

#include "app/model/AppModel.h"
#include "app/model/CurveRecord.h"
#include "project/ProjectJson.h"

namespace ddaw::app {

void RecordingController::arm(project::Uid track, bool on) {
    const auto* t = edit::findTrack(model_.project(), track);
    if (on && (!t || t->kind == project::TrackKind::Bus)) return;
    const bool audio = t && t->kind == project::TrackKind::Audio;
    if (on) armed_.insert(track); else armed_.erase(track);
    if (on && audio && env_.openInput) {
        const auto err = env_.openInput();   // the first arm opens the device input (and asks for microphone access)
        if (!err.empty()) model_.reportError(err);
    }
    if (audio) model_.setMonitor(track, on && settings_.monitor);
    model_.notify(ModelEvent::Recording);
}

project::Uid RecordingController::armedNoteTrack() const {
    for (const auto uid : armed_) {
        const auto* t = edit::findTrack(model_.project(), uid);
        if (t && (t->kind == project::TrackKind::Synth || t->kind == project::TrackKind::Drum)) return uid;
    }
    return 0;
}

void RecordingController::setMonitor(bool on) {
    settings_.monitor = on;
    for (const auto uid : armed_) {
        const auto* t = edit::findTrack(model_.project(), uid);
        if (t && t->kind == project::TrackKind::Audio) model_.setMonitor(uid, on);
    }
}

void RecordingController::pruneArmed() {
    for (auto it = armed_.begin(); it != armed_.end();)
        if (!edit::findTrack(model_.project(), *it)) it = armed_.erase(it); else ++it;
}

int RecordingController::compensationFrames() const {
    const double sr = env_.sampleRate ? env_.sampleRate() : 48000.0;
    const int in = env_.inputLatencyFrames ? env_.inputLatencyFrames() : 0, out = env_.outputLatencyFrames ? env_.outputLatencyFrames() : 0;
    return std::max(0, in + out + engine_.latencySamples() + int(std::lround(settings_.offsetMs * sr / 1000.0)));
}

bool RecordingController::canRecord(std::string* why) const {
    bool audio = false, midi = false;
    for (const auto uid : armed_) {
        const auto* t = edit::findTrack(model_.project(), uid);
        if (!t) continue;
        if (t->kind == project::TrackKind::Audio) audio = true; else midi = true;
    }
    bool routes = false;
    for (const auto& t : model_.project().tracks) for (const auto& pf : t.perf) routes |= pf.on && pf.record;
    if (audio || (midi && settings_.recordNotes) || (settings_.recordCurves && routes)) return true;
    if (why) *why = settings_.recordCurves ? "no performance route is set to record" : "arm a track first";
    return false;
}

void RecordingController::poll() {
    engine::Engine::TimedPerf tp;
    engine::Engine::NoteRecord nr;
    if (!recording_) { while (engine_.popPerformance(tp)) {} while (engine_.popNoteRecord(nr)) {} return; }
    while (engine_.popPerformance(tp)) curve_.push_back(tp);
    while (engine_.popNoteRecord(nr)) notes_.push_back(nr);
}

bool RecordingController::start(std::string& error) {
    if (recording_) { error = "already recording"; return false; }
    pruneArmed();
    if (!canRecord(&error)) return false;
    audioTake_ = false;
    midiTake_ = false;
    for (const auto uid : armed_) {
        const auto* t = edit::findTrack(model_.project(), uid);
        if (!t) continue;
        if (t->kind == project::TrackKind::Audio) audioTake_ = true; else midiTake_ = settings_.recordNotes;
    }
    const double sr = env_.sampleRate ? env_.sampleRate() : 48000.0;
    const bool needsInput = audioTake_ || (settings_.recordCurves && !midiTake_);
    const int open = env_.inputChannels ? env_.inputChannels() : 1;
    if (needsInput && open <= 0 && env_.openInput) {   // curve recording without an armed track has not opened the input yet
        const auto err = env_.openInput();
        if (!err.empty()) { error = err; return false; }
    }
    const int nowOpen = env_.inputChannels ? env_.inputChannels() : 1;
    if (needsInput && nowOpen <= 0 && env_.inputChannels) { error = "No audio input is open: check the input device and the microphone permission"; return false; }
    const int ch = std::clamp(nowOpen, 1, 2);
    curve_.clear();
    notes_.clear();
    engine::Engine::TimedPerf drop;
    while (engine_.popPerformance(drop)) {}      // stale frames from before the take
    engine::Engine::NoteRecord dropN;
    while (engine_.popNoteRecord(dropN)) {}
    trackerWas_ = engine_.trackerEnabled();
    if (settings_.recordCurves) engine_.setTrackerEnabled(true);
    if (audioTake_) {
        takePath_ = (std::filesystem::temp_directory_path() / ("ddaw-take-" + std::to_string(++takeCounter_) + ".wav")).string();
        if (!recorder_.start(takePath_, ch, sr, error)) return false;
    } else {
        engine_.requestCapture(true, false);
    }
    compAtStart_ = compensationFrames();
    // count-in: play from before the cursor, with the click on
    const auto& meta = model_.project().meta;
    const double bar = 96.0 * meta.tsTop;
    const double from = model_.cursorTicks() - settings_.countInBars * bar;
    metroWas_ = model_.metronome();
    if (settings_.countInBars > 0 && !metroWas_) model_.setMetronome(true);
    recording_ = true;
    model_.play(true, from);
    model_.notify(ModelEvent::Recording);
    return true;
}

double RecordingController::recordedSeconds() const {
    const double sr = env_.sampleRate ? env_.sampleRate() : 48000.0;
    return recording_ ? double(engine_.captureInfo().frames) / sr : 0.0;
}

bool RecordingController::stop(std::string& message) {
    if (!recording_) { message = "not recording"; return false; }
    poll();
    recording_ = false;
    model_.stopTransportOnly();
    if (settings_.countInBars > 0 && !metroWas_) model_.setMetronome(false);

    engine::Recorder::Take take;
    if (audioTake_) take = recorder_.stop();
    else {
        engine_.requestCapture(false);
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (engine_.captureInfo().active && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const auto info = engine_.captureInfo();
        take.sampleRate = env_.sampleRate ? env_.sampleRate() : 48000.0;
        take.startTick = info.startTick;
        take.frames = info.frames;
        take.ok = info.frames > 0;
        if (!take.ok) take.error = "nothing was recorded (the transport never started, or no input reached the engine)";
    }
    // frames the engine queued up to the stop
    {
        engine::Engine::TimedPerf tp;
        while (engine_.popPerformance(tp)) curve_.push_back(tp);
        engine::Engine::NoteRecord nr;
        while (engine_.popNoteRecord(nr)) notes_.push_back(nr);
    }
    if (settings_.recordCurves && !trackerWas_) engine_.setTrackerEnabled(false);
    model_.notify(ModelEvent::Recording);
    if (!take.ok) {
        message = "Recording failed: " + take.error;
        std::error_code ec;
        if (audioTake_) std::filesystem::remove(take.path, ec);
        return false;
    }

    const auto& p = model_.project();
    const double sr = take.sampleRate, bpm = p.meta.bpm > 0 ? p.meta.bpm : 120.0;
    const double secPerTick = 60.0 / bpm / 96.0;
    const double compSec = double(compAtStart_) / sr;
    std::vector<document::Command> cmds;
    std::string audioNote, curveNote, noteNote;

    // ---- audio: a clip on every armed track, trimmed by the latency and by any count-in before tick 0 ----
    if (audioTake_) {
        std::string id, err;
        const std::string name = "Take " + std::to_string(takeCounter_);
        if (!model_.loadWavSample(take.path, id, err, name)) { message = "Cannot load the take: " + err; return false; }
        std::error_code ec;
        std::filesystem::remove(take.path, ec);
        const double preSec = take.startTick < 0 ? -take.startTick * secPerTick : 0.0;
        const double offset = compSec + preSec;
        const double usable = double(take.frames) / sr - offset;
        if (usable <= 0.01) {
            audioNote = "the take was shorter than the latency compensation, so no clip was placed";
        } else {
            project::Clip c;
            c.len = std::max(24.0, usable / secPerTick);
            c.audio = project::AudioClipData{};
            c.audio->sampleId = id;
            c.audio->sampleName = name;
            c.audio->offset = offset;
            c.audio->dur = usable;
            const double start = std::max(0.0, take.startTick);
            const std::string base = edit::uniqueArrKey(p);   // the group applies later, so keys are made distinct here
            int i = 0;
            for (const auto uid : armed_) {
                const auto* t = edit::findTrack(p, uid);
                if (!t || t->kind != project::TrackKind::Audio) continue;   // only audio tracks get the recorded sound
                cmds.push_back({"arrclip.set", {{"key", base + "_" + std::to_string(++i)}, {"arr", {{"trackId", t->id}, {"start", start}, {"clip", project::clipToJson(c)}}}}});
            }
            char buf[120];
            std::snprintf(buf, sizeof buf, "%.1f s on %d track(s), compensated %.1f ms%s", double(take.frames) / sr, i, compSec * 1000.0, take.dropped ? " (WARNING: samples were dropped)" : "");
            audioNote = buf;
        }
    }

    // ---- curves: the routes marked `record` become automation lanes on their tracks ----
    if (settings_.recordCurves) {
        CurveTiming timing;
        timing.startInput = double(engine_.captureStartInputFrame());
        timing.startTick = take.startTick;
        timing.compFrames = double(compAtStart_);
        timing.ticksPerFrame = bpm * 96.0 / 60.0 / sr;
        int lanes = 0;
        for (const auto& t : p.tracks)
            for (const auto& pf : t.perf) {
                if (!pf.on || !pf.record) continue;
                const auto pts = buildLane(curve_, perfSourceFromName(pf.source), pf.srcMin, pf.srcMax, timing);
                if (pts.empty()) continue;
                for (const auto& tg : pf.targets) {
                    if (tg.dest.empty() || tg.pkey.empty()) continue;
                    const std::string key = tg.dest + "|" + tg.fxId + "|" + tg.pkey;
                    auto it = t.autoLanes.find(key);
                    const auto merged = mergeLane(it == t.autoLanes.end() ? std::vector<project::AutoPoint>{} : it->second, pts);
                    cmds.push_back({"env.set", {{"scope", {{"track", t.uid}}}, {"key", key}, {"points", project::pointsToJson(merged)}}});
                    ++lanes;
                }
            }
        char buf[100];
        std::snprintf(buf, sizeof buf, "%d automation lane(s) from %zu tracked frames", lanes, curve_.size());
        curveNote = buf;
    }
    curve_.clear();

    // ---- notes: what was played on the armed synth / drum tracks becomes a clip on each ----
    if (midiTake_) {
        // The player kept time with what they heard, which lagged the timeline by the device's output latency and
        // the engine's own delay (there is no input latency on a MIDI key).
        const double outLat = env_.outputLatencyFrames ? double(env_.outputLatencyFrames()) : 0.0;
        double lat = outLat + double(engine_.latencySamples());
        if (engine_.polyInputEnabled())   // notes found in audio: also the input device and the analysis window
            lat += (env_.inputLatencyFrames ? double(env_.inputLatencyFrames()) : 0.0) + double(engine_.polyLatencyFrames());
        const double compTicks = lat / sr / secPerTick;
        const double startTick = take.startTick, endTick = take.startTick + double(take.frames) / sr / secPerTick;
        const double clipStart = std::max(0.0, startTick);
        int made = 0, total = 0;
        const std::string base = edit::uniqueArrKey(p) + "_n";
        for (size_t ti = 0; ti < p.tracks.size(); ++ti) {
            const auto& t = p.tracks[ti];
            if (!armed_.count(t.uid) || t.kind == project::TrackKind::Audio) continue;
            struct Open { double start; float vel; };
            std::map<uint32_t, Open> open;
            std::map<uint32_t, std::array<std::vector<project::ExprPoint>, 3>> curves;   // expression echoed while each note sounds
            project::Clip c;
            auto close = [&](uint32_t id, double endAt) {
                auto it = open.find(id);
                if (it == open.end()) return;
                project::Note n;
                n.pitch = 0;
                n.startTicks = it->second.start;
                n.durTicks = std::max(12.0, endAt - it->second.start);
                n.velocity = it->second.vel;
                if (auto cv = curves.find(id); cv != curves.end()) {   // dimension 0 slide, 1 pressure, 2 bend
                    for (auto& [dim, dst] : {std::pair<int, std::vector<project::ExprPoint>*>{0, &n.slide}, {1, &n.pressure}, {2, &n.bend}})
                        for (const auto& pt : cv->second[size_t(dim)]) if (pt.t <= n.durTicks) dst->push_back(pt);
                }
                c.notes.push_back(n);
                open.erase(it);
            };
            std::map<uint32_t, int> pitchOf;
            std::vector<project::Note> done;
            for (const auto& ev : notes_) {
                if (ev.track != ti) continue;
                const double tick = ev.tick - compTicks;
                if (ev.on == 2) {   // expression of a note that is sounding: a point on its curve, relative to the note's start
                    if (auto it = open.find(ev.id); it != open.end() && ev.dim < 3)
                        curves[ev.id][ev.dim].push_back({std::max(0.0, tick - clipStart - it->second.start), double(ev.velocity)});
                } else if (ev.on) { if (tick >= clipStart) { open[ev.id] = {tick - clipStart, ev.velocity}; pitchOf[ev.id] = ev.pitch; } }
                else if (open.count(ev.id)) { const auto pitch = pitchOf[ev.id]; close(ev.id, tick - clipStart); c.notes.back().pitch = pitch; }
            }
            for (auto& [id, o] : std::map<uint32_t, Open>(open)) { const auto pitch = pitchOf[id]; close(id, std::max(o.start + 12.0, endTick - clipStart)); c.notes.back().pitch = pitch; }
            if (c.notes.empty()) continue;
            std::sort(c.notes.begin(), c.notes.end(), [](const project::Note& a, const project::Note& b) { return a.startTicks < b.startTicks; });
            double last = 0;
            for (const auto& n : c.notes) last = std::max(last, n.startTicks + n.durTicks);
            c.len = std::max(96.0, std::ceil(std::max(last, endTick - clipStart) / 96.0) * 96.0);
            cmds.push_back({"arrclip.set", {{"key", base + std::to_string(++made)}, {"arr", {{"trackId", t.id}, {"start", clipStart}, {"clip", project::clipToJson(c)}}}}});
            total += int(c.notes.size());
        }
        if (made) noteNote = std::to_string(total) + " note(s) on " + std::to_string(made) + " track(s)";
    }
    notes_.clear();

    if (!cmds.empty() && !model_.applyGroup("record", cmds)) { message = "Cannot place the take: " + model_.lastError(); return false; }
    message = "Recorded";
    bool first = true;
    for (const auto* part : {&audioNote, &noteNote, &curveNote}) {
        if (part->empty()) continue;
        message += (first ? ": " : "; ") + *part;
        first = false;
    }
    return true;
}

}  // namespace ddaw::app
