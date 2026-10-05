#include "engine/GraphBuilder.h"
#include "engine/Meters.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <stdexcept>
#include <functional>
#include <span>
#include <unordered_map>

#include <optional>

#include "devices/Registry.h"
#include "engine/MidiFx.h"

namespace ddaw::engine {

namespace {

using project::DeviceSpec;
using project::TrackKind;

constexpr double kFbDelaySec = 0.09;
constexpr float kFbDefaultSend = 0.3f;
const std::string& fxIdOf(const DeviceSpec& d) { return d.id.empty() ? d.type : d.id; }

float dbToLin(double db) { return std::pow(10.0f, static_cast<float>(db) / 20.0f); }

dsp::Smoother snapped(double sr, float v) {
    dsp::Smoother s;
    s.prepare(sr, 15.0f);
    s.snap(v);
    return s;
}

// Applies the stored parameters and registers the resolver entries for one device.
template <class Dev>
void applyParams(Dev& dev, const DeviceSpec& spec, const std::string& where, std::set<std::string>& issues,
                 ParamResolver& resolver, const std::string& keyPrefix, uint16_t target, uint8_t slot) {
    auto specs = dev.params();
    for (auto& ps : specs) resolver.insert(keyPrefix + ps.key, {target, slot, ps.index});
    resolver.insert(keyPrefix + "out", {target, slot, kParamOut});
    for (auto& [k, v] : spec.params) {
        const int idx = findParam(specs, k);
        if (idx < 0) { issues.insert(where + ": unknown param '" + k + "'"); continue; }
        dev.setParam(specs[size_t(idx)].index, static_cast<float>(v));
    }
}

// A built (enabled, known) effect, recorded so modulation keys can address it by id.
struct BuiltFx {
    std::string id;
    uint8_t slot;
    std::span<const ParamSpec> specs;
    const DeviceSpec* spec;
};

std::vector<std::string> splitKey(const std::string& key) {
    std::vector<std::string> parts;
    size_t from = 0;
    for (;;) {
        const size_t bar = key.find('|', from);
        parts.push_back(key.substr(from, bar == std::string::npos ? std::string::npos : bar - from));
        if (bar == std::string::npos) break;
        from = bar + 1;
    }
    return parts;
}

// Registers the expression curves of the notes that have any and returns each note's index (-1: none).
std::vector<int> exprIndices(Graph& g, const std::vector<project::Note>& notes) {
    std::vector<int> out(notes.size(), -1);
    for (size_t i = 0; i < notes.size(); ++i) {
        const auto& n = notes[i];
        if (n.bend.empty() && n.slide.empty() && n.pressure.empty()) continue;
        ExprSeq s;
        s.uid = n.uid;
        const std::vector<project::ExprPoint>* src[3] = {&n.slide, &n.pressure, &n.bend};
        for (int d = 0; d < 3; ++d) {
            for (const auto& p : *src[d]) s.dim[size_t(d)].push_back({float(p.t), float(p.v)});
            std::stable_sort(s.dim[size_t(d)].begin(), s.dim[size_t(d)].end(), [](const ExprSeq::Pt& a, const ExprSeq::Pt& b) { return a.t < b.t; });
        }
        out[i] = g.addExprSeq(std::move(s));
    }
    return out;
}

std::vector<ModPt> ptsOf(const std::vector<project::AutoPoint>& points) {
    std::vector<ModPt> out;
    out.reserve(points.size());
    for (auto& p : points) out.push_back({p.t, static_cast<float>(p.v)});
    return out;
}

}  // namespace

BuildResult buildGraph(const project::Fixture& fx, double sr, uint32_t epoch, const project::SampleBank* bank) {
    const auto& p = fx.project;
    // A render needs something to render; a live graph may be empty (a new project is just the master bus).
    if (p.tracks.size() > static_cast<size_t>(kMaxMeterTracks)) throw std::runtime_error("too many tracks: the engine supports " + std::to_string(kMaxMeterTracks));
    if (p.tracks.empty() && fx.scope.kind != "live") throw std::runtime_error("project has no tracks");

    BuildResult out;
    out.graph = std::make_unique<Graph>(epoch, sr);
    Graph& g = *out.graph;
    std::set<std::string> issues;
    ParamResolver& res = out.resolver;
    const size_t nTracks = p.tracks.size();

    // ---- render scope (render.ts scopeBounds) ----
    const double bar = 4.0 * kPpq * p.meta.tsTop / std::max(p.meta.tsBottom, 1);
    if (fx.scope.kind == "scene") {
        const auto it = std::find(p.scenes.begin(), p.scenes.end(), fx.scope.sceneId);
        if (it == p.scenes.end()) throw std::runtime_error("scene '" + fx.scope.sceneId + "' not in project");
        out.sceneIndex = static_cast<int>(it - p.scenes.begin());
        double longest = bar;
        const std::string suffix = "|" + fx.scope.sceneId;
        for (auto& [key, c] : p.clips)
            if (key.size() > suffix.size() && key.compare(key.size() - suffix.size(), std::string::npos, suffix) == 0)
                longest = std::max(longest, c.len);
        out.mode = TransportMode::Session;
        out.fromTicks = 0;
        out.lengthTicks = longest * 2.0;
    } else if (fx.scope.kind == "arr") {
        double end = 0;
        for (auto& [k, a] : p.arr) end = std::max(end, a.start + a.clip.len);
        if (p.arr.empty()) throw std::runtime_error("Arrangement is empty: nothing to render");
        out.mode = TransportMode::Arrangement;
        out.fromTicks = 0;
        out.lengthTicks = std::max(end, bar);
    } else if (fx.scope.kind == "loop") {
        const double s = p.meta.loopStart, e = p.meta.loopEnd > p.meta.loopStart ? p.meta.loopEnd : 4.0 * bar;
        out.mode = TransportMode::Arrangement;
        out.fromTicks = s;
        out.lengthTicks = std::max(e - s, bar);
    } else if (fx.scope.kind == "live") {  // an interactive graph: no render scope, clips are launched by commands
        out.mode = TransportMode::Session;
    } else {
        throw std::runtime_error("unknown render scope '" + fx.scope.kind + "'");
    }

    // ---- global settings ----
    if (p.meta.humanize < 0) issues.insert("meta: negative humanize");
    g.setMasterGainDb(static_cast<float>(p.meta.masterGainDb));
    g.setTiming({p.meta.swing, swingSubdivTicks(p.meta.swingSubdivision), p.meta.humanize}, p.meta.launchQ, p.meta.tsTop, p.meta.tsBottom);
    g.setLoop(p.meta.loopOn, p.meta.loopStart, p.meta.loopEnd);
    res.insert("master|gain", {kTargetMaster, kSlotMixer, kMixGain});
    for (size_t si = 0; si < p.scenes.size(); ++si) res.insertScene(p.scenes[si], static_cast<uint16_t>(si));

    // ---- track index by id; bus topology (Kahn over output-target + send edges; cycles cut below) ----
    std::unordered_map<std::string, int> idxOf;
    for (size_t i = 0; i < nTracks; ++i) { idxOf.emplace(p.tracks[i].id, static_cast<int>(i)); res.insertTrack(p.tracks[i].id, static_cast<uint16_t>(i)); }
    auto find = [&](const std::string& id) { auto it = idxOf.find(id); return it == idxOf.end() ? -1 : it->second; };
    auto isBus = [&](int i) { return p.tracks[static_cast<size_t>(i)].kind == TrackKind::Bus; };

    std::vector<int> busIdx;
    for (size_t i = 0; i < nTracks; ++i) if (p.tracks[i].kind == TrackKind::Bus) busIdx.push_back(static_cast<int>(i));
    std::vector<std::pair<int, int>> edges;  // u renders before v
    for (int u : busIdx) {
        const auto& t = p.tracks[static_cast<size_t>(u)];
        if (!t.output.empty()) { const int v = find(t.output); if (v >= 0 && isBus(v) && v != u) edges.push_back({u, v}); }
        for (auto& [bid, level] : t.sends) { (void)level; const int v = find(bid); if (v >= 0 && isBus(v) && v != u) edges.push_back({u, v}); }
    }
    std::vector<int> busOrder;
    {
        std::vector<int> indeg(busIdx.size(), 0);
        std::vector<bool> done(busIdx.size(), false);
        for (size_t q = 0; q < busIdx.size(); ++q)
            for (auto& e : edges) if (e.second == busIdx[q]) ++indeg[q];
        for (;;) {
            size_t pos = busIdx.size();
            for (size_t q = 0; q < busIdx.size(); ++q) if (!done[q] && indeg[q] == 0) { pos = q; break; }
            if (pos == busIdx.size()) break;
            done[pos] = true;
            busOrder.push_back(busIdx[pos]);
            for (auto& e : edges)
                if (e.first == busIdx[pos])
                    for (size_t q = 0; q < busIdx.size(); ++q) if (busIdx[q] == e.second && indeg[q] > 0) --indeg[q];
        }
        for (size_t q = 0; q < busIdx.size(); ++q) if (!done[q]) busOrder.push_back(busIdx[q]);  // cyclic leftovers, document order
    }
    auto busPos = [&](int i) { for (size_t q = 0; q < busOrder.size(); ++q) if (busOrder[q] == i) return static_cast<int>(q); return -1; };

    int sendABus = -1, sendBBus = -1;
    for (size_t i = 0; i < nTracks; ++i) {
        if (p.tracks[i].kind != TrackKind::Bus) continue;
        if (p.tracks[i].send == project::SendBus::A && sendABus < 0) sendABus = static_cast<int>(i);
        if (p.tracks[i].send == project::SendBus::B && sendBBus < 0) sendBBus = static_cast<int>(i);
    }
    g.setSendBuses(sendABus, sendBBus);
    const bool legacyAB = sendABus < 0 && sendBBus < 0;

    bool anySolo = false;
    for (auto& t : p.tracks) anySolo |= t.solo;

    struct PendingDuck { std::string src; int track; int fx; int pitch; };
    std::vector<PendingDuck> pendingDucks;
    std::vector<std::vector<BuiltFx>> builtFx(nTracks);
    std::vector<BuiltFx> masterBuilt;

    // ---- tracks ----
    for (size_t ti = 0; ti < nTracks; ++ti) {
        const auto& t = p.tracks[ti];
        const std::string& tid = t.id;
        const std::string where = "track " + tid;
        const uint16_t idx = static_cast<uint16_t>(ti);
        const bool bus = t.kind == TrackKind::Bus;
        TrackStrip& s = g.addTrack();
        s.id = tid;
        s.isBus = bus;

        if (!bus) {
            s.inst = createInstrument(t.inst.type);
            if (s.inst) {
                s.inst->prepare(sr, kMaxBlock);
                applyParams(*s.inst, t.inst, where + " inst", issues, res, tid + "|inst|", idx, kSlotInst);
                if (bank) {  // sample injection: the main sample into slot 0, drum pad overrides into slots 0-7
                    if (!t.inst.sampleId.empty())
                        if (auto buf = bank->get(t.inst.sampleId)) s.inst->setSample(0, buf);
                    for (const auto& [slotKey, sid] : t.inst.padSamples) {
                        const long slot = std::strtol(slotKey.c_str(), nullptr, 10);
                        if (slot >= 0 && slot < 8)
                            if (auto buf = bank->get(sid)) s.inst->setSample(static_cast<uint32_t>(slot), buf);
                    }
                }
                s.inst->reset();
            } else if (t.kind != TrackKind::Audio) {
                issues.insert("instrument: " + t.inst.type);
            }
        }
        s.instOut.snap(t.inst.outDb ? dbToLin(*t.inst.outDb) : 1.0f);
        res.insert(tid + "|inst|out", {idx, kSlotInst, kParamOut});

        for (const auto& f : t.fx) {
            if (!f.on) continue;  // bypassed devices take no slot
            auto dev = createEffect(f.type);
            if (!dev) { issues.insert("effect: " + f.type); continue; }
            dev->prepare(sr, kMaxBlock);
            const uint8_t slot = static_cast<uint8_t>(kSlotFx0 + s.fx.size());
            applyParams(*dev, f, where + " fx " + f.type, issues, res, tid + "|" + fxIdOf(f) + "|", idx, slot);
            dev->reset();  // snap smoothers to the loaded values
            if (f.type == "duck") {
                dev->setSidechain(!f.srcTrack.empty());
                if (!f.srcTrack.empty())
                    pendingDucks.push_back({f.srcTrack, static_cast<int>(ti), static_cast<int>(s.fx.size()), f.srcPitch ? static_cast<int>(*f.srcPitch) : -1});
            }
            builtFx[ti].push_back({fxIdOf(f), slot, dev->params(), &f});
            FxSlot fs;
            fs.dev = std::move(dev);
            fs.out.prepare(sr, 15.0f);
            fs.out.snap(f.outDb ? dbToLin(*f.outDb) : 1.0f);
            s.fx.push_back(std::move(fs));
        }

        const bool muted = t.mute || (anySolo && !t.solo);
        s.audible = !muted;
        s.fader.snap(dbToLin(t.gainDb));
        s.pan.snap(static_cast<float>(t.pan));
        s.muteGain.snap(muted ? 0.0f : 1.0f);
        s.sendA.snap(static_cast<float>(t.sendA));
        s.sendB.snap(static_cast<float>(t.sendB));
        res.insert(tid + "|mix|gain", {idx, kSlotMixer, kMixGain});
        res.insert(tid + "|mix|pan", {idx, kSlotMixer, kMixPan});
        res.insert(tid + "|mix|sendA", {idx, kSlotMixer, kMixSendA});
        res.insert(tid + "|mix|sendB", {idx, kSlotMixer, kMixSendB});

        // per-bus sends (forward edges only for bus sources; the feedback bus's self-send is handled below)
        for (auto& [bid, level] : t.sends) {
            const int v = find(bid);
            if (v < 0 || v == static_cast<int>(ti) || !isBus(v)) continue;
            if (bus) {
                const int pu = busPos(static_cast<int>(ti)), pv = busPos(v);
                if (pu < 0 || pv < 0 || pv < pu) continue;  // a back edge: cut
            }
            if (s.busSends.size() >= kMaxBusSends) break;
            res.insert(tid + "|send|" + bid, {idx, kSlotMixer, static_cast<uint16_t>(kMixBusSend0 + s.busSends.size())});
            s.busSends.push_back({v, snapped(sr, static_cast<float>(level))});
        }

        // bus output routing (rewireBuses); non-bus tracks always feed the master
        if (bus && !t.output.empty() && t.output != "master") {
            const int v = find(t.output);
            if (v >= 0 && isBus(v) && v != static_cast<int>(ti)) {
                if (busPos(v) > busPos(static_cast<int>(ti))) {
                    s.outKind = TrackStrip::Out::Bus;
                    s.outTarget = v;
                } else {  // closes a cycle: route it through a delay so the loop regenerates (Feedback Chunk)
                    const int delay = std::max(static_cast<int>(sr * kFbDelaySec), 64);
                    g.addBackEdge(v, delay);
                    s.outKind = TrackStrip::Out::Delayed;
                    s.outTarget = -2;  // patched below, once all edge indices are known
                }
            }
        }

        // Feedback bus: post-fader -> 0.09 s delay -> loop gain -> its own input
        if (t.send == project::SendBus::F) {
            auto fb = std::make_unique<TrackStrip::Feedback>();
            fb->delaySamples = std::max(static_cast<int>(kFbDelaySec * sr), kMaxBlock + 1);
            fb->dl.prepare(fb->delaySamples + kMaxBlock + 2);
            fb->dr.prepare(fb->delaySamples + kMaxBlock + 2);
            float level = kFbDefaultSend;
            if (auto it = t.sends.find(tid); it != t.sends.end()) level = static_cast<float>(it->second);
            fb->gain.prepare(sr, 15.0f);
            fb->gain.snap(level);
            res.insert(tid + "|send|" + tid, {idx, kSlotMixer, kMixFeedback});
            s.fb = std::move(fb);
        }

        // ---- event patterns, expanded through the track's MIDI-fx chain ----
        if (!bus) {
            const double root = p.meta.root;
            const std::string& scale = p.meta.scale;
            s.sched.session.resize(p.scenes.size());
            s.sched.audioSession.resize(p.scenes.size());
            for (size_t si = 0; si < p.scenes.size(); ++si) {
                auto it = p.clips.find(tid + "|" + p.scenes[si]);
                if (it == p.clips.end()) continue;
                const auto& clip = it->second;
                if (clip.audio) {  // an audio clip: derive it from the sample bank (a missing sample plays silent)
                    if (auto buf = bank ? bank->get(clip.audio->sampleId) : nullptr)
                        if (auto derived = DerivedClip::build(*clip.audio, *buf, sr, p.meta.bpm > 0 ? p.meta.bpm : 120.0)) {
                            s.audio.clips.push_back(std::move(*derived));
                            s.sched.audioSession[si] = AudioSlotRef{static_cast<int>(s.audio.clips.size()) - 1, std::max(clip.len, 1.0), clip.audio->loop != 0.0};
                        }
                    continue;
                }
                const double len = std::max(clip.len, 1.0);
                MidiFxContext mc{tid, t.kind == TrackKind::Drum, root, scale, len};
                ClipPattern pat;
                pat.loopLen = len;
                const auto exprOf = exprIndices(g, clip.notes);
                pat.events = expandMidi(t.midifx, clip.notes, mc, &exprOf);
                pat.events.erase(std::remove_if(pat.events.begin(), pat.events.end(), [&](const NoteEv& e) { return e.tick >= len; }), pat.events.end());
                s.sched.session[si] = std::move(pat);
            }
            for (auto& [key, a] : p.arr) {
                if (a.trackId != tid) continue;
                const double len = std::max(a.clip.len, 1.0);
                if (a.clip.audio) {  // a bounded arrangement audio clip: start .. start + len, fading out after
                    if (auto buf = bank ? bank->get(a.clip.audio->sampleId) : nullptr)
                        if (auto derived = DerivedClip::build(*a.clip.audio, *buf, sr, p.meta.bpm > 0 ? p.meta.bpm : 120.0)) {
                            s.audio.clips.push_back(std::move(*derived));
                            s.audio.arr.push_back({a.start, a.start + len, static_cast<int>(s.audio.clips.size()) - 1});
                        }
                    continue;
                }
                MidiFxContext mc{tid, t.kind == TrackKind::Drum, root, scale, len};
                const auto exprOf = exprIndices(g, a.clip.notes);
                auto evs = expandMidi(t.midifx, a.clip.notes, mc, &exprOf);
                for (auto& e : evs) if (e.tick < len) { e.tick += a.start; s.sched.arr.push_back(e); }
            }
            std::stable_sort(s.sched.arr.begin(), s.sched.arr.end(), [](const NoteEv& a, const NoteEv& b) { return a.tick < b.tick; });
            std::stable_sort(s.audio.arr.begin(), s.audio.arr.end(), [](const AudioArrEv& a, const AudioArrEv& b) { return a.tick < b.tick; });
        }
    }

    // patch Delayed outputs to their back-edge indices (edges were appended in track order)
    {
        int e = 0;
        for (size_t ti = 0; ti < nTracks; ++ti) {
            auto& s = g.track(static_cast<int>(ti));
            if (s.outKind == TrackStrip::Out::Delayed) s.outTarget = e++;
        }
    }

    // ---- audio-rate routes (B4): resolve target and source, order the tracks so a source renders first, reject loops ----
    std::vector<std::pair<int, int>> aEdges;   // [src, dst]: the source track renders before the target track
    auto reaches = [&](int from, int to) {     // is there already a render-order path from -> to?
        std::vector<int> stack{from};
        std::vector<bool> seen(nTracks, false);
        while (!stack.empty()) {
            const int u = stack.back(); stack.pop_back();
            if (u == to) return true;
            if (seen[size_t(u)]) continue;
            seen[size_t(u)] = true;
            for (auto& e : aEdges) if (e.first == u) stack.push_back(e.second);
        }
        return false;
    };
    for (size_t ti = 0; ti < nTracks; ++ti) {
        const auto& t = p.tracks[ti];
        for (size_t ai = 0; ai < t.arate.size(); ++ai) {
            const auto& sp = t.arate[ai];
            const std::string where = "track " + t.id + " audio-rate route " + std::to_string(ai);
            if (!sp.on) continue;
            // target: the instrument or one of this track's effects, an A-rate parameter
            std::span<const ParamSpec> specs;
            AModBufs* mods = nullptr;
            int dstFx = -1;
            if (sp.target.dest == "inst") {
                if (g.track(int(ti)).inst) { specs = g.track(int(ti)).inst->params(); mods = &g.track(int(ti)).instMod; }
            } else {
                for (const auto& bf : builtFx[ti])
                    if (bf.id == sp.target.fxId) { specs = bf.specs; dstFx = int(bf.slot) - int(kSlotFx0); mods = &g.track(int(ti)).fx[size_t(dstFx)].mod; break; }
            }
            if (!mods) { issues.insert(where + ": target device not found"); continue; }
            const int pi = findParam(specs, sp.target.pkey);
            if (pi < 0) { issues.insert(where + ": unknown parameter '" + sp.target.pkey + "'"); continue; }
            const ParamSpec& ps = specs[size_t(pi)];
            if (!ps.audioRate) { issues.insert(where + ": '" + sp.target.pkey + "' does not accept audio-rate modulation"); continue; }
            size_t ordinal = 0, numA = 0;
            for (size_t q = 0; q < specs.size(); ++q) if (specs[q].audioRate) { if (q < size_t(pi)) ++ordinal; ++numA; }
            AudioRoute r;
            r.specIndex = int(ai);
            r.dstTrack = int(ti);
            r.dstFx = dstFx;
            r.ordinal = ordinal;
            r.halfRange = (ps.max - ps.min) * 0.5f;
            r.depth = std::clamp(static_cast<float>(sp.depth), -1.0f, 1.0f);
            if (sp.source == "track") {
                const int src = find(sp.srcTrack);
                if (src < 0) { issues.insert(where + ": source track '" + sp.srcTrack + "' not found"); continue; }
                if (isBus(src)) { issues.insert(where + ": a bus cannot be a modulation source"); continue; }
                if (src == int(ti) || (!isBus(int(ti)) && reaches(int(ti), src))) { issues.insert(where + ": modulation loop with track '" + sp.srcTrack + "'"); continue; }
                r.kind = AudioRoute::Kind::Track;
                r.src = src;
                r.follow = sp.follow;
                r.atk = 1.0f - std::exp(-1.0f / std::max(static_cast<float>(sp.attackMs) * 0.001f * static_cast<float>(sr), 1.0f));
                r.rel = 1.0f - std::exp(-1.0f / std::max(static_cast<float>(sp.releaseMs) * 0.001f * static_cast<float>(sr), 1.0f));
                if (!isBus(int(ti))) aEdges.push_back({src, int(ti)});
            } else {
                r.kind = AudioRoute::Kind::Osc;
                r.shape = std::clamp(sp.shape, 0, 3);
                r.hz = std::clamp(static_cast<float>(sp.hz), 0.05f, 12000.0f);
            }
            if (mods->ptrs.empty()) mods->init(numA);
            mods->ensure(ordinal);
            g.addAudioRoute(r);
            res.insert(t.id + "|arate" + std::to_string(ai) + "|depth", {static_cast<uint16_t>(ti), kSlotArate, static_cast<uint16_t>(ai * 4 + kArateDepth)});
            res.insert(t.id + "|arate" + std::to_string(ai) + "|hz", {static_cast<uint16_t>(ti), kSlotArate, static_cast<uint16_t>(ai * 4 + kArateHz)});
        }
    }

    // ---- render order: non-bus tracks in document order (sources of audio-rate routes first), then buses topologically ----
    std::vector<int> order;
    {
        std::vector<int> indeg(nTracks, 0);
        std::vector<bool> done(nTracks, false);
        for (auto& e : aEdges) ++indeg[size_t(e.second)];
        for (;;) {
            int pick = -1;
            for (size_t i = 0; i < nTracks; ++i) if (!isBus(int(i)) && !done[i] && indeg[i] == 0) { pick = int(i); break; }
            if (pick < 0) break;
            done[size_t(pick)] = true;
            order.push_back(pick);
            for (auto& e : aEdges) if (e.first == pick) --indeg[size_t(e.second)];
        }
        for (size_t i = 0; i < nTracks; ++i) if (!isBus(int(i)) && !done[i]) order.push_back(int(i));   // unreachable: loops are rejected above
    }
    for (int b : busOrder) order.push_back(b);
    g.setOrder(std::move(order));

    // ---- legacy return channels, only when the project has no A/B send buses ----
    if (legacyAB) {
        for (const auto& r : p.returns) {
            auto dev = createEffect(r.fxType);
            if (!dev) { issues.insert("effect: " + r.fxType); continue; }
            dev->prepare(sr, kMaxBlock);
            for (auto& [k, v] : r.params) {
                const int pi = findParam(dev->params(), k);
                if (pi >= 0) dev->setParam(dev->params()[size_t(pi)].index, static_cast<float>(v));
                else issues.insert("return " + r.name + ": unknown param '" + k + "'");
            }
            dev->reset();
            ReturnNode& rn = g.addReturn();
            rn.dev = std::move(dev);
            rn.gain.snap(dbToLin(r.gainDb));
        }
    }

    // ---- master effect chain ----
    for (const auto& f : p.masterFx) {
        if (!f.on) continue;
        auto dev = createEffect(f.type);
        if (!dev) { issues.insert("effect: " + f.type); continue; }
        dev->prepare(sr, kMaxBlock);
        FxSlot& slot = g.addMasterFx();
        const uint8_t slotIdx = static_cast<uint8_t>(kSlotFx0 + g.masterFxCount() - 1);
        applyParams(*dev, f, "master fx " + f.type, issues, res, "master|" + fxIdOf(f) + "|", kTargetMaster, slotIdx);
        dev->reset();
        if (f.type == "duck") {
            dev->setSidechain(!f.srcTrack.empty());
            if (!f.srcTrack.empty())
                pendingDucks.push_back({f.srcTrack, -1, static_cast<int>(g.masterFxCount() - 1), f.srcPitch ? static_cast<int>(*f.srcPitch) : -1});
        }
        masterBuilt.push_back({fxIdOf(f), slotIdx, dev->params(), &f});
        slot.dev = std::move(dev);
        slot.out.snap(f.outDb ? dbToLin(*f.outDb) : 1.0f);
    }

    // ---- duck routes: source ids resolve to track indices (unresolvable sources are dropped) ----
    for (const auto& d : pendingDucks) {
        const int src = find(d.src);
        if (src >= 0) g.addDuck({src, d.track, d.fx, d.pitch});
    }

    // ---- modulation: resolve every "dest|fxId|pkey" mapping to numeric routes ----
    auto mod = std::make_unique<ModState>();
    mod->sampleRate = sr;
    mod->tracks.resize(nTracks);
    struct Leaf { ParamAddr addr; float min, max, base; bool log; };
    std::unordered_map<std::string, std::optional<uint32_t>> routeCache;

    auto leaf = [&](size_t ti, const std::string& dest, const std::string& fxid, const std::string& pkey) -> std::optional<Leaf> {
        const auto& t = p.tracks[ti];
        const uint16_t idx = static_cast<uint16_t>(ti);
        auto fromSpec = [&](std::span<const ParamSpec> specs, const DeviceSpec& d, uint8_t slot) -> std::optional<Leaf> {
            const int pi = findParam(specs, pkey);
            if (pi < 0) return std::nullopt;
            const ParamSpec& sp = specs[size_t(pi)];
            const auto it = d.params.find(pkey);
            const float base = it != d.params.end() ? static_cast<float>(it->second) : sp.def;  // stored value, else the default
            return Leaf{{idx, slot, sp.index}, sp.min, sp.max, base, sp.curve == Curve::Exponential && sp.min > 0 && sp.max > 0};
        };
        if (dest == "inst") {
            if (!g.track(static_cast<int>(ti)).inst) return std::nullopt;
            return fromSpec(g.track(static_cast<int>(ti)).inst->params(), t.inst, kSlotInst);
        }
        if (dest == "mix") {
            if (pkey == "gain") return Leaf{{idx, kSlotMixer, kMixGain}, -48.0f, 6.0f, static_cast<float>(t.gainDb), false};
            if (pkey == "pan") return Leaf{{idx, kSlotMixer, kMixPan}, -1.0f, 1.0f, static_cast<float>(t.pan), false};
            return std::nullopt;
        }
        if (dest == "send") {
            if (pkey == "F") {  // the feedback bus's self-send exists only on the F bus
                if (t.send != project::SendBus::F) return std::nullopt;
                const auto it = t.sends.find(t.id);
                return Leaf{{idx, kSlotMixer, kMixFeedback}, 0.0f, 0.95f, it != t.sends.end() ? static_cast<float>(it->second) : kFbDefaultSend, false};
            }
            if (pkey == "B") return Leaf{{idx, kSlotMixer, kMixSendB}, 0.0f, 1.0f, static_cast<float>(t.sendB), false};
            return Leaf{{idx, kSlotMixer, kMixSendA}, 0.0f, 1.0f, static_cast<float>(t.sendA), false};
        }
        for (const auto& bf : builtFx[ti])  // anything else falls through to the effect lookup (browser behaviour)
            if (bf.id == fxid) return fromSpec(bf.specs, *bf.spec, bf.slot);
        return std::nullopt;
    };

    std::function<std::optional<uint32_t>(size_t, const std::string&)> trackRoute;
    trackRoute = [&](size_t ti, const std::string& key) -> std::optional<uint32_t> {
        const std::string ck = std::to_string(ti) + "|" + key;
        if (auto it = routeCache.find(ck); it != routeCache.end()) return it->second;
        std::optional<uint32_t> made;
        const auto parts = splitKey(key);
        if (parts.size() >= 3) {
            const std::string &dest = parts[0], &fxid = parts[1], &pkey = parts[2];
            const auto& t = p.tracks[ti];
            ModState::Route rt;
            bool ok = false;
            if (dest == "lfo") {  // another LFO's rate: fxId = the target LFO id; spec = hz 0.01..30, exponential
                for (size_t li = 0; li < t.lfos.size(); ++li)
                    if (t.lfos[li].id == fxid) {
                        rt.kind = ModState::Kind::LfoRate;
                        rt.lfoTrack = static_cast<uint16_t>(ti);
                        rt.lfoIdx = static_cast<uint16_t>(li);
                        rt.min = 0.01f; rt.max = 30.0f; rt.log = true; rt.base = static_cast<float>(t.lfos[li].hz);
                        ok = true;
                        break;
                    }
            } else if (dest == "midi") {
                issues.insert("feature: modulating MIDI-fx parameters (dest midi)");
            } else if (dest == "macro") {
                const long mi = std::strtol(pkey.c_str(), nullptr, 10);
                if (mi >= 0 && static_cast<size_t>(mi) < t.macros.size()) {
                    const auto& m = t.macros[static_cast<size_t>(mi)];
                    rt.kind = ModState::Kind::Macro;
                    rt.subStart = static_cast<uint32_t>(mod->subs.size());
                    for (const auto& tg : m.targets) {
                        if (tg.dest == "macro" || tg.dest == "midi" || tg.dest == "lfo") continue;  // no recursion
                        if (auto lf = leaf(ti, tg.dest, tg.fxId, tg.pkey)) {
                            mod->subs.push_back({lf->addr, lf->min, lf->max});
                            ++rt.subLen;
                        }
                    }
                    rt.min = 0.0f; rt.max = 1.0f; rt.log = false; rt.base = static_cast<float>(m.value);
                    ok = true;
                }
            } else if (auto lf = leaf(ti, dest, fxid, pkey)) {
                rt.kind = ModState::Kind::Param;
                rt.addr = lf->addr; rt.min = lf->min; rt.max = lf->max; rt.log = lf->log; rt.base = lf->base;
                ok = true;
            }
            if (ok) { made = static_cast<uint32_t>(mod->routes.size()); mod->routes.push_back(rt); }
        }
        routeCache.emplace(ck, made);
        return made;
    };

    auto masterRoute = [&](const std::string& key) -> std::optional<uint32_t> {
        const std::string ck = "master|" + key;
        if (auto it = routeCache.find(ck); it != routeCache.end()) return it->second;
        std::optional<uint32_t> made;
        const auto parts = splitKey(key);
        if (parts.size() >= 3) {
            ModState::Route rt;
            rt.kind = ModState::Kind::Param;
            bool ok = false;
            if (parts[0] == "mix" && parts[2] == "gain") {
                rt.addr = {kTargetMaster, kSlotMixer, kMixGain};
                rt.min = -48.0f; rt.max = 6.0f; rt.log = false; rt.base = static_cast<float>(p.meta.masterGainDb);
                ok = true;
            } else if (parts[0] == "fx") {
                for (const auto& bf : masterBuilt)
                    if (bf.id == parts[1]) {
                        const int pi = findParam(bf.specs, parts[2]);
                        if (pi < 0) break;
                        const ParamSpec& sp = bf.specs[size_t(pi)];
                        const auto it = bf.spec->params.find(parts[2]);
                        rt.addr = {kTargetMaster, bf.slot, sp.index};
                        rt.min = sp.min; rt.max = sp.max; rt.log = sp.curve == Curve::Exponential && sp.min > 0 && sp.max > 0;
                        rt.base = it != bf.spec->params.end() ? static_cast<float>(it->second) : sp.def;
                        ok = true;
                        break;
                    }
            }
            if (ok) { made = static_cast<uint32_t>(mod->routes.size()); mod->routes.push_back(rt); }
        }
        routeCache.emplace(ck, made);
        return made;
    };

    for (size_t ti = 0; ti < nTracks; ++ti) {
        const auto& t = p.tracks[ti];
        auto& tm = mod->tracks[ti];
        for (size_t si = 0; si < p.scenes.size(); ++si) {  // session clip envelopes, one lane per (scene, key)
            auto it = p.clips.find(t.id + "|" + p.scenes[si]);
            if (it == p.clips.end()) continue;
            for (auto& [key, pts] : it->second.env) {
                if (pts.empty()) continue;
                if (auto r = trackRoute(ti, key)) tm.env.push_back({*r, static_cast<uint16_t>(si), it->second.len > 0 ? it->second.len : bar, ptsOf(pts)});
            }
        }
        for (auto& [ak, a] : p.arr) {  // arrangement clip envelopes
            if (a.trackId != t.id) continue;
            for (auto& [key, pts] : a.clip.env) {
                if (pts.empty()) continue;
                if (auto r = trackRoute(ti, key)) tm.arrEnv.push_back({*r, a.start, a.clip.len, a.clip.len > 0 ? a.clip.len : bar, ptsOf(pts)});
            }
        }
        for (auto& [key, pts] : t.autoLanes) {  // track automation lanes (arrangement mode, absolute ticks)
            if (pts.empty()) continue;
            if (auto r = trackRoute(ti, key)) tm.autoLanes.push_back({*r, ptsOf(pts)});
        }
        for (const auto& l : t.lfos) {
            ModState::LfoState ls;
            ls.on = l.on;
            ls.shape = static_cast<uint32_t>(std::max(l.shape, 0));
            ls.sync = l.sync;
            ls.divTicks = lfoDivTicks(l.rate);
            ls.hz = ls.effHz = l.hz;
            ls.depth = static_cast<float>(l.depth);
            ls.phase = l.phase;
            std::vector<std::string> keys;  // targets plus the legacy single dest, deduplicated
            auto pushKey = [&](const std::string& d, const std::string& f, const std::string& pk) {
                if (d.empty() || pk.empty()) return;
                const std::string k = d + "|" + f + "|" + pk;
                if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
            };
            for (auto& tg : l.targets) pushKey(tg.dest, tg.fxId, tg.pkey);
            pushKey(l.dest, l.fxId, l.pkey);
            for (auto& k : keys) if (auto r = trackRoute(ti, k)) ls.targets.push_back(*r);
            tm.lfos.push_back(std::move(ls));
        }
        // macros with targets: the knob itself is a live source, and its value is a live parameter
        for (size_t mi = 0; mi < t.macros.size(); ++mi) {
            res.insert(t.id + "|macro|" + std::to_string(mi), {static_cast<uint16_t>(ti), kSlotMacro, static_cast<uint16_t>(mi)});
            if (t.macros[mi].targets.empty()) continue;
            if (auto r = trackRoute(ti, "macro||" + std::to_string(mi)))
                if (mod->routes[*r].subLen > 0) tm.macros.push_back({*r, static_cast<uint16_t>(mi), static_cast<float>(t.macros[mi].value)});
        }
        for (size_t li = 0; li < t.lfos.size(); ++li) {
            const std::string base = t.id + "|lfo" + std::to_string(li) + "|";
            res.insert(base + "depth", {static_cast<uint16_t>(ti), kSlotLfo, static_cast<uint16_t>(li * 4 + kLfoDepth)});
            res.insert(base + "hz", {static_cast<uint16_t>(ti), kSlotLfo, static_cast<uint16_t>(li * 4 + kLfoHz)});
            res.insert(base + "phase", {static_cast<uint16_t>(ti), kSlotLfo, static_cast<uint16_t>(li * 4 + kLfoPhase)});
        }
        for (size_t mi = 0; mi < t.morph.size(); ++mi) {   // morph maps (B5): the stick is a live parameter; targets resolve like any modulation target
            const auto& ms = t.morph[mi];
            const std::string base = t.id + "|morph" + std::to_string(mi) + "|";
            res.insert(base + "x", {static_cast<uint16_t>(ti), kSlotMorph, static_cast<uint16_t>(mi * 4 + kMorphX)});
            res.insert(base + "y", {static_cast<uint16_t>(ti), kSlotMorph, static_cast<uint16_t>(mi * 4 + kMorphY)});
            if (!ms.on) continue;
            const std::string where = "track " + t.id + " morph map " + std::to_string(mi);
            ModState::MorphState st;
            st.track = static_cast<uint16_t>(ti);
            st.idx = static_cast<uint16_t>(mi);
            st.x = st.sx = std::clamp(static_cast<float>(ms.x), 0.0f, 1.0f);
            st.y = st.sy = std::clamp(static_cast<float>(ms.y), 0.0f, 1.0f);
            st.method = ms.method == "rbf" ? dsp::MorphMethod::Rbf : dsp::MorphMethod::Idw;
            st.power = static_cast<float>(ms.power);
            st.width = static_cast<float>(ms.width);
            std::vector<size_t> kept;           // the columns (targets) that resolved
            std::vector<float> baseUnit;        // where each one sits now: what a missing anchor value falls back to
            for (size_t k = 0; k < ms.targets.size(); ++k) {
                const auto& tg = ms.targets[k];
                if (tg.dest.empty() || tg.pkey.empty() || tg.dest == "macro" || tg.dest == "midi" || tg.dest == "lfo") { issues.insert(where + ": target " + std::to_string(k) + " is not a parameter"); continue; }
                const auto lf = leaf(ti, tg.dest, tg.fxId, tg.pkey);
                if (!lf) { issues.insert(where + ": target '" + tg.pkey + "' not found"); continue; }
                ModState::MorphTarget mt;
                mt.addr = lf->addr; mt.min = lf->min; mt.max = lf->max; mt.log = lf->log;
                mt.curve = k < ms.curves.size() ? static_cast<float>(ms.curves[k]) : 1.0f;
                st.targets.push_back(mt);
                kept.push_back(k);
                const float span = lf->log ? std::log(lf->max) - std::log(lf->min) : lf->max - lf->min;
                baseUnit.push_back(span > 0.0f ? std::clamp((lf->log ? std::log(std::max(lf->base, lf->min)) - std::log(lf->min) : lf->base - lf->min) / span, 0.0f, 1.0f) : 0.0f);
            }
            if (kept.empty() || ms.anchors.empty()) continue;
            for (const auto& a : ms.anchors) {
                st.ax.push_back(std::clamp(static_cast<float>(a.x), 0.0f, 1.0f));
                st.ay.push_back(std::clamp(static_cast<float>(a.y), 0.0f, 1.0f));
                for (size_t c = 0; c < kept.size(); ++c) st.au.push_back(kept[c] < a.values.size() ? std::clamp(static_cast<float>(a.values[kept[c]]), 0.0f, 1.0f) : baseUnit[c]);
            }
            st.w.assign(st.ax.size(), 0.0f);
            mod->morphs.push_back(std::move(st));
        }
        for (const auto& pf : t.perf) {   // performance routes: the same targets as an LFO's, driven by the tracker
            if (!pf.on) continue;
            const PerfSource src = perfSourceFromName(pf.source);
            double lo = pf.srcMin, hi = pf.srcMax;
            if (lo == 0.0 && hi == 0.0) perfDefaultRange(src, lo, hi);
            std::vector<std::string> done;
            for (const auto& tg : pf.targets) {
                if (tg.dest.empty() || tg.pkey.empty()) continue;
                const std::string k = tg.dest + "|" + tg.fxId + "|" + tg.pkey;
                if (std::find(done.begin(), done.end(), k) != done.end()) continue;
                done.push_back(k);
                if (auto r = trackRoute(ti, k)) tm.perfs.push_back({*r, src, lo, hi});
            }
        }
    }
    for (auto& [key, pts] : p.masterAuto) {  // master automation (arrangement mode)
        if (pts.empty()) continue;
        if (auto r = masterRoute(key)) mod->masterAuto.push_back({*r, ptsOf(pts)});
    }
    mod->finalize();
    if (!mod->empty()) g.setModulation(std::move(mod));

    g.finalize();
    out.unsupported.assign(issues.begin(), issues.end());
    return out;
}

}  // namespace ddaw::engine
