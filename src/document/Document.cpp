#include "document/Document.h"

#include <algorithm>
#include <stdexcept>

#include "project/ProjectJson.h"

namespace ddaw::document {

using nlohmann::json;
using namespace ddaw::project;

namespace {

[[noreturn]] void bad(const std::string& m) { throw std::invalid_argument(m); }

Uid uidArg(const json& a, const char* key) {
    if (!a.contains(key) || !a[key].is_number()) bad(std::string("missing numeric '") + key + "'");
    return a[key].get<Uid>();
}
size_t indexArg(const json& a, const char* key, size_t size, bool allowEnd) {
    if (!a.contains(key) || !a[key].is_number()) bad(std::string("missing numeric '") + key + "'");
    const auto v = a[key].get<long long>();
    if (v < 0 || static_cast<size_t>(v) > size || (!allowEnd && static_cast<size_t>(v) == size)) bad(std::string("index out of range: ") + key);
    return static_cast<size_t>(v);
}
const json& need(const json& a, const char* key) {
    if (!a.contains(key)) bad(std::string("missing '") + key + "'");
    return a[key];
}

// ---- JSON <-> scalar helpers for meta / track fields ----
json metaGet(const Meta& m, const std::string& f) {
    if (f == "title") return m.title;
    if (f == "bpm") return m.bpm;
    if (f == "swing") return m.swing;
    if (f == "swingSubdivision") return m.swingSubdivision;
    if (f == "humanize") return m.humanize;
    if (f == "root") return m.root;
    if (f == "scale") return m.scale;
    if (f == "launchQ") return m.launchQ;
    if (f == "masterGain") return m.masterGainDb;
    if (f == "loopOn") return m.loopOn;
    if (f == "loopStart") return m.loopStart;
    if (f == "loopEnd") return m.loopEnd;
    if (f == "tsTop") return m.tsTop;
    if (f == "tsBottom") return m.tsBottom;
    bad("unknown meta field '" + f + "'");
}
void metaSet(Meta& m, const std::string& f, const json& v) {
    auto num = [&] { if (!v.is_number()) bad("meta." + f + " needs a number"); return v.get<double>(); };
    auto str = [&] { if (!v.is_string()) bad("meta." + f + " needs a string"); return v.get<std::string>(); };
    auto flag = [&] { if (!v.is_boolean()) bad("meta." + f + " needs a boolean"); return v.get<bool>(); };
    if (f == "title") m.title = str();
    else if (f == "bpm") { const double b = num(); if (b < 20 || b > 999) bad("bpm must be 20..999"); m.bpm = b; }
    else if (f == "swing") m.swing = num();
    else if (f == "swingSubdivision") m.swingSubdivision = str();
    else if (f == "humanize") m.humanize = num();
    else if (f == "root") m.root = num();
    else if (f == "scale") m.scale = str();
    else if (f == "launchQ") m.launchQ = num();
    else if (f == "masterGain") m.masterGainDb = num();
    else if (f == "loopOn") m.loopOn = flag();
    else if (f == "loopStart") m.loopStart = num();
    else if (f == "loopEnd") m.loopEnd = num();
    else if (f == "tsTop") { const int t = static_cast<int>(num()); if (t < 1 || t > 32) bad("tsTop must be 1..32"); m.tsTop = t; }
    else if (f == "tsBottom") { const int t = static_cast<int>(num()); if (t != 1 && t != 2 && t != 4 && t != 8 && t != 16) bad("tsBottom must be 1, 2, 4, 8 or 16"); m.tsBottom = t; }
    else bad("unknown meta field '" + f + "'");
}

json trackGet(const Track& t, const std::string& f) {
    if (f == "name") return t.name;
    if (f == "kind") return t.kind == TrackKind::Drum ? "drum" : t.kind == TrackKind::Audio ? "audio" : t.kind == TrackKind::Bus ? "bus" : "synth";
    if (f == "gain") return t.gainDb;
    if (f == "pan") return t.pan;
    if (f == "mute") return t.mute;
    if (f == "solo") return t.solo;
    if (f == "sendA") return t.sendA;
    if (f == "sendB") return t.sendB;
    if (f == "output") return t.output;
    if (f == "send") return t.send == SendBus::A ? "A" : t.send == SendBus::B ? "B" : t.send == SendBus::F ? "F" : "";
    bad("unknown track field '" + f + "'");
}
void trackSet(Track& t, const std::string& f, const json& v) {
    auto num = [&] { if (!v.is_number()) bad("track." + f + " needs a number"); return v.get<double>(); };
    auto str = [&] { if (!v.is_string()) bad("track." + f + " needs a string"); return v.get<std::string>(); };
    auto flag = [&] { if (!v.is_boolean()) bad("track." + f + " needs a boolean"); return v.get<bool>(); };
    if (f == "name") t.name = str();
    else if (f == "kind") { const auto k = str(); t.kind = k == "drum" ? TrackKind::Drum : k == "audio" ? TrackKind::Audio : k == "bus" ? TrackKind::Bus : TrackKind::Synth; }
    else if (f == "gain") t.gainDb = num();
    else if (f == "pan") t.pan = std::clamp(num(), -1.0, 1.0);
    else if (f == "mute") t.mute = flag();
    else if (f == "solo") t.solo = flag();
    else if (f == "sendA") t.sendA = std::max(num(), 0.0);
    else if (f == "sendB") t.sendB = std::max(num(), 0.0);
    else if (f == "output") t.output = str();
    else if (f == "send") { const auto k = str(); t.send = k == "A" ? SendBus::A : k == "B" ? SendBus::B : k == "F" ? SendBus::F : SendBus::None; }
    else bad("unknown track field '" + f + "'");
}

enum class Chain { Inst, Fx, Midifx, Master };
struct Loc { Chain chain; int track; size_t index; };

const char* chainName(Chain c) { return c == Chain::Fx ? "fx" : c == Chain::Midifx ? "midifx" : "master"; }

void mergeInfo(ChangeInfo& into, const ChangeInfo& from) {
    into.structural |= from.structural;
    for (const auto& pe : from.params) {
        auto it = std::find_if(into.params.begin(), into.params.end(), [&](const ParamEdit& e) { return e.key == pe.key; });
        if (it != into.params.end()) it->value = pe.value; else into.params.push_back(pe);
    }
    if (from.tempo) into.tempo = from.tempo;
}

ChangeInfo structuralChange() { ChangeInfo i; i.structural = true; return i; }

}  // namespace

// ---------------------------------------------------------------- Document

Document::Document() = default;

Document::Document(Project p, Uid nextUid) : p_(std::move(p)), nextUid_(std::max<Uid>({nextUid, maxUid(p_) + 1, 1})) {
    assignUids(p_, nextUid_);
}

const Track* Document::findTrack(Uid uid) const {
    for (auto& t : p_.tracks) if (t.uid == uid) return &t;
    return nullptr;
}

const DeviceSpec* Document::findDevice(Uid uid) const {
    for (auto& t : p_.tracks) {
        if (t.inst.uid == uid) return &t.inst;
        for (auto& d : t.fx) if (d.uid == uid) return &d;
        for (auto& d : t.midifx) if (d.uid == uid) return &d;
    }
    for (auto& d : p_.masterFx) if (d.uid == uid) return &d;
    return nullptr;
}

Document::Exec Document::execute(const Command& c) {
    const json& a = c.args;
    Exec ex;
    ex.forward = c;

    auto trackIdx = [&](Uid uid) -> size_t {
        for (size_t i = 0; i < p_.tracks.size(); ++i) if (p_.tracks[i].uid == uid) return i;
        bad("no track with uid " + std::to_string(uid));
    };
    auto locate = [&](Uid uid) -> Loc {
        for (size_t t = 0; t < p_.tracks.size(); ++t) {
            auto& tr = p_.tracks[t];
            if (tr.inst.uid == uid) return {Chain::Inst, int(t), 0};
            for (size_t i = 0; i < tr.fx.size(); ++i) if (tr.fx[i].uid == uid) return {Chain::Fx, int(t), i};
            for (size_t i = 0; i < tr.midifx.size(); ++i) if (tr.midifx[i].uid == uid) return {Chain::Midifx, int(t), i};
        }
        for (size_t i = 0; i < p_.masterFx.size(); ++i) if (p_.masterFx[i].uid == uid) return {Chain::Master, -1, i};
        bad("no device with uid " + std::to_string(uid));
    };
    auto deviceAt = [&](const Loc& l) -> DeviceSpec& {
        switch (l.chain) {
            case Chain::Inst: return p_.tracks[size_t(l.track)].inst;
            case Chain::Fx: return p_.tracks[size_t(l.track)].fx[l.index];
            case Chain::Midifx: return p_.tracks[size_t(l.track)].midifx[l.index];
            default: return p_.masterFx[l.index];
        }
    };
    auto fxKeyPrefix = [&](const Loc& l) -> std::string {
        const auto& d = deviceAt(l);
        const std::string fid = d.id.empty() ? d.type : d.id;
        if (l.chain == Chain::Inst) return p_.tracks[size_t(l.track)].id + "|inst|";
        if (l.chain == Chain::Fx) return p_.tracks[size_t(l.track)].id + "|" + fid + "|";
        if (l.chain == Chain::Master) return "master|" + fid + "|";
        return "";  // midi fx are not live-addressable
    };
    // clip addressing: {track, scene} for a session clip, {arr: key} for an arrangement clip
    auto clipRef = [&](const json& ref) -> Clip* {
        if (ref.contains("arr")) {
            auto it = p_.arr.find(ref["arr"].get<std::string>());
            if (it == p_.arr.end()) bad("no arrangement clip '" + ref["arr"].get<std::string>() + "'");
            return &it->second.clip;
        }
        const auto& t = p_.tracks[trackIdx(uidArg(ref, "track"))];
        const std::string scene = need(ref, "scene").get<std::string>();
        auto it = p_.clips.find(t.id + "|" + scene);
        if (it == p_.clips.end()) bad("no clip at " + t.id + "|" + scene);
        return &it->second;
    };
    auto sessionKey = [&](const json& ref) { return p_.tracks[trackIdx(uidArg(ref, "track"))].id + "|" + need(ref, "scene").get<std::string>(); };

    const std::string& k = c.kind;

    if (k == "meta.set") {
        const std::string f = need(a, "field").get<std::string>();
        const json old = metaGet(p_.meta, f);
        metaSet(p_.meta, f, need(a, "value"));
        ex.inverse = {"meta.set", {{"field", f}, {"value", old}}};
        if (f == "bpm") ex.info.tempo = p_.meta.bpm;
        else if (f == "masterGain") ex.info.params.push_back({"master|gain", p_.meta.masterGainDb});
        else if (f != "title") ex.info = structuralChange();
    } else if (k == "track.insert") {
        Track t = trackFromJson(need(a, "track"));
        if (t.id.empty()) bad("a track needs an id");
        for (auto& o : p_.tracks) if (o.id == t.id) bad("a track with id '" + t.id + "' already exists");
        const size_t idx = indexArg(a, "index", p_.tracks.size(), true);
        const std::string id = t.id;
        p_.tracks.insert(p_.tracks.begin() + static_cast<std::ptrdiff_t>(idx), std::move(t));
        if (a.contains("clips")) for (auto& [key, jc] : a["clips"].items()) p_.clips[key] = clipFromJson(jc);
        if (a.contains("arr"))
            for (auto& [key, ja] : a["arr"].items()) {
                ArrClip ac; ac.trackId = ja.value("trackId", id); ac.start = ja.value("start", 0.0);
                if (ja.contains("clip")) ac.clip = clipFromJson(ja["clip"]);
                p_.arr[key] = std::move(ac);
            }
        assignUids(p_, nextUid_);
        const Track& ins = p_.tracks[idx];
        json fwd = {{"index", idx}, {"track", trackToJson(ins)}, {"clips", json::object()}, {"arr", json::object()}};
        for (auto& [key, cl] : p_.clips) if (key.rfind(id + "|", 0) == 0) fwd["clips"][key] = clipToJson(cl);
        for (auto& [key, ac] : p_.arr) if (ac.trackId == id) fwd["arr"][key] = {{"trackId", ac.trackId}, {"start", ac.start}, {"clip", clipToJson(ac.clip)}};
        ex.forward.args = fwd;
        ex.inverse = {"track.remove", {{"uid", ins.uid}}};
        ex.info = structuralChange();
    } else if (k == "track.remove") {
        const size_t idx = trackIdx(uidArg(a, "uid"));
        const Track t = p_.tracks[idx];
        json inv = {{"index", idx}, {"track", trackToJson(t)}, {"clips", json::object()}, {"arr", json::object()}};
        for (auto it = p_.clips.begin(); it != p_.clips.end();)
            if (it->first.rfind(t.id + "|", 0) == 0) { inv["clips"][it->first] = clipToJson(it->second); it = p_.clips.erase(it); } else ++it;
        for (auto it = p_.arr.begin(); it != p_.arr.end();)
            if (it->second.trackId == t.id) { inv["arr"][it->first] = {{"trackId", it->second.trackId}, {"start", it->second.start}, {"clip", clipToJson(it->second.clip)}}; it = p_.arr.erase(it); } else ++it;
        p_.tracks.erase(p_.tracks.begin() + static_cast<std::ptrdiff_t>(idx));
        ex.inverse = {"track.insert", inv};
        ex.info = structuralChange();
    } else if (k == "track.set") {
        const size_t idx = trackIdx(uidArg(a, "uid"));
        Track& t = p_.tracks[idx];
        const std::string f = need(a, "field").get<std::string>();
        const json old = trackGet(t, f);
        trackSet(t, f, need(a, "value"));
        ex.inverse = {"track.set", {{"uid", t.uid}, {"field", f}, {"value", old}}};
        if (f == "gain") ex.info.params.push_back({t.id + "|mix|gain", t.gainDb});
        else if (f == "pan") ex.info.params.push_back({t.id + "|mix|pan", t.pan});
        else if (f == "sendA") ex.info.params.push_back({t.id + "|mix|sendA", t.sendA});
        else if (f == "sendB") ex.info.params.push_back({t.id + "|mix|sendB", t.sendB});
        else if (f != "name") ex.info = structuralChange();
    } else if (k == "track.move") {
        const size_t from = trackIdx(uidArg(a, "uid"));
        const size_t to = indexArg(a, "index", p_.tracks.size(), false);
        Track t = std::move(p_.tracks[from]);
        p_.tracks.erase(p_.tracks.begin() + static_cast<std::ptrdiff_t>(from));
        p_.tracks.insert(p_.tracks.begin() + static_cast<std::ptrdiff_t>(to), std::move(t));
        ex.inverse = {"track.move", {{"uid", p_.tracks[to].uid}, {"index", from}}};
        ex.info = structuralChange();
    } else if (k == "device.insert") {
        const std::string chain = need(a, "chain").get<std::string>();
        DeviceSpec d = deviceFromJson(need(a, "device"));
        if (d.type.empty()) bad("a device needs a type");
        std::vector<DeviceSpec>* vec;
        json where = {{"chain", chain}};
        if (chain == "master") vec = &p_.masterFx;
        else if (chain == "fx" || chain == "midifx") {
            Track& t = p_.tracks[trackIdx(uidArg(a, "track"))];
            vec = chain == "fx" ? &t.fx : &t.midifx;
            where["track"] = t.uid;
        } else bad("unknown device chain '" + chain + "'");
        const size_t idx = indexArg(a, "index", vec->size(), true);
        vec->insert(vec->begin() + static_cast<std::ptrdiff_t>(idx), std::move(d));
        assignUids(p_, nextUid_);
        const DeviceSpec& ins = (*vec)[idx];
        ex.forward.args = where; ex.forward.args["index"] = idx; ex.forward.args["device"] = deviceToJson(ins);
        ex.inverse = {"device.remove", {{"uid", ins.uid}}};
        ex.info = structuralChange();
    } else if (k == "device.remove") {
        const Loc l = locate(uidArg(a, "uid"));
        if (l.chain == Chain::Inst) bad("an instrument cannot be removed; replace it with inst.set");
        const DeviceSpec d = deviceAt(l);
        json inv = {{"chain", chainName(l.chain)}, {"index", l.index}, {"device", deviceToJson(d)}};
        if (l.chain != Chain::Master) inv["track"] = p_.tracks[size_t(l.track)].uid;
        auto& vec = l.chain == Chain::Fx ? p_.tracks[size_t(l.track)].fx : l.chain == Chain::Midifx ? p_.tracks[size_t(l.track)].midifx : p_.masterFx;
        vec.erase(vec.begin() + static_cast<std::ptrdiff_t>(l.index));
        ex.inverse = {"device.insert", inv};
        ex.info = structuralChange();
    } else if (k == "device.set") {
        const Loc l = locate(uidArg(a, "uid"));
        DeviceSpec& d = deviceAt(l);
        const std::string f = need(a, "field").get<std::string>();
        const json& v = need(a, "value");
        json old;
        if (f == "on") { if (!v.is_boolean()) bad("device.on needs a boolean"); old = d.on; d.on = v.get<bool>(); ex.info = structuralChange(); }
        else if (f == "out") {
            old = d.outDb ? json(*d.outDb) : json(nullptr);
            if (v.is_null()) d.outDb.reset(); else if (v.is_number()) d.outDb = v.get<double>(); else bad("device.out needs a number or null");
            const std::string pre = fxKeyPrefix(l);
            if (pre.empty()) ex.info = structuralChange(); else ex.info.params.push_back({pre + "out", d.outDb.value_or(0.0)});
        }
        else if (f == "id") { if (!v.is_string()) bad("device.id needs a string"); old = d.id; d.id = v.get<std::string>(); ex.info = structuralChange(); }
        else if (f == "srcTrack") { if (!v.is_string()) bad("device.srcTrack needs a string"); old = d.srcTrack; d.srcTrack = v.get<std::string>(); ex.info = structuralChange(); }
        else if (f == "srcPitch") {
            old = d.srcPitch ? json(*d.srcPitch) : json(nullptr);
            if (v.is_null()) d.srcPitch.reset(); else if (v.is_number()) d.srcPitch = v.get<double>(); else bad("device.srcPitch needs a number or null");
            ex.info = structuralChange();
        }
        else bad("unknown device field '" + f + "'");
        ex.inverse = {"device.set", {{"uid", d.uid}, {"field", f}, {"value", old}}};
    } else if (k == "device.param") {
        const Loc l = locate(uidArg(a, "uid"));
        DeviceSpec& d = deviceAt(l);
        const std::string key = need(a, "key").get<std::string>();
        const json& v = need(a, "value");
        auto it = d.params.find(key);
        const json old = it == d.params.end() ? json(nullptr) : json(it->second);
        if (v.is_null()) d.params.erase(key); else if (v.is_number()) d.params[key] = v.get<double>(); else bad("device.param needs a number or null");
        ex.inverse = {"device.param", {{"uid", d.uid}, {"key", key}, {"value", old}}};
        const std::string pre = fxKeyPrefix(l);
        if (v.is_null() || pre.empty() || !d.on) ex.info = structuralChange();
        else ex.info.params.push_back({pre + key, v.get<double>()});
    } else if (k == "inst.set") {
        Track& t = p_.tracks[trackIdx(uidArg(a, "track"))];
        DeviceSpec d = deviceFromJson(need(a, "device"));
        const DeviceSpec old = t.inst;
        if (d.uid == 0) d.uid = nextUid_++;
        t.inst = std::move(d);
        ex.forward.args["device"] = deviceToJson(t.inst, true);
        ex.inverse = {"inst.set", {{"track", t.uid}, {"device", deviceToJson(old, true)}}};
        ex.info = structuralChange();
    } else if (k == "scene.insert") {
        const std::string id = need(a, "id").get<std::string>();
        if (id.empty() || std::find(p_.scenes.begin(), p_.scenes.end(), id) != p_.scenes.end()) bad("scene id empty or already used");
        const size_t idx = indexArg(a, "index", p_.scenes.size(), true);
        p_.scenes.insert(p_.scenes.begin() + static_cast<std::ptrdiff_t>(idx), id);
        if (a.contains("clips")) for (auto& [key, jc] : a["clips"].items()) p_.clips[key] = clipFromJson(jc);
        assignUids(p_, nextUid_);
        json fwd = {{"index", idx}, {"id", id}, {"clips", json::object()}};
        for (auto& [key, cl] : p_.clips) if (key.size() > id.size() && key.compare(key.size() - id.size() - 1, std::string::npos, "|" + id) == 0) fwd["clips"][key] = clipToJson(cl);
        ex.forward.args = fwd;
        ex.inverse = {"scene.remove", {{"id", id}}};
        ex.info = structuralChange();
    } else if (k == "scene.remove") {
        const std::string id = need(a, "id").get<std::string>();
        auto it = std::find(p_.scenes.begin(), p_.scenes.end(), id);
        if (it == p_.scenes.end()) bad("no scene '" + id + "'");
        const size_t idx = static_cast<size_t>(it - p_.scenes.begin());
        json inv = {{"index", idx}, {"id", id}, {"clips", json::object()}};
        for (auto ci = p_.clips.begin(); ci != p_.clips.end();)
            if (ci->first.size() > id.size() && ci->first.compare(ci->first.size() - id.size() - 1, std::string::npos, "|" + id) == 0) { inv["clips"][ci->first] = clipToJson(ci->second); ci = p_.clips.erase(ci); } else ++ci;
        p_.scenes.erase(it);
        ex.inverse = {"scene.insert", inv};
        ex.info = structuralChange();
    } else if (k == "clip.set") {
        const std::string key = sessionKey(a);
        const json& jc = need(a, "clip");
        if (!jc.is_null() && std::find(p_.scenes.begin(), p_.scenes.end(), need(a, "scene").get<std::string>()) == p_.scenes.end())
            bad("no scene '" + need(a, "scene").get<std::string>() + "'");
        auto old = p_.clips.find(key);
        const json oldJson = old == p_.clips.end() ? json(nullptr) : clipToJson(old->second);
        if (jc.is_null()) p_.clips.erase(key); else p_.clips[key] = clipFromJson(jc);
        assignUids(p_, nextUid_);
        if (!jc.is_null()) ex.forward.args["clip"] = clipToJson(p_.clips[key]);
        ex.inverse = {"clip.set", {{"track", a["track"]}, {"scene", a["scene"]}, {"clip", oldJson}}};
        ex.info = structuralChange();
    } else if (k == "arrclip.set") {
        const std::string key = need(a, "key").get<std::string>();
        const json& ja = need(a, "arr");
        auto old = p_.arr.find(key);
        json oldJson = nullptr;
        if (old != p_.arr.end()) oldJson = {{"trackId", old->second.trackId}, {"start", old->second.start}, {"clip", clipToJson(old->second.clip)}};
        if (ja.is_null()) p_.arr.erase(key);
        else {
            ArrClip ac; ac.trackId = need(ja, "trackId").get<std::string>(); ac.start = ja.value("start", 0.0);
            if (ja.contains("clip")) ac.clip = clipFromJson(ja["clip"]);
            p_.arr[key] = std::move(ac);
        }
        assignUids(p_, nextUid_);
        if (!ja.is_null()) { auto& ac = p_.arr[key]; ex.forward.args["arr"] = {{"trackId", ac.trackId}, {"start", ac.start}, {"clip", clipToJson(ac.clip)}}; }
        ex.inverse = {"arrclip.set", {{"key", key}, {"arr", oldJson}}};
        ex.info = structuralChange();
    } else if (k == "note.add") {
        Clip* clip = clipRef(need(a, "clip"));
        Note n = clipFromJson(json{{"notes", json{{"x", need(a, "note")}}}}).notes.at(0);
        const size_t idx = a.contains("index") ? indexArg(a, "index", clip->notes.size(), true) : clip->notes.size();
        if (n.uid == 0) n.uid = nextUid_++;
        clip->notes.insert(clip->notes.begin() + static_cast<std::ptrdiff_t>(idx), n);
        ex.forward.args["index"] = idx;
        ex.forward.args["note"] = clipToJson(Clip{384, std::nullopt, {n}, {}})["notes"]["n000000"];
        ex.inverse = {"note.remove", {{"clip", a["clip"]}, {"uid", n.uid}}};
        ex.info = structuralChange();
    } else if (k == "note.remove") {
        Clip* clip = clipRef(need(a, "clip"));
        const Uid uid = uidArg(a, "uid");
        auto it = std::find_if(clip->notes.begin(), clip->notes.end(), [&](const Note& n) { return n.uid == uid; });
        if (it == clip->notes.end()) bad("no note with uid " + std::to_string(uid));
        const size_t idx = static_cast<size_t>(it - clip->notes.begin());
        const json nj = clipToJson(Clip{384, std::nullopt, {*it}, {}})["notes"]["n000000"];
        clip->notes.erase(it);
        ex.inverse = {"note.add", {{"clip", a["clip"]}, {"note", nj}, {"index", idx}}};
        ex.info = structuralChange();
    } else if (k == "note.edit") {
        Clip* clip = clipRef(need(a, "clip"));
        const Uid uid = uidArg(a, "uid");
        auto it = std::find_if(clip->notes.begin(), clip->notes.end(), [&](const Note& n) { return n.uid == uid; });
        if (it == clip->notes.end()) bad("no note with uid " + std::to_string(uid));
        const json oldJson = clipToJson(Clip{384, std::nullopt, {*it}, {}})["notes"]["n000000"];
        Note n = clipFromJson(json{{"notes", json{{"x", need(a, "note")}}}}).notes.at(0);
        n.uid = uid;  // an edit never changes identity
        *it = n;
        ex.forward.args["note"] = clipToJson(Clip{384, std::nullopt, {n}, {}})["notes"]["n000000"];
        ex.inverse = {"note.edit", {{"clip", a["clip"]}, {"uid", uid}, {"note", oldJson}}};
        ex.info = structuralChange();
    } else if (k == "env.set") {
        const json& scope = need(a, "scope");
        const std::string key = need(a, "key").get<std::string>();
        const json& pts = need(a, "points");
        AutoMap* map;
        if (scope.contains("master")) map = &p_.masterAuto;
        else if (scope.contains("arr") || scope.contains("scene")) map = &clipRef(scope)->env;
        else map = &p_.tracks[trackIdx(uidArg(scope, "track"))].autoLanes;
        auto it = map->find(key);
        const json oldJson = it == map->end() ? json(nullptr) : pointsToJson(it->second);
        if (pts.is_null()) map->erase(key); else (*map)[key] = pointsFromJson(pts);
        ex.inverse = {"env.set", {{"scope", scope}, {"key", key}, {"points", oldJson}}};
        ex.info = structuralChange();
    } else if (k == "lfo.insert" || k == "lfo.remove" || k == "lfo.edit") {
        Track& t = p_.tracks[trackIdx(uidArg(a, "track"))];
        const bool ins = k == "lfo.insert";
        const size_t idx = indexArg(a, "index", t.lfos.size(), ins);
        if (ins) { t.lfos.insert(t.lfos.begin() + static_cast<std::ptrdiff_t>(idx), lfoFromJson(need(a, "lfo"))); ex.inverse = {"lfo.remove", {{"track", t.uid}, {"index", idx}}}; }
        else if (k == "lfo.remove") { const json old = lfoToJson(t.lfos[idx]); t.lfos.erase(t.lfos.begin() + static_cast<std::ptrdiff_t>(idx)); ex.inverse = {"lfo.insert", {{"track", t.uid}, {"index", idx}, {"lfo", old}}}; }
        else { const json old = lfoToJson(t.lfos[idx]); t.lfos[idx] = lfoFromJson(need(a, "lfo")); ex.inverse = {"lfo.edit", {{"track", t.uid}, {"index", idx}, {"lfo", old}}}; }
        ex.info = structuralChange();
    } else if (k == "macro.value") {
        Track& t = p_.tracks[trackIdx(uidArg(a, "track"))];
        const size_t idx = indexArg(a, "index", t.macros.size(), false);
        const double v = std::clamp(need(a, "value").get<double>(), 0.0, 1.0);
        ex.inverse = {"macro.value", {{"track", t.uid}, {"index", idx}, {"value", t.macros[idx].value}}};
        t.macros[idx].value = v;
        ex.info.params.push_back({t.id + "|macro|" + std::to_string(idx), v});
    } else if (k == "lfo.field") {
        Track& t = p_.tracks[trackIdx(uidArg(a, "track"))];
        const size_t idx = indexArg(a, "index", t.lfos.size(), false);
        const std::string f = need(a, "field").get<std::string>();
        const double v = need(a, "value").get<double>();
        LfoSpec& l = t.lfos[idx];
        double* slot = f == "depth" ? &l.depth : f == "hz" ? &l.hz : f == "phase" ? &l.phase : nullptr;
        if (!slot) bad("lfo.field is depth, hz or phase");
        ex.inverse = {"lfo.field", {{"track", t.uid}, {"index", idx}, {"field", f}, {"value", *slot}}};
        *slot = f == "depth" ? std::clamp(v, 0.0, 1.0) : f == "hz" ? std::clamp(v, 0.01, 30.0) : v;
        ex.info.params.push_back({t.id + "|lfo" + std::to_string(idx) + "|" + f, *slot});
    } else if (k == "perf.insert" || k == "perf.remove" || k == "perf.edit") {
        Track& t = p_.tracks[trackIdx(uidArg(a, "track"))];
        const bool ins = k == "perf.insert";
        const size_t idx = indexArg(a, "index", t.perf.size(), ins);
        if (ins) { t.perf.insert(t.perf.begin() + static_cast<std::ptrdiff_t>(idx), perfFromJson(need(a, "perf"))); ex.inverse = {"perf.remove", {{"track", t.uid}, {"index", idx}}}; }
        else if (k == "perf.remove") { const json old = perfToJson(t.perf[idx]); t.perf.erase(t.perf.begin() + static_cast<std::ptrdiff_t>(idx)); ex.inverse = {"perf.insert", {{"track", t.uid}, {"index", idx}, {"perf", old}}}; }
        else { const json old = perfToJson(t.perf[idx]); t.perf[idx] = perfFromJson(need(a, "perf")); ex.inverse = {"perf.edit", {{"track", t.uid}, {"index", idx}, {"perf", old}}}; }
        ex.info = structuralChange();
    } else if (k == "macro.insert" || k == "macro.remove" || k == "macro.edit") {
        Track& t = p_.tracks[trackIdx(uidArg(a, "track"))];
        const bool ins = k == "macro.insert";
        const size_t idx = indexArg(a, "index", t.macros.size(), ins);
        if (ins) { t.macros.insert(t.macros.begin() + static_cast<std::ptrdiff_t>(idx), macroFromJson(need(a, "macro"))); ex.inverse = {"macro.remove", {{"track", t.uid}, {"index", idx}}}; }
        else if (k == "macro.remove") { const json old = macroToJson(t.macros[idx]); t.macros.erase(t.macros.begin() + static_cast<std::ptrdiff_t>(idx)); ex.inverse = {"macro.insert", {{"track", t.uid}, {"index", idx}, {"macro", old}}}; }
        else { const json old = macroToJson(t.macros[idx]); t.macros[idx] = macroFromJson(need(a, "macro")); ex.inverse = {"macro.edit", {{"track", t.uid}, {"index", idx}, {"macro", old}}}; }
        ex.info = structuralChange();
    } else if (k == "return.insert" || k == "return.remove" || k == "return.edit") {
        auto parse = [&](const json& j) { Return r; r.id = j.value("id", ""); r.name = j.value("name", ""); r.fxType = j.value("fxType", ""); r.gainDb = j.value("gain", 0.0);
            if (j.contains("params")) for (auto& [pk, pv] : j["params"].items()) if (pv.is_number()) r.params[pk] = pv.get<double>(); return r; };
        auto dump = [&](const Return& r) { json pj = json::object(); for (auto& [pk, pv] : r.params) pj[pk] = pv; return json{{"id", r.id}, {"name", r.name}, {"fxType", r.fxType}, {"params", pj}, {"gain", r.gainDb}}; };
        const bool ins = k == "return.insert";
        const size_t idx = indexArg(a, "index", p_.returns.size(), ins);
        if (ins) { p_.returns.insert(p_.returns.begin() + static_cast<std::ptrdiff_t>(idx), parse(need(a, "ret"))); ex.inverse = {"return.remove", {{"index", idx}}}; }
        else if (k == "return.remove") { const json old = dump(p_.returns[idx]); p_.returns.erase(p_.returns.begin() + static_cast<std::ptrdiff_t>(idx)); ex.inverse = {"return.insert", {{"index", idx}, {"ret", old}}}; }
        else { const json old = dump(p_.returns[idx]); p_.returns[idx] = parse(need(a, "ret")); ex.inverse = {"return.edit", {{"index", idx}, {"ret", old}}}; }
        ex.info = structuralChange();
    } else if (k == "compound") {
        const json& list = need(a, "commands");
        if (!list.is_array()) bad("compound needs a commands array");
        std::vector<Exec> done;
        try {
            for (const auto& cj : list) {
                Command child{need(cj, "kind").get<std::string>(), cj.value("args", json::object())};
                done.push_back(execute(child));
            }
        } catch (...) {  // all or nothing: undo what was applied, in reverse
            for (auto it = done.rbegin(); it != done.rend(); ++it) execute(it->inverse);
            throw;
        }
        json fwd = json::array(), inv = json::array();
        for (auto& d : done) { fwd.push_back({{"kind", d.forward.kind}, {"args", d.forward.args}}); mergeInfo(ex.info, d.info); }
        for (auto it = done.rbegin(); it != done.rend(); ++it) inv.push_back({{"kind", it->inverse.kind}, {"args", it->inverse.args}});
        ex.forward.args = {{"label", a.value("label", "")}, {"commands", fwd}};
        ex.inverse = {"compound", {{"label", a.value("label", "")}, {"commands", inv}}};
    } else {
        bad("unknown command '" + k + "'");
    }
    if (ex.info.structural) { ex.info.params.clear(); ex.info.tempo.reset(); }  // a rebuild carries every value anyway
    return ex;
}

ChangeInfo Document::apply(const Command& c, Command* applied) {
    Exec ex = execute(c);  // throws before any history change on failure
    bump();
    if (applied) *applied = ex.forward;
    ChangeInfo info = ex.info;
    if (grouping_) group_.push_back(std::move(ex));
    else { undo_.push_back({ex.forward, ex.inverse, c.kind}); redo_.clear(); }
    return info;
}

ChangeInfo Document::undo() {
    if (undo_.empty() || grouping_) return {};
    Step s = std::move(undo_.back());
    undo_.pop_back();
    Exec ex = execute(s.inverse);
    bump();
    redo_.push_back(std::move(s));
    return ex.info;
}

ChangeInfo Document::redo() {
    if (redo_.empty() || grouping_) return {};
    Step s = std::move(redo_.back());
    redo_.pop_back();
    Exec ex = execute(s.forward);
    bump();
    undo_.push_back(std::move(s));
    return ex.info;
}

void Document::beginGroup(const std::string& label) {
    if (grouping_) throw std::logic_error("groups do not nest");
    grouping_ = true;
    groupLabel_ = label;
    group_.clear();
}

ChangeInfo Document::endGroup() {
    if (!grouping_) throw std::logic_error("no group open");
    grouping_ = false;
    ChangeInfo info;
    if (group_.empty()) return info;
    json fwd = json::array(), inv = json::array();
    for (auto& e : group_) { fwd.push_back({{"kind", e.forward.kind}, {"args", e.forward.args}}); mergeInfo(info, e.info); }
    for (auto it = group_.rbegin(); it != group_.rend(); ++it) inv.push_back({{"kind", it->inverse.kind}, {"args", it->inverse.args}});
    undo_.push_back({Command{"compound", {{"label", groupLabel_}, {"commands", fwd}}}, Command{"compound", {{"label", groupLabel_}, {"commands", inv}}}, groupLabel_});
    redo_.clear();
    group_.clear();
    return info;
}

const std::string& Document::undoLabel() const {
    static const std::string none;
    return undo_.empty() ? none : undo_.back().label;
}

namespace cmd {
Command setParam(Uid deviceUid, const std::string& key, double value) { return {"device.param", {{"uid", deviceUid}, {"key", key}, {"value", value}}}; }
Command setTrack(Uid trackUid, const std::string& field, json value) { return {"track.set", {{"uid", trackUid}, {"field", field}, {"value", std::move(value)}}}; }
Command setMeta(const std::string& field, json value) { return {"meta.set", {{"field", field}, {"value", std::move(value)}}}; }
}  // namespace cmd

}  // namespace ddaw::document
