#include "app/model/Demo.h"

#include <initializer_list>

#include "project/ProjectJson.h"

namespace ddaw::app {

namespace {

struct N { int pitch; double start, dur; double vel = 0.8; };

project::Clip clipOf(double len, std::initializer_list<N> notes) {
    project::Clip c;
    c.len = len;
    for (auto& n : notes) {
        project::Note x;
        x.pitch = n.pitch; x.startTicks = n.start; x.durTicks = n.dur; x.velocity = n.vel;
        c.notes.push_back(x);
    }
    return c;
}

document::Command setClip(project::Uid track, const std::string& scene, const project::Clip& c) {
    return {"clip.set", {{"track", track}, {"scene", scene}, {"clip", project::clipToJson(c)}}};
}
document::Command arrClip(const std::string& key, const std::string& trackId, double start, const project::Clip& c) {
    return {"arrclip.set", {{"key", key}, {"arr", {{"trackId", trackId}, {"start", start}, {"clip", project::clipToJson(c)}}}}};
}

}  // namespace

void buildDemo(AppModel& m) {
    using edit::addTrack;
    using project::TrackKind;
    std::vector<document::Command> cmds;
    auto add = [&](TrackKind k, const std::string& name, const std::string& inst, project::Uid& uid) {
        auto c = addTrack(m.project(), k, name);
        if (!inst.empty()) c.args["track"]["inst"]["type"] = inst;
        m.apply(c);
        uid = m.project().tracks.back().uid;
    };
    m.apply(document::cmd::setMeta("title", "Demo song"));
    m.apply(document::cmd::setMeta("bpm", 118.0));
    project::Uid drums, bass, chords, lead, pluck, bus;
    add(TrackKind::Drum, "Drums", "", drums);
    add(TrackKind::Synth, "Bass", "mono", bass);
    add(TrackKind::Synth, "Chords", "poly", chords);
    add(TrackKind::Synth, "Lead", "fm", lead);
    add(TrackKind::Synth, "Pluck", "pluck", pluck);
    add(TrackKind::Bus, "Space", "", bus);
    for (int i = 0; i < 5; ++i) m.apply(edit::addScene(m.project()));
    const auto& p = m.project();
    const auto sc = [&](int i) { return p.scenes[size_t(i)]; };

    // drums: kick 0, snare 1, hat 3 (pad = pitch % 8)
    auto beat = clipOf(384, {{0, 0, 24, 1.0}, {0, 96, 24, 0.9}, {0, 192, 24, 1.0}, {0, 288, 24, 0.9}, {1, 96, 24, 0.9}, {1, 288, 24, 0.95}, {2, 288, 12, 0.5},
                             {3, 0, 12, 0.6}, {3, 48, 12, 0.45}, {3, 96, 12, 0.6}, {3, 144, 12, 0.45}, {3, 192, 12, 0.6}, {3, 240, 12, 0.45}, {3, 288, 12, 0.6}, {3, 336, 12, 0.5}});
    auto fill = beat;
    for (int i = 0; i < 4; ++i) fill.notes.push_back([&] { project::Note n; n.pitch = 5; n.startTicks = 288.0 + i * 24; n.durTicks = 18; n.velocity = 0.6 + 0.1 * i; return n; }());
    // bass
    auto bassA = clipOf(384, {{33, 0, 90}, {33, 144, 40}, {36, 192, 90}, {33, 288, 40}, {31, 336, 40, 0.7}});
    auto bassB = clipOf(384, {{29, 0, 90}, {29, 144, 40}, {32, 192, 90}, {29, 288, 40}, {31, 336, 40, 0.7}});
    // chords: Am, F
    auto am = clipOf(384, {{57, 0, 360, 0.7}, {60, 0, 360, 0.65}, {64, 0, 360, 0.65}});
    auto fm = clipOf(384, {{53, 0, 360, 0.7}, {57, 0, 360, 0.65}, {60, 0, 360, 0.65}});
    // lead
    auto melody = clipOf(768, {{69, 0, 90}, {72, 96, 90}, {76, 192, 180, 0.9}, {74, 384, 90}, {72, 480, 90}, {69, 576, 180, 0.85}});
    auto arpy = clipOf(384, {{64, 0, 40}, {67, 48, 40}, {71, 96, 40}, {72, 144, 40}, {71, 192, 40}, {67, 240, 40}, {64, 288, 40}, {60, 336, 40}});

    m.applyGroup("demo clips", {
        setClip(drums, sc(0), beat), setClip(drums, sc(1), beat), setClip(drums, sc(2), fill),
        setClip(bass, sc(0), bassA), setClip(bass, sc(1), bassB), setClip(bass, sc(2), bassA),
        setClip(chords, sc(1), am), setClip(chords, sc(2), fm),
        setClip(lead, sc(2), melody), setClip(pluck, sc(1), arpy), setClip(pluck, sc(3), arpy),
        arrClip("a1", "t1", 0, beat), arrClip("a2", "t1", 384, beat), arrClip("a3", "t1", 768, fill),
        arrClip("a4", "t2", 0, bassA), arrClip("a5", "t2", 384, bassB), arrClip("a6", "t2", 768, bassA),
        arrClip("a7", "t3", 384, am), arrClip("a8", "t3", 768, fm),
        arrClip("a9", "t4", 768, melody),
    });
    m.apply(edit::addDevice(m.project(), chords, "fx", "chorus"));
    m.apply(edit::addDevice(m.project(), chords, "fx", "delay"));
    m.apply(edit::addDevice(m.project(), bass, "fx", "filter"));
    m.apply(edit::addDevice(m.project(), pluck, "midifx", "arp"));
    m.apply(edit::addDevice(m.project(), 0, "master", "comp"));
    m.apply(edit::addDevice(m.project(), bus, "fx", "reverb"));
    m.apply(document::cmd::setTrack(chords, "output", "t6"));
    m.apply(document::cmd::setTrack(chords, "gain", -4.0));
    m.apply(document::cmd::setTrack(bass, "gain", -2.0));
    m.apply(document::cmd::setTrack(lead, "gain", -6.0));
    m.apply(document::cmd::setTrack(pluck, "pan", 0.3));
    m.apply(document::cmd::setTrack(lead, "pan", -0.25));
    m.document().clearHistory();
    m.selectTrack(chords);
}

}  // namespace ddaw::app
