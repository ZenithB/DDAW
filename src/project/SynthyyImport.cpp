#include "project/SynthyyImport.h"
#include "project/ProjectJson.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace ddaw::project {

using json = nlohmann::json;

namespace {

template <class T>
T get(const json& j, const char* key, T def) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    return it->get<T>();
}

// synthyy's loose booleans: true/false or a number (0 = false).
bool getBool(const json& j, const char* key, bool def) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0.0;
    return def;
}

std::optional<double> getOptNum(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return std::nullopt;
    return it->get<double>();
}

std::map<std::string, double> numMap(const json& j) {
    std::map<std::string, double> m;
    if (j.is_object())
        for (auto& [k, v] : j.items()) {
            if (v.is_number()) m[k] = v.get<double>();
            else if (v.is_boolean()) m[k] = v.get<bool>() ? 1.0 : 0.0;
        }
    return m;
}

DeviceSpec device(const json& j) {
    DeviceSpec d;
    d.uid = static_cast<Uid>(get<double>(j, "uid", 0));
    d.id = get<std::string>(j, "id", "");
    d.type = get<std::string>(j, "type", "");
    d.on = getBool(j, "on", true);
    if (auto it = j.find("params"); it != j.end()) d.params = numMap(*it);
    d.outDb = getOptNum(j, "out");
    d.srcTrack = get<std::string>(j, "srcTrack", "");
    d.srcPitch = getOptNum(j, "srcPitch");
    d.keyHpf = getOptNum(j, "keyHpf");
    d.sampleId = get<std::string>(j, "sampleId", "");
    d.sampleName = get<std::string>(j, "sampleName", "");
    auto strMap = [&](const char* key) {
        std::map<std::string, std::string> m;
        if (auto it = j.find(key); it != j.end() && it->is_object())
            for (auto& [k, v] : it->items()) if (v.is_string()) m[k] = v.get<std::string>();
        return m;
    };
    d.padSamples = strMap("padSamples");
    d.padNames = strMap("padNames");
    d.plugin = get<std::string>(j, "plugin", "");
    d.pluginName = get<std::string>(j, "pluginName", "");
    d.pluginState = get<std::string>(j, "pluginState", "");
    return d;
}

std::vector<DeviceSpec> devices(const json& j, const char* key) {
    std::vector<DeviceSpec> out;
    if (auto it = j.find(key); it != j.end() && it->is_array())
        for (auto& x : *it) out.push_back(device(x));
    return out;
}

AutoMap autoMap(const json& j, const char* key) {
    AutoMap m;
    if (auto it = j.find(key); it != j.end() && it->is_object())
        for (auto& [k, pts] : it->items()) {
            if (!pts.is_array()) continue;
            std::vector<AutoPoint> v;
            for (auto& p : pts) v.push_back({get<double>(p, "t", 0), get<double>(p, "v", 0)});
            m[k] = std::move(v);
        }
    return m;
}

std::vector<ModTarget> modTargets(const json& j);

PerfSpec perfSpec(const json& jp) {
    PerfSpec ps;
    ps.source = get<std::string>(jp, "source", "f0");
    ps.srcMin = get<double>(jp, "min", 0);
    ps.srcMax = get<double>(jp, "max", 0);
    ps.on = getBool(jp, "on", true);
    ps.record = getBool(jp, "rec", false);
    if (auto tg = jp.find("targets"); tg != jp.end()) ps.targets = modTargets(*tg);
    return ps;
}

MorphSpec morphSpec(const json& jm) {
    MorphSpec m;
    m.name = get<std::string>(jm, "name", "");
    m.on = getBool(jm, "on", true);
    m.x = get<double>(jm, "x", 0.5);
    m.y = get<double>(jm, "y", 0.5);
    m.method = get<std::string>(jm, "method", "idw");
    m.power = get<double>(jm, "power", 2.0);
    m.width = get<double>(jm, "width", 0.35);
    if (auto tg = jm.find("targets"); tg != jm.end()) m.targets = modTargets(*tg);
    if (auto c = jm.find("curves"); c != jm.end() && c->is_array()) for (auto& v : *c) m.curves.push_back(v.is_number() ? v.get<double>() : 1.0);
    if (auto an = jm.find("anchors"); an != jm.end() && an->is_array())
        for (auto& ja : *an) {
            MorphAnchor a;
            a.name = get<std::string>(ja, "name", "");
            a.x = get<double>(ja, "x", 0.5);
            a.y = get<double>(ja, "y", 0.5);
            if (auto v = ja.find("values"); v != ja.end() && v->is_array()) for (auto& e : *v) a.values.push_back(e.is_number() ? e.get<double>() : 0.0);
            m.anchors.push_back(std::move(a));
        }
    return m;
}

