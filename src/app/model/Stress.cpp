#include "app/model/Stress.h"

#include "app/model/Controllers.h"

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
    {   // audio-rate modulation: an FM Operators track whose amp is driven by an oscillator and whose index follows the bass
        model.apply(edit::addTrack(model.project(), project::TrackKind::Synth, "FM Ops"));
        const auto uid = model.project().tracks.back().uid;
        model.apply(edit::setInstrument(model.project(), uid, "fmop"));
        project::Clip n; n.len = 384;
        for (int i = 0; i < 4; ++i) {   // each note carries expression curves, so playback exercises them
            project::Note x; x.pitch = 48 + 3 * i; x.startTicks = 96.0 * i; x.durTicks = 90;
            x.bend = {{0, 0.0}, {45, 1.5 + i}, {90, 0.0}};
            x.pressure = {{0, 0.0}, {60, 0.8}};
            if (i % 2) x.slide = {{0, 0.2}, {90, 1.0}};
            n.notes.push_back(x);
        }
        model.apply({"clip.set", {{"track", uid}, {"scene", model.project().scenes[0]}, {"clip", project::clipToJson(n)}}});
        project::ARateSpec am; am.id = "am"; am.source = "osc"; am.hz = 6.0; am.depth = 0.4; am.target = {"inst", "inst", "amp"};
        project::ARateSpec fol; fol.id = "fol"; fol.source = "track"; fol.srcTrack = model.project().tracks[1].id; fol.follow = true; fol.depth = 0.5; fol.target = {"inst", "inst", "index"};
        model.apply({"arate.insert", {{"track", uid}, {"index", 0}, {"arate", project::arateToJson(am)}}});
        model.apply({"arate.insert", {{"track", uid}, {"index", 1}, {"arate", project::arateToJson(fol)}}});
        // a morph map moving the index, a ratio and a level, with a gamepad and a CC bound to its stick
        const auto id = edit::findTrack(model.project(), uid)->id;
        project::MorphSpec mm; mm.name = "Timbre"; mm.x = 0.4; mm.y = 0.6;
        mm.targets = {{"inst", "inst", "index"}, {"inst", "inst", "r2"}, {"inst", "inst", "l2"}};
        mm.curves = {1.0, 1.0, 0.7};
        mm.anchors = {{"a", 0.1, 0.1, {0.1, 0.3, 0.2}}, {"b", 0.9, 0.2, {0.6, 0.8, 0.7}}, {"c", 0.5, 0.9, {0.9, 0.5, 0.9}}};
        model.apply({"morph.insert", {{"track", uid}, {"index", 0}, {"morph", project::morphToJson(mm)}}});
        project::ControlBinding bx; bx.source = "pad:lx"; bx.target = morphTarget(id, 0, 'x');
        project::ControlBinding by; by.source = "midi:cc74"; by.target = morphTarget(id, 0, 'y');
        model.apply({"binding.insert", {{"index", 0}, {"binding", project::bindingToJson(bx)}}});
        model.apply({"binding.insert", {{"index", 1}, {"binding", project::bindingToJson(by)}}});
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
