#include "app/model/Edit.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "app/model/Catalog.h"
#include "project/ProjectJson.h"

namespace ddaw::app::edit {

using nlohmann::json;

json ClipRef::toJson() const {
    if (!arrKey.empty()) return {{"arr", arrKey}};
    return {{"track", track}, {"scene", scene}};
}

const project::Track* findTrack(const Project& p, Uid uid) {
    for (auto& t : p.tracks) if (t.uid == uid) return &t;
    return nullptr;
}
const project::Track* findTrackById(const Project& p, const std::string& id) {
    for (auto& t : p.tracks) if (t.id == id) return &t;
    return nullptr;
}

const project::Clip* findClip(const Project& p, const ClipRef& r) {
    if (!r.arrKey.empty()) {
        auto it = p.arr.find(r.arrKey);
        return it == p.arr.end() ? nullptr : &it->second.clip;
    }
    const auto* t = findTrack(p, r.track);
    if (!t) return nullptr;
    auto it = p.clips.find(t->id + "|" + r.scene);
    return it == p.clips.end() ? nullptr : &it->second;
}

namespace {
template <class F> std::string unique(const std::string& prefix, F taken) {
    for (int i = 1;; ++i) {
        std::string id = prefix + std::to_string(i);
        if (!taken(id)) return id;
    }
}
}  // namespace

std::string uniqueTrackId(const Project& p) {
    return unique("t", [&](const std::string& id) { return findTrackById(p, id) != nullptr; });
}
std::string uniqueSceneId(const Project& p) {
    return unique("s", [&](const std::string& id) { return std::find(p.scenes.begin(), p.scenes.end(), id) != p.scenes.end(); });
}
std::string uniqueArrKey(const Project& p) {
    return unique("a", [&](const std::string& id) { return p.arr.count(id) != 0; });
}
std::string uniqueDeviceId(const Project& p, const std::string& type) {
    std::set<std::string> used;
    for (auto& t : p.tracks) {
        used.insert(t.inst.id);
        for (auto& d : t.fx) used.insert(d.id);
        for (auto& d : t.midifx) used.insert(d.id);
    }
    for (auto& d : p.masterFx) used.insert(d.id);
    if (!used.count(type)) return type;
    return unique(type, [&](const std::string& id) { return used.count(id) != 0; });
}

Command addTrack(const Project& p, project::TrackKind kind, std::string name) {
    project::Track t;
    t.id = uniqueTrackId(p);
    t.kind = kind;
    const char* inst = kind == project::TrackKind::Drum ? "drum" : kind == project::TrackKind::Bus ? "audiobus" : kind == project::TrackKind::Synth ? "poly" : "";
    const char* label = kind == project::TrackKind::Drum ? "Drums" : kind == project::TrackKind::Bus ? "Bus" : kind == project::TrackKind::Audio ? "Audio" : "Synth";
    if (name.empty()) {
        int n = 1;
        for (auto& e : p.tracks) if (e.kind == kind) ++n;
        name = std::string(label) + " " + std::to_string(n);
    }
    t.name = std::move(name);
    t.inst.type = inst;
    t.inst.id = *inst ? "inst" : "";
    return {"track.insert", {{"index", p.tracks.size()}, {"track", project::trackToJson(t)}}};
}
Command removeTrack(Uid uid) { return {"track.remove", {{"uid", uid}}}; }
Command addScene(const Project& p) { return {"scene.insert", {{"index", p.scenes.size()}, {"id", uniqueSceneId(p)}}}; }
Command removeScene(const std::string& id) { return {"scene.remove", {{"id", id}}}; }

Command addDevice(const Project& p, Uid track, const std::string& chain, const std::string& type, int index) {
    project::DeviceSpec d;
    d.type = type;
    d.id = uniqueDeviceId(p, type);
    json a = {{"chain", chain}, {"device", project::deviceToJson(d)}};
    if (index >= 0) a["index"] = index;
    else {
        size_t n = 0;
        if (chain == "master") n = p.masterFx.size();
        else if (const auto* t = findTrack(p, track)) n = chain == "midifx" ? t->midifx.size() : t->fx.size();
        a["index"] = n;
    }
    if (chain != "master") a["track"] = track;
    return {"device.insert", a};
}
Command removeDevice(Uid device) { return {"device.remove", {{"uid", device}}}; }
Command setInstrument(const Project& p, Uid track, const std::string& type) {
    project::DeviceSpec d;
    d.type = type;
    d.id = "inst";
    (void)p;
    return {"inst.set", {{"track", track}, {"device", project::deviceToJson(d, true)}}};
}

Command setPluginInstrument(Uid track, const std::string& pluginId, const std::string& name) {
    project::DeviceSpec d;
    d.type = "plugin";
    d.id = "inst";
    d.plugin = pluginId;
    d.pluginName = name;
    return {"inst.set", {{"track", track}, {"device", project::deviceToJson(d, true)}}};
}

Command addPluginEffect(const Project& p, Uid track, const std::string& chain, const std::string& pluginId, const std::string& name) {
    auto c = addDevice(p, track, chain, "plugin");
    auto d = c.args["device"];
    d["plugin"] = pluginId;
    d["pluginName"] = name;
    c.args["device"] = d;
    return c;
}

Command newSessionClip(Uid track, const std::string& scene, double len) {
    project::Clip c;
    c.len = len;
    return {"clip.set", {{"track", track}, {"scene", scene}, {"clip", project::clipToJson(c)}}};
}
Command clearSessionClip(Uid track, const std::string& scene) { return {"clip.set", {{"track", track}, {"scene", scene}, {"clip", nullptr}}}; }

Command newArrClip(const Project& p, Uid track, double start, double len) {
    const auto* t = findTrack(p, track);
    project::Clip c;
    c.len = len;
    return {"arrclip.set", {{"key", uniqueArrKey(p)}, {"arr", {{"trackId", t ? t->id : ""}, {"start", start}, {"clip", project::clipToJson(c)}}}}};
}
Command moveArrClip(const Project& p, const std::string& key, double start, std::optional<Uid> toTrack) {
    auto it = p.arr.find(key);
    if (it == p.arr.end()) throw std::invalid_argument("no arrangement clip " + key);
    std::string trackId = it->second.trackId;
    if (toTrack) if (const auto* t = findTrack(p, *toTrack)) trackId = t->id;
    return {"arrclip.set", {{"key", key}, {"arr", {{"trackId", trackId}, {"start", std::max(0.0, start)}, {"clip", project::clipToJson(it->second.clip)}}}}};
}
Command resizeArrClip(const Project& p, const std::string& key, double len) {
    auto it = p.arr.find(key);
    if (it == p.arr.end()) throw std::invalid_argument("no arrangement clip " + key);
    project::Clip c = it->second.clip;
    c.len = std::max(len, kPpq / 4);
    return {"arrclip.set", {{"key", key}, {"arr", {{"trackId", it->second.trackId}, {"start", it->second.start}, {"clip", project::clipToJson(c)}}}}};
}
Command removeArrClip(const std::string& key) { return {"arrclip.set", {{"key", key}, {"arr", nullptr}}}; }

Command setClipLength(const ClipRef& r, const Project& p, double len) {
    if (!r.arrKey.empty()) return resizeArrClip(p, r.arrKey, len);
    const auto* c = findClip(p, r);
    if (!c) throw std::invalid_argument("no such clip");
    project::Clip n = *c;
    n.len = std::max(len, kPpq / 4);
    return {"clip.set", {{"track", r.track}, {"scene", r.scene}, {"clip", project::clipToJson(n)}}};
}

namespace {
json noteJson(const project::Note& n) {
    project::Clip c; c.notes = {n};
    return project::clipToJson(c)["notes"].begin().value();
}
}  // namespace

Command addNote(const ClipRef& r, int pitch, double start, double dur, double vel) {
    project::Note n; n.pitch = pitch; n.startTicks = start; n.durTicks = dur; n.velocity = vel;
    return {"note.add", {{"clip", r.toJson()}, {"note", noteJson(n)}}};
}
Command editNote(const ClipRef& r, const project::Note& n) { return {"note.edit", {{"clip", r.toJson()}, {"uid", n.uid}, {"note", noteJson(n)}}}; }
Command addNoteCopy(const ClipRef& r, project::Note n) {
    n.uid = 0;
    return {"note.add", {{"clip", r.toJson()}, {"note", noteJson(n)}}};
}
Command removeNote(const ClipRef& r, Uid uid) { return {"note.remove", {{"clip", r.toJson()}, {"uid", uid}}}; }

double snapTicks(double t, double g) { return g > 0 ? std::round(t / g) * g : t; }
double snapFloor(double t, double g) { return g > 0 ? std::floor(t / g + 1e-9) * g : t; }

}  // namespace ddaw::app::edit