ARateSpec arateSpec(const json& ja) {
    ARateSpec a;
    a.id = get<std::string>(ja, "id", "");
    a.on = getBool(ja, "on", true);
    a.source = get<std::string>(ja, "source", "osc");
    a.shape = static_cast<int>(get<double>(ja, "shape", 0));
    a.hz = get<double>(ja, "hz", 220);
    a.srcTrack = get<std::string>(ja, "track", "");
    a.follow = getBool(ja, "follow", false);
    a.attackMs = get<double>(ja, "attackMs", 5);
    a.releaseMs = get<double>(ja, "releaseMs", 80);
    a.depth = get<double>(ja, "depth", 0.5);
    if (auto tg = ja.find("target"); tg != ja.end() && tg->is_object()) {
        a.target.dest = get<std::string>(*tg, "dest", "");
        a.target.fxId = get<std::string>(*tg, "fxId", "");
        a.target.pkey = get<std::string>(*tg, "pkey", "");
    }
    return a;
}

Clip clip(const json& jc) {
    Clip c;
    c.len = get<double>(jc, "len", 384);
    c.env = autoMap(jc, "env");
    if (auto au = jc.find("audio"); au != jc.end() && au->is_object()) {
        AudioClipData a;
        a.sampleId = get<std::string>(*au, "sampleId", "");
        a.sampleName = get<std::string>(*au, "sampleName", "");
        a.gainDb = get<double>(*au, "gainDb", 0);
        a.pitch = get<double>(*au, "pitch", 0);
        a.rev = get<double>(*au, "rev", 0);
        a.loop = get<double>(*au, "loop", 0);
        a.fadeIn = get<double>(*au, "fadeIn", 0);
        a.fadeOut = get<double>(*au, "fadeOut", 0);
        a.offset = getOptNum(*au, "offset");
        a.dur = getOptNum(*au, "dur");
        a.cents = getOptNum(*au, "cents");
        a.xfade = getOptNum(*au, "xfade");
        c.audio = std::move(a);
    }
    if (auto nt = jc.find("notes"); nt != jc.end() && nt->is_object())
        for (auto& [id, jn] : nt->items()) {
            Note n;
            n.uid = static_cast<Uid>(get<double>(jn, "uid", 0));
            n.pitch = static_cast<int>(get<double>(jn, "p", 60));
            n.startTicks = get<double>(jn, "s", 0);
            n.durTicks = get<double>(jn, "d", 0);
            n.velocity = get<double>(jn, "v", 1);
            n.probability = get<double>(jn, "pr", 1);
            for (const auto& [name, curve] : {std::pair<const char*, std::vector<ExprPoint>*>{"bend", &n.bend}, {"slide", &n.slide}, {"pressure", &n.pressure}})
                if (auto it = jn.find(name); it != jn.end() && it->is_array())
                    for (auto& pt : *it) curve->push_back({get<double>(pt, "t", 0), get<double>(pt, "v", 0)});
            c.notes.push_back(n);
        }
    return c;
}

PerfSpec perfSpec(const json& jp);   // below

std::vector<ModTarget> modTargets(const json& j) {
    std::vector<ModTarget> out;
    if (j.is_array())
        for (auto& t : j) out.push_back({get<std::string>(t, "dest", ""), get<std::string>(t, "fxId", ""), get<std::string>(t, "pkey", "")});
    return out;
}

TrackKind kindOf(const std::string& s) {
    if (s == "drum") return TrackKind::Drum;
    if (s == "audio") return TrackKind::Audio;
    if (s == "bus") return TrackKind::Bus;
    return TrackKind::Synth;
}

