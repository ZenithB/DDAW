#include "app/model/Stress.h"

#include <cmath>
#include <filesystem>

#include "app/model/Demo.h"
#include "devices/Registry.h"
#include "project/ProjectJson.h"
#include "project/SampleBank.h"

namespace ddaw::app {

void buildStressProject(AppModel& model, double sr) {
    buildDemo(model);
    // a sample, a sampler track and an audio track with clips, so the sample paths are exercised too
    const auto wav = (std::filesystem::temp_directory_path() / "ddaw_stress_tone.wav").string();
    {
        SampleBuf b;
        b.sampleRate = float(sr);
        for (int i = 0; i < int(sr); ++i) b.l.push_back(0.4f * std::sin(2.0f * 3.14159265f * 220.0f * float(i) / float(sr)));
        project::writeWavFloat32(wav, b);
    }
    std::string id, err;
    model.loadWavSample(wav, id, err, "tone");
    std::filesystem::remove(wav);
    model.apply(edit::addTrack(model.project(), project::TrackKind::Audio, "Audio"));
    model.apply(edit::addTrack(model.project(), project::TrackKind::Synth, "Sampler"));
    {
        const auto& p = model.project();
        const auto audioUid = p.tracks[p.tracks.size() - 2].uid, samplerUid = p.tracks.back().uid;
        project::Clip c; c.len = 384; c.audio = project::AudioClipData{}; c.audio->sampleId = id; c.audio->loop = 1;
        model.apply({"clip.set", {{"track", audioUid}, {"scene", p.scenes[0]}, {"clip", project::clipToJson(c)}}});
        model.apply(edit::setInstrument(p, samplerUid, "sampler"));
        auto d = model.project().tracks.back().inst;
        d.sampleId = id;
        model.apply({"inst.set", {{"track", samplerUid}, {"device", project::deviceToJson(d, true)}}});
        project::Clip n; n.len = 384; project::Note x; x.pitch = 60; x.durTicks = 96; n.notes = {x};
        model.apply({"clip.set", {{"track", samplerUid}, {"scene", p.scenes[0]}, {"clip", project::clipToJson(n)}}});
    }
    if (instrumentRegistered("ddsp")) {   // the neural instrument, under the same abuse
        model.apply(edit::addTrack(model.project(), project::TrackKind::Synth, "Violin"));
        const auto uid = model.project().tracks.back().uid;
        model.apply(edit::setInstrument(model.project(), uid, "ddsp"));
        project::Clip n; n.len = 384;
        for (int i = 0; i < 4; ++i) { project::Note x; x.pitch = 57 + 2 * i; x.startTicks = 96.0 * i; x.durTicks = 80; n.notes.push_back(x); }
        model.apply({"clip.set", {{"track", uid}, {"scene", model.project().scenes[0]}, {"clip", project::clipToJson(n)}}});
    }
    {   // live input: a voice follower sings the "microphone", routes drive a cutoff and a gain, an audio track monitors it
        model.apply(edit::addTrack(model.project(), project::TrackKind::Synth, "Voice"));
        const auto voice = model.project().tracks.back().uid;
        model.apply(edit::setInstrument(model.project(), voice, "follow"));
        model.apply(edit::addTrack(model.project(), project::TrackKind::Audio, "Mic"));
        const auto mic = model.project().tracks.back().uid;
        model.apply(edit::addDevice(model.project(), mic, "fx", "reverb"));
        project::PerfSpec route; route.source = "f0"; route.targets.push_back({"inst", "inst", "cutoff"});
        model.apply({"perf.insert", {{"track", model.project().tracks[2].uid}, {"index", 0}, {"perf", project::perfToJson(route)}}});
        project::PerfSpec route2; route2.source = "loudness"; route2.targets.push_back({"mix", "", "gain"});
        model.apply({"perf.insert", {{"track", mic}, {"index", 0}, {"perf", project::perfToJson(route2)}}});
        model.setTracking(true, voice);
        model.setMonitor(mic, true);
    }
}

}  // namespace ddaw::app
