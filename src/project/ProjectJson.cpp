#include "project/ProjectJson.h"

#include <algorithm>
#include <cstdio>

namespace ddaw::project {

using json = nlohmann::json;

namespace {

json paramsJson(const std::map<std::string, double>& m) {
    json o = json::object();
    for (auto& [k, v] : m) o[k] = v;
    return o;
}
json strMapJson(const std::map<std::string, std::string>& m) {
    json o = json::object();
    for (auto& [k, v] : m) o[k] = v;
    return o;
}

json deviceJson(const DeviceSpec& d, bool isInst = false) {
    json j = json::object();
    if (d.uid) j["uid"] = d.uid;
    if (!d.id.empty()) j["id"] = d.id;
    j["type"] = d.type;
    if (!isInst) j["on"] = d.on;
    j["params"] = paramsJson(d.params);
    if (d.outDb) j["out"] = *d.outDb;
    if (!d.srcTrack.empty()) j["srcTrack"] = d.srcTrack;
    if (d.srcPitch) j["srcPitch"] = *d.srcPitch;
    if (!d.sampleId.empty()) j["sampleId"] = d.sampleId;
    if (!d.sampleName.empty()) j["sampleName"] = d.sampleName;
    if (!d.padSamples.empty()) j["padSamples"] = strMapJson(d.padSamples);
    if (!d.padNames.empty()) j["padNames"] = strMapJson(d.padNames);
    return j;
}

json devicesJson(const std::vector<DeviceSpec>& v) {
    json a = json::array();
    for (auto& d : v) a.push_back(deviceJson(d));
    return a;
}

json autoJson(const AutoMap& m) {
    json o = json::object();
    for (auto& [k, pts] : m) {
        json a = json::array();
        for (auto& p : pts) a.push_back({{"t", p.t}, {"v", p.v}});
        o[k] = a;
    }
    return o;
}

json targetsJson(const std::vector<ModTarget>& ts) {
    json a = json::array();
    for (auto& t : ts) a.push_back({{"dest", t.dest}, {"fxId", t.fxId}, {"pkey", t.pkey}});
    return a;
}

json clipJson(const Clip& c) {
    json j = json::object();
    j["len"] = c.len;
    json notes = json::object();
    int i = 0;
    for (auto& n : c.notes) {
        json jn = {{"p", n.pitch}, {"s", n.startTicks}, {"d", n.durTicks}, {"v", n.velocity}, {"pr", n.probability}};
        if (n.uid) jn["uid"] = n.uid;
        for (const auto& [name, curve] : {std::pair<const char*, const std::vector<ExprPoint>*>{"bend", &n.bend}, {"slide", &n.slide}, {"pressure", &n.pressure}})
            if (!curve->empty()) { json a = json::array(); for (auto& p : *curve) a.push_back({{"t", p.t}, {"v", p.v}}); jn[name] = a; }
        char key[16];
        std::snprintf(key, sizeof key, "n%06d", i++);  // zero-padded: JSON objects sort keys lexically, so key order = note order
        notes[key] = jn;
    }
    j["notes"] = notes;
    if (!c.env.empty()) j["env"] = autoJson(c.env);
    if (c.audio) {
        const auto& a = *c.audio;
        json ja = {{"sampleId", a.sampleId}, {"sampleName", a.sampleName}, {"gainDb", a.gainDb}, {"pitch", a.pitch},
                   {"rev", a.rev}, {"loop", a.loop}, {"fadeIn", a.fadeIn}, {"fadeOut", a.fadeOut}};
        if (a.offset) ja["offset"] = *a.offset;
        if (a.dur) ja["dur"] = *a.dur;
        if (a.cents) ja["cents"] = *a.cents;
        if (a.xfade) ja["xfade"] = *a.xfade;
        j["audio"] = ja;
    }
    return j;
}

const char* kindName(TrackKind k) {
    switch (k) { case TrackKind::Drum: return "drum"; case TrackKind::Audio: return "audio"; case TrackKind::Bus: return "bus"; default: return "synth"; }
}

json arateJson(const ARateSpec& r) {
    return {{"id", r.id}, {"on", r.on}, {"source", r.source}, {"shape", r.shape}, {"hz", r.hz}, {"track", r.srcTrack}, {"follow", r.follow},
            {"attackMs", r.attackMs}, {"releaseMs", r.releaseMs}, {"depth", r.depth},
            {"target", {{"dest", r.target.dest}, {"fxId", r.target.fxId}, {"pkey", r.target.pkey}}}};
}

json morphJson(const MorphSpec& m) {
    json anchors = json::array();
    for (auto& a : m.anchors) anchors.push_back({{"name", a.name}, {"x", a.x}, {"y", a.y}, {"values", a.values}});
    return {{"name", m.name}, {"on", m.on}, {"x", m.x}, {"y", m.y}, {"method", m.method}, {"power", m.power}, {"width", m.width},
            {"targets", targetsJson(m.targets)}, {"curves", m.curves}, {"anchors", anchors}};
}

json trackJson(const Track& t) {
    json j = json::object();
    if (t.uid) j["uid"] = t.uid;
    j["id"] = t.id;
    j["name"] = t.name;
    j["kind"] = kindName(t.kind);
    j["inst"] = deviceJson(t.inst, true);
    j["fx"] = devicesJson(t.fx);
    if (!t.midifx.empty()) j["midifx"] = devicesJson(t.midifx);
    j["gain"] = t.gainDb;
    j["pan"] = t.pan;
    j["mute"] = t.mute;
    j["solo"] = t.solo;
    j["sendA"] = t.sendA;
    j["sendB"] = t.sendB;
    if (!t.output.empty()) j["output"] = t.output;
    if (!t.sends.empty()) j["sends"] = paramsJson(t.sends);
    if (t.send != SendBus::None) j["send"] = t.send == SendBus::A ? "A" : t.send == SendBus::B ? "B" : "F";
    if (!t.lfos.empty()) {
        json a = json::array();
        for (auto& l : t.lfos) {
            json jl = {{"id", l.id}, {"on", l.on}, {"shape", l.shape}, {"sync", l.sync}, {"rate", l.rate}, {"hz", l.hz},
                       {"depth", l.depth}, {"phase", l.phase}, {"dest", l.dest}, {"fxId", l.fxId}, {"pkey", l.pkey}};
            if (!l.targets.empty()) jl["targets"] = targetsJson(l.targets);
            a.push_back(jl);
        }
        j["lfos"] = a;
    }
    if (!t.macros.empty()) {
        json a = json::array();
        for (auto& m : t.macros) a.push_back({{"name", m.name}, {"value", m.value}, {"targets", targetsJson(m.targets)}});
        j["macros"] = a;
    }
    if (!t.perf.empty()) {
        json a = json::array();
        for (auto& pf : t.perf) a.push_back({{"source", pf.source}, {"min", pf.srcMin}, {"max", pf.srcMax}, {"on", pf.on}, {"rec", pf.record}, {"targets", targetsJson(pf.targets)}});
        j["perf"] = a;
    }
    if (!t.arate.empty()) {
        json a = json::array();
        for (auto& r : t.arate) a.push_back(arateJson(r));
        j["arate"] = a;
    }
    if (!t.morph.empty()) {
        json a = json::array();
        for (auto& m : t.morph) a.push_back(morphJson(m));
        j["morph"] = a;
    }
    if (!t.autoLanes.empty()) j["auto"] = autoJson(t.autoLanes);
    return j;
}

}  // namespace

json trackToJson(const Track& t) { return trackJson(t); }
json clipToJson(const Clip& c) { return clipJson(c); }
json deviceToJson(const DeviceSpec& d, bool isInst) { return deviceJson(d, isInst); }
json lfoToJson(const LfoSpec& l) {
    json jl = {{"id", l.id}, {"on", l.on}, {"shape", l.shape}, {"sync", l.sync}, {"rate", l.rate}, {"hz", l.hz},
               {"depth", l.depth}, {"phase", l.phase}, {"dest", l.dest}, {"fxId", l.fxId}, {"pkey", l.pkey}};
    if (!l.targets.empty()) jl["targets"] = targetsJson(l.targets);
    return jl;
}
json perfToJson(const PerfSpec& pf) { return {{"source", pf.source}, {"min", pf.srcMin}, {"max", pf.srcMax}, {"on", pf.on}, {"rec", pf.record}, {"targets", targetsJson(pf.targets)}}; }
json arateToJson(const ARateSpec& r) { return arateJson(r); }
json morphToJson(const MorphSpec& m) { return morphJson(m); }
json bindingToJson(const ControlBinding& b) { return {{"source", b.source}, {"target", b.target}, {"min", b.min}, {"max", b.max}, {"invert", b.invert}}; }
json macroToJson(const MacroSpec& m) { return {{"name", m.name}, {"value", m.value}, {"targets", targetsJson(m.targets)}}; }
json pointsToJson(const std::vector<AutoPoint>& pts) {
    json a = json::array();
    for (auto& p : pts) a.push_back({{"t", p.t}, {"v", p.v}});
    return a;
}

json projectToJson(const Project& p) {
    json j = json::object();
    json meta = {{"title", p.meta.title}, {"bpm", p.meta.bpm}, {"swing", p.meta.swing}, {"swingSubdivision", p.meta.swingSubdivision},
                 {"humanize", p.meta.humanize}, {"root", p.meta.root}, {"scale", p.meta.scale}, {"launchQ", p.meta.launchQ},
                 {"masterGain", p.meta.masterGainDb}, {"loopOn", p.meta.loopOn}, {"loopStart", p.meta.loopStart},
                 {"loopEnd", p.meta.loopEnd}, {"tsTop", p.meta.tsTop}, {"tsBottom", p.meta.tsBottom}};
    j["meta"] = meta;
    j["tracks"] = json::array();
    for (auto& t : p.tracks) j["tracks"].push_back(trackJson(t));
    j["scenes"] = json::array();
    for (auto& s : p.scenes) j["scenes"].push_back({{"id", s}, {"name", s}});
    j["clips"] = json::object();
    for (auto& [k, c] : p.clips) j["clips"][k] = clipJson(c);
    j["arr"] = json::object();
    for (auto& [k, a] : p.arr) j["arr"][k] = {{"trackId", a.trackId}, {"start", a.start}, {"clip", clipJson(a.clip)}};
    if (!p.returns.empty()) {
        json a = json::array();
        for (auto& r : p.returns) a.push_back({{"id", r.id}, {"name", r.name}, {"fxType", r.fxType}, {"params", paramsJson(r.params)}, {"gain", r.gainDb}});
        j["returns"] = a;
    }
    if (!p.bindings.empty()) {
        json a = json::array();
        for (auto& b : p.bindings) a.push_back(bindingToJson(b));
        j["bindings"] = a;
    }
    j["masterFx"] = devicesJson(p.masterFx);
    if (!p.masterAuto.empty()) j["masterAuto"] = autoJson(p.masterAuto);
    return j;
}

size_t assignUids(Project& p, Uid& next) {
    size_t n = 0;
    auto give = [&](Uid& u) { if (u == 0) { u = next++; ++n; } };
    auto dev = [&](std::vector<DeviceSpec>& v) { for (auto& d : v) give(d.uid); };
    auto clip = [&](Clip& c) { for (auto& note : c.notes) give(note.uid); };
    for (auto& t : p.tracks) { give(t.uid); give(t.inst.uid); dev(t.fx); dev(t.midifx); }
    dev(p.masterFx);
    for (auto& [k, c] : p.clips) clip(c);
    for (auto& [k, a] : p.arr) clip(a.clip);
    return n;
}

Uid maxUid(const Project& p) {
    Uid m = 0;
    auto dev = [&](const std::vector<DeviceSpec>& v) { for (auto& d : v) m = std::max(m, d.uid); };
    auto clip = [&](const Clip& c) { for (auto& n : c.notes) m = std::max(m, n.uid); };
    for (auto& t : p.tracks) { m = std::max({m, t.uid, t.inst.uid}); dev(t.fx); dev(t.midifx); }
    dev(p.masterFx);
    for (auto& [k, c] : p.clips) clip(c);
    for (auto& [k, a] : p.arr) clip(a.clip);
    return m;
}

}  // namespace ddaw::project