Project project(const json& j) {
    Project p;
    if (auto it = j.find("meta"); it != j.end()) {
        p.meta.title = get<std::string>(*it, "title", "");
        p.meta.bpm = get<double>(*it, "bpm", 120);
        p.meta.swing = get<double>(*it, "swing", 0);
        p.meta.swingSubdivision = get<std::string>(*it, "swingSubdivision", "16n");
        p.meta.humanize = get<double>(*it, "humanize", 0);
        p.meta.root = get<double>(*it, "root", 9);
        p.meta.scale = get<std::string>(*it, "scale", "minor");
        p.meta.launchQ = get<double>(*it, "launchQ", 1);
        p.meta.masterGainDb = get<double>(*it, "masterGain", 0);
        p.meta.loopOn = getBool(*it, "loopOn", false);
        p.meta.loopStart = get<double>(*it, "loopStart", 0);
        p.meta.loopEnd = get<double>(*it, "loopEnd", 0);
        p.meta.tsTop = static_cast<int>(get<double>(*it, "tsTop", 4));
        p.meta.tsBottom = static_cast<int>(get<double>(*it, "tsBottom", 4));
    }
    if (!j.contains("tracks") || !j["tracks"].is_array()) throw std::runtime_error("project has no tracks array");
    for (auto& jt : j["tracks"]) {
        Track t;
        t.uid = static_cast<Uid>(get<double>(jt, "uid", 0));
        t.id = get<std::string>(jt, "id", "");
        t.name = get<std::string>(jt, "name", "");
        if (t.id.empty()) t.id = t.name;
        t.kind = kindOf(get<std::string>(jt, "kind", "synth"));
        if (auto it = jt.find("inst"); it != jt.end()) t.inst = device(*it);
        t.fx = devices(jt, "fx");
        t.midifx = devices(jt, "midifx");
        t.gainDb = get<double>(jt, "gain", 0);
        t.pan = get<double>(jt, "pan", 0);
        t.sendA = get<double>(jt, "sendA", 0);
        t.sendB = get<double>(jt, "sendB", 0);
        t.mute = getBool(jt, "mute", false);
        t.solo = getBool(jt, "solo", false);
        t.output = get<std::string>(jt, "output", "");
        if (auto it = jt.find("sends"); it != jt.end()) t.sends = numMap(*it);
        const std::string sb = get<std::string>(jt, "send", "");
        t.send = sb == "A" ? SendBus::A : sb == "B" ? SendBus::B : sb == "F" ? SendBus::F : SendBus::None;
        if (auto it = jt.find("lfos"); it != jt.end() && it->is_array())
            for (auto& jl : *it) {
                LfoSpec l;
                l.id = get<std::string>(jl, "id", "");
                l.on = getBool(jl, "on", true);
                l.shape = static_cast<int>(get<double>(jl, "shape", 0));
                l.sync = getBool(jl, "sync", false);
                l.rate = get<double>(jl, "rate", 5);
                l.hz = get<double>(jl, "hz", 1);
                l.depth = get<double>(jl, "depth", 0.5);
                l.phase = get<double>(jl, "phase", 0);
                l.dest = get<std::string>(jl, "dest", "");
                l.fxId = get<std::string>(jl, "fxId", "");
                l.pkey = get<std::string>(jl, "pkey", "");
                if (auto tg = jl.find("targets"); tg != jl.end()) l.targets = modTargets(*tg);
                t.lfos.push_back(std::move(l));
            }
        if (auto it = jt.find("macros"); it != jt.end() && it->is_array())
            for (auto& jm : *it) {
                MacroSpec m;
                m.name = get<std::string>(jm, "name", "");
                m.value = get<double>(jm, "value", 0);
                if (auto tg = jm.find("targets"); tg != jm.end()) m.targets = modTargets(*tg);
                t.macros.push_back(std::move(m));
            }
        if (auto it = jt.find("perf"); it != jt.end() && it->is_array())
            for (auto& jp : *it) t.perf.push_back(perfSpec(jp));
        if (auto it = jt.find("arate"); it != jt.end() && it->is_array())
            for (auto& ja : *it) t.arate.push_back(arateSpec(ja));
        if (auto it = jt.find("morph"); it != jt.end() && it->is_array())
            for (auto& jm : *it) t.morph.push_back(morphSpec(jm));
        t.autoLanes = autoMap(jt, "auto");
        p.tracks.push_back(std::move(t));
    }
    if (auto it = j.find("scenes"); it != j.end() && it->is_array())
        for (auto& s : *it) p.scenes.push_back(get<std::string>(s, "id", ""));
    if (auto it = j.find("clips"); it != j.end() && it->is_object())
        for (auto& [key, jc] : it->items()) p.clips.emplace(key, clip(jc));
    if (auto it = j.find("arr"); it != j.end() && it->is_object())
        for (auto& [key, ja] : it->items()) {
            ArrClip a;
            if (auto c = ja.find("clip"); c != ja.end()) a.clip = clip(*c);
            a.trackId = get<std::string>(ja, "trackId", "");
            a.start = get<double>(ja, "start", 0);
            p.arr.emplace(key, std::move(a));
        }
    if (auto it = j.find("returns"); it != j.end() && it->is_array())
        for (auto& jr : *it) {
            Return r;
            r.id = get<std::string>(jr, "id", "");
            r.name = get<std::string>(jr, "name", "");
            r.fxType = get<std::string>(jr, "fxType", "");
            if (auto pr = jr.find("params"); pr != jr.end()) r.params = numMap(*pr);
            r.gainDb = get<double>(jr, "gain", 0);
            p.returns.push_back(std::move(r));
        }
    p.masterFx = devices(j, "masterFx");
    p.masterAuto = autoMap(j, "masterAuto");
    if (auto it = j.find("bindings"); it != j.end() && it->is_array())
        for (auto& jb : *it) p.bindings.push_back(bindingFromJson(jb));
    return p;
}

}  // namespace

Project projectFromJson(const nlohmann::json& j) { return project(j); }
Clip clipFromJson(const nlohmann::json& j) { return clip(j); }
DeviceSpec deviceFromJson(const nlohmann::json& j) { return device(j); }
Track trackFromJson(const nlohmann::json& j) {
    // reuse the project parser on a one-track wrapper so there is a single track parser
    json wrap = {{"tracks", json::array({j})}};
    return project(wrap).tracks.at(0);
}
LfoSpec lfoFromJson(const nlohmann::json& jl) {
    LfoSpec l;
    l.id = get<std::string>(jl, "id", "");
    l.on = getBool(jl, "on", true);
    l.shape = static_cast<int>(get<double>(jl, "shape", 0));
    l.sync = getBool(jl, "sync", false);
    l.rate = get<double>(jl, "rate", 5);
    l.hz = get<double>(jl, "hz", 1);
    l.depth = get<double>(jl, "depth", 0.5);
    l.phase = get<double>(jl, "phase", 0);
    l.dest = get<std::string>(jl, "dest", "");
    l.fxId = get<std::string>(jl, "fxId", "");
    l.pkey = get<std::string>(jl, "pkey", "");
    if (auto tg = jl.find("targets"); tg != jl.end()) l.targets = modTargets(*tg);
    return l;
}
MacroSpec macroFromJson(const nlohmann::json& jm) {
    MacroSpec m;
    m.name = get<std::string>(jm, "name", "");
    m.value = get<double>(jm, "value", 0);
    if (auto tg = jm.find("targets"); tg != jm.end()) m.targets = modTargets(*tg);
    return m;
}
PerfSpec perfFromJson(const nlohmann::json& jp) { return perfSpec(jp); }
ARateSpec arateFromJson(const nlohmann::json& ja) { return arateSpec(ja); }
MorphSpec morphFromJson(const nlohmann::json& jm) { return morphSpec(jm); }
ControlBinding bindingFromJson(const nlohmann::json& jb) {
    ControlBinding b;
    b.source = get<std::string>(jb, "source", "");
    b.target = get<std::string>(jb, "target", "");
    b.min = get<double>(jb, "min", 0);
    b.max = get<double>(jb, "max", 1);
    b.invert = getBool(jb, "invert", false);
    return b;
}
std::vector<AutoPoint> pointsFromJson(const nlohmann::json& pts) {
    std::vector<AutoPoint> v;
    if (pts.is_array()) for (auto& p : pts) v.push_back({get<double>(p, "t", 0), get<double>(p, "v", 0)});
    return v;
}

Fixture importFixtureJson(const std::string& text) {
    json j = json::parse(text);  // throws json::parse_error (a std::exception)
    Fixture f;
    if (j.contains("project")) {
        f.name = get<std::string>(j, "name", "");
        if (auto it = j.find("scope"); it != j.end()) {
            f.scope.kind = get<std::string>(*it, "kind", "scene");
            f.scope.sceneId = get<std::string>(*it, "sceneId", "");
        }
        f.project = project(j["project"]);
    } else {
        f.project = project(j);  // bare ProjectJSON
    }
    return f;
}

Fixture importFixtureFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    return importFixtureJson(ss.str());
}

}  // namespace ddaw::project
