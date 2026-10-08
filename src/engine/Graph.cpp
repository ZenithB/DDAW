#include "engine/Graph.h"

#include <chrono>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

#include "dsp/FastSin.h"

namespace ddaw::engine {

namespace {

float dbToLin(float db) noexcept { return std::pow(10.0f, db / 20.0f); }

// Web Audio StereoPanner law (matches synthyy's stereo_pan).
inline void stereoPan(float& l, float& r, float pan) noexcept {
    const float p = std::clamp(pan, -1.0f, 1.0f);
    if (p == 0.0f) return;
    const float x = p <= 0.0f ? p + 1.0f : p;
    const float gl = std::cos(x * float(std::numbers::pi / 2)), gr = std::sin(x * float(std::numbers::pi / 2));
    if (p <= 0.0f) { const float nl = l + r * gl, nr = r * gr; l = nl; r = nr; }
    else           { const float nl = l * gl, nr = r + l * gr; l = nl; r = nr; }
}

// Advance a smoother by n samples and return the landed value (per-chunk gains, like synthyy's advance()).
inline float advance(dsp::Smoother& s, int n) noexcept {
    for (int i = 0; i < n; ++i) s.next();
    return s.current();
}

inline void applyGain(float* l, float* r, int n, float g) noexcept {
    if (g == 1.0f) return;
    for (int k = 0; k < n; ++k) { l[k] *= g; r[k] *= g; }
}

inline void addScaled(float* dl, float* dr, const float* sl, const float* sr, int n, float g) noexcept {
    for (int k = 0; k < n; ++k) { dl[k] += sl[k] * g; dr[k] += sr[k] * g; }
}

}  // namespace

int TrackStrip::latencySamples() const {
    int n = inst ? inst->latencySamples() : 0;
    for (auto& f : fx) n += f.dev->latencySamples();
    return n;
}

Graph::Graph(uint32_t epoch, double sr) : epoch_(epoch), sr_(sr), invSr_(1.0 / std::max(sr, 1.0)) {
    masterGain_.prepare(sr, 15.0f);
    masterGain_.snap(1.0f);
    mixL_.assign(kMaxBlock, 0.0f); mixR_.assign(kMaxBlock, 0.0f);
    tmpL_.assign(kMaxBlock, 0.0f); tmpR_.assign(kMaxBlock, 0.0f);
    pdcL_.assign(kMaxBlock, 0.0f); pdcR_.assign(kMaxBlock, 0.0f); sdL_.assign(kMaxBlock, 0.0f); sdR_.assign(kMaxBlock, 0.0f);
}

TrackStrip& Graph::addTrack() {
    tracks_.emplace_back();
    auto& t = tracks_.back();
    for (auto* s : {&t.instOut, &t.pan, &t.fader, &t.muteGain, &t.sendA, &t.sendB}) s->prepare(sr_, 15.0f);
    t.instOut.snap(1.0f); t.pan.snap(0.0f); t.fader.snap(1.0f); t.muteGain.snap(1.0f); t.sendA.snap(0.0f); t.sendB.snap(0.0f);
    t.busSends.reserve(kMaxBusSends);
    return t;
}

FxSlot& Graph::addMasterFx() {
    masterFx_.emplace_back();
    masterFx_.back().out.prepare(sr_, 15.0f);
    masterFx_.back().out.snap(1.0f);
    return masterFx_.back();
}

ReturnNode& Graph::addReturn() {
    returns_.emplace_back();
    auto& r = returns_.back();
    r.gain.prepare(sr_, 15.0f);
    r.gain.snap(1.0f);
    r.bufL.assign(kMaxBlock, 0.0f);
    r.bufR.assign(kMaxBlock, 0.0f);
    return r;
}

BackEdge& Graph::addBackEdge(int target, int delaySamples) {
    backEdges_.emplace_back();
    auto& e = backEdges_.back();
    e.target = target;
    e.delaySamples = delaySamples;
    e.dl.prepare(delaySamples + 2);
    e.dr.prepare(delaySamples + 2);
    return e;
}

void Graph::setMasterGainDb(float db) { masterGain_.snap(dbToLin(db)); }

uint32_t Graph::addAudioRoute(const AudioRoute& r) {
    aroutes_.push_back(r);
    const uint32_t idx = uint32_t(aroutes_.size() - 1);
    tracks_[size_t(r.dstTrack)].aroutes.push_back(idx);
    if (r.kind == AudioRoute::Kind::Track && r.src >= 0) {
        auto& s = tracks_[size_t(r.src)];
        s.tapped = true;
        if (s.tap.empty()) s.tap.assign(size_t(kMaxBlock), 0.0f);
    }
    return idx;
}

namespace {
void publishReduction(MeterBank& m, int track, int fx, const EffectDevice& d) noexcept {
    const int bands = std::min(d.reductionBands(), kMaxBands);
    for (int b = 0; b < bands; ++b) m.setFxGr(track, fx, b, d.reductionDb(b));
}
}  // namespace

void Graph::addKey(int dstTrack, size_t fx, int src, float hpfHz) {
    FxSlot& f = dstTrack < 0 ? masterFx_[fx] : tracks_[size_t(dstTrack)].fx[fx];
    auto& s = tracks_[size_t(src)];
    s.tapped = true;
    if (s.tap.empty()) s.tap.assign(size_t(kMaxBlock), 0.0f);
    f.keySrc = src;
    f.keyHpfHz = std::max(hpfHz, 0.0f);
    f.keyBuf.assign(size_t(kMaxBlock), 0.0f);
    f.keyHp.prepare(float(sr_));
    if (f.keyHpfHz > 0.0f) f.keyHp.setCutoffQ(f.keyHpfHz, 0.7071f);
}

ModInputs Graph::inputsFor(FxSlot& f, int n) noexcept {
    ModInputs in = f.mod.inputs();
    if (f.keySrc < 0) return in;
    const float* s = tracks_[size_t(f.keySrc)].tap.data();
    if (f.keyHpfHz > 0.0f) {
        for (int i = 0; i < n; ++i) f.keyBuf[size_t(i)] = f.keyHp.processSample(s[i]);
        s = f.keyBuf.data();
    }
    in.keyL = in.keyR = s;
    return in;
}

// Fill this track's devices' audio-rate buffers for the chunk: every route adds source * depth * halfRange.
void Graph::renderRoutes(TrackStrip& t, int n) noexcept {
    const auto modOf = [&](const AudioRoute& r) -> AModBufs& { return r.dstFx < 0 ? t.instMod : t.fx[size_t(r.dstFx)].mod; };
    for (const uint32_t ri : t.aroutes) { const AudioRoute& r = aroutes_[ri]; modOf(r).ptrs[r.ordinal] = nullptr; }
    for (const uint32_t ri : t.aroutes) {
        AudioRoute& r = aroutes_[ri];
        AModBufs& m = modOf(r);
        float* b = m.bufs[r.ordinal].data();
        if (m.ptrs[r.ordinal] == nullptr) { std::fill_n(b, n, 0.0f); m.ptrs[r.ordinal] = b; }
        const float g = r.depth * r.halfRange;
        if (r.kind == AudioRoute::Kind::Osc) {
            const double inc = std::min(double(r.hz) * invSr_, 0.5);
            double ph = r.phase;
            switch (r.shape) {
                case 1: for (int i = 0; i < n; ++i) { b[i] += g * (ph < 0.5 ? float(4.0 * ph - 1.0) : float(3.0 - 4.0 * ph)); ph += inc; if (ph >= 1.0) ph -= 1.0; } break;
                case 2: for (int i = 0; i < n; ++i) { b[i] += g * float(2.0 * ph - 1.0); ph += inc; if (ph >= 1.0) ph -= 1.0; } break;
                case 3: for (int i = 0; i < n; ++i) { b[i] += g * (ph < 0.5 ? 1.0f : -1.0f); ph += inc; if (ph >= 1.0) ph -= 1.0; } break;
                default: for (int i = 0; i < n; ++i) { b[i] += g * dsp::fastSin2Pi(float(ph)); ph += inc; if (ph >= 1.0) ph -= 1.0; } break;
            }
            r.phase = ph;
        } else if (r.src >= 0) {
            const float* s = tracks_[size_t(r.src)].tap.data();
            if (!r.follow) for (int i = 0; i < n; ++i) b[i] += g * s[i];
            else {
                float env = r.env;
                for (int i = 0; i < n; ++i) { const float x = std::abs(s[i]); env += (x > env ? r.atk : r.rel) * (x - env); b[i] += g * env; }
                r.env = env;
            }
        }
    }
}

void Graph::finalize() {
    if (order_.empty()) {  // hand-built graphs: tracks in document order, then buses
        for (size_t i = 0; i < tracks_.size(); ++i) if (!tracks_[i].isBus) order_.push_back(static_cast<int>(i));
        for (size_t i = 0; i < tracks_.size(); ++i) if (tracks_[i].isBus) order_.push_back(static_cast<int>(i));
    }
    // Buses (and any track routed into one) accumulate input in a buffer.
    for (auto& t : tracks_) if (t.isBus) { t.bufL.assign(kMaxBlock, 0.0f); t.bufR.assign(kMaxBlock, 0.0f); }
    // Delay compensation. Every route into a summing point (a bus input, the master) must arrive with the same delay, so the
    // paths of one source that reach it by different ways (its output and its sends) stay aligned, and so do different sources.
    // Walk the strips in render order (sources before their targets): a strip's output is `inLat` (the latest of what feeds it)
    // plus its own chain; each route then gets a delay of (the destination's inLat - this output). A feedback route (a bus cycle's
    // delayed edge) carries its own delay and is left out. Sends count when their level is above 0 as the graph is built (a send
    // raised from 0 later reaches a bus aligned for the others, not for itself, until the next build).
    const size_t nTracks = tracks_.size();
    std::vector<int> inLat(nTracks, 0), outLat(nTracks, 0);
    std::vector<int> retIn(returns_.size(), 0);
    int masterIn = 0;
    struct Edge { int src; enum Kind { Out, A, B, Send, Ret } kind; size_t idx; int dstTrack; int dstRet; };   // dstTrack: -1 master
    std::vector<Edge> edges;
    for (size_t i = 0; i < nTracks; ++i) {
        const auto& t = tracks_[i];
        if (t.outKind == TrackStrip::Out::Master) edges.push_back({int(i), Edge::Out, 0, -1, -1});
        else if (t.outKind == TrackStrip::Out::Bus) edges.push_back({int(i), Edge::Out, 0, t.outTarget, -1});
        if (!t.isBus) {
            const auto sendDst = [&](int bus, size_t retIdx, Edge::Kind k) {
                if (bus >= 0) edges.push_back({int(i), k, 0, bus, -1});
                else if (retIdx < returns_.size()) edges.push_back({int(i), k, 0, -1, int(retIdx)});
            };
            if (t.sendA.target() > 0.0f) sendDst(sendABus_, 0, Edge::A);
            if (t.sendB.target() > 0.0f) sendDst(sendBBus_, 1, Edge::B);
        }
        for (size_t s = 0; s < t.busSends.size(); ++s)
            if (t.busSends[s].level.target() > 0.0f) edges.push_back({int(i), Edge::Send, s, t.busSends[s].target, -1});
    }
    std::vector<char> fed(nTracks, 0), retFed(returns_.size(), 0);   // buses and returns that something is routed into; the rest are idle and cost nothing
    for (size_t i = 0; i < nTracks; ++i) fed[i] = !tracks_[i].isBus;
    const auto arrive = [&](const Edge& e, int out) {
        if (e.dstRet >= 0) { retIn[size_t(e.dstRet)] = std::max(retIn[size_t(e.dstRet)], out); retFed[size_t(e.dstRet)] = 1; }
        else if (e.dstTrack >= 0) { inLat[size_t(e.dstTrack)] = std::max(inLat[size_t(e.dstTrack)], out); fed[size_t(e.dstTrack)] = 1; }
        else masterIn = std::max(masterIn, out);
    };
    for (const int i : order_) {
        if (!fed[size_t(i)]) continue;
        outLat[size_t(i)] = inLat[size_t(i)] + tracks_[size_t(i)].latencySamples();
        for (const auto& e : edges) if (e.src == i) arrive(e, outLat[size_t(i)]);
    }
    std::vector<int> retOut(returns_.size(), 0);
    for (size_t r = 0; r < returns_.size(); ++r) {
        if (!retFed[r]) continue;
        retOut[r] = retIn[r] + returns_[r].dev->latencySamples();
        masterIn = std::max(masterIn, retOut[r]);
    }
    const auto dstIn = [&](const Edge& e) { return e.dstRet >= 0 ? retIn[size_t(e.dstRet)] : e.dstTrack >= 0 ? inLat[size_t(e.dstTrack)] : masterIn; };
    for (auto& t : tracks_) { t.pdcOut = {}; t.pdcA = {}; t.pdcB = {}; t.pdcSend.assign(t.busSends.size(), {}); }
    for (const auto& e : edges) {
        auto& t = tracks_[size_t(e.src)];
        const int d = std::max(dstIn(e) - outLat[size_t(e.src)], 0);
        switch (e.kind) {
            case Edge::Out: t.pdcOut.prepare(d); break;
            case Edge::A: t.pdcA.prepare(d); break;
            case Edge::B: t.pdcB.prepare(d); break;
            case Edge::Send: t.pdcSend[e.idx].prepare(d); break;
            case Edge::Ret: break;
        }
    }
    for (size_t r = 0; r < returns_.size(); ++r) returns_[r].pdcOut.prepare(retFed[r] ? std::max(masterIn - retOut[r], 0) : 0);
    exprByUid_.clear();
    for (size_t i = 0; i < exprSeqs_.size(); ++i) if (exprSeqs_[i].uid) exprByUid_.push_back({exprSeqs_[i].uid, int(i)});
    std::sort(exprByUid_.begin(), exprByUid_.end());
    if (exprByUid_.size() != exprSeqs_.size()) exprByUid_.clear();   // seqs without an identity: fall back to the scan
    int masterLat = 0;
    for (auto& f : masterFx_) masterLat += f.dev->latencySamples();
    latency_ = masterIn + masterLat;
}

double Graph::nextBoundaryTicks(double cur) const noexcept {
    cur = std::round(cur);
    if (launchQ_ == 0.0) return cur + 4.0;
    const double q = launchQ_ * barTicks();
    return std::ceil((cur + 1.0) / q) * q;
}

void Graph::relocateAll(TransportMode mode, double now) noexcept {
    nPlayers_ = 0;   // a position jump: the notes that were playing are over
    for (auto& t : tracks_) t.sched.relocate(mode, now);
}

void Graph::inheritNotes(const Graph& old) noexcept {
    for (size_t i = 0; i < old.nActive_; ++i) {
        const Active& a = old.active_[i];
        if (a.track < tracks_.size()) noteOn(a.track, a.pitch, a.vel, a.noteId);
    }
    for (size_t i = 0; i < old.nGates_; ++i)
        if (old.gates_[i].track < tracks_.size()) scheduleGate(old.gates_[i].frame, old.gates_[i].track, old.gates_[i].noteId);
    // A sounding note keeps playing its curves, as edited: they are found again by the note's identity. The note's timing
    // stays what it was when it started. A note whose curves are gone (or which was deleted) just holds neutral expression.
    for (size_t i = 0; i < old.nPlayers_; ++i) {
        const ExprPlayer& p = old.players_[i];
        const int seq = p.uid ? findExprSeq(p.uid) : -1;
        if (seq < 0 || p.track >= tracks_.size() || nPlayers_ >= kMaxExprPlayers) continue;
        players_[nPlayers_++] = {p.track, p.noteId, p.start, p.end, seq, {1e30f, 1e30f, 1e30f}, {0, 0, 0}, p.uid};   // `last` reset: the new voice gets the current values at once
    }
}

int Graph::findExprSeq(uint64_t uid) const noexcept {
    if (exprByUid_.size() == exprSeqs_.size()) {   // indexed by finalize(): binary search
        const auto it = std::lower_bound(exprByUid_.begin(), exprByUid_.end(), uid, [](const std::pair<uint64_t, int>& a, uint64_t u) { return a.first < u; });
        return it != exprByUid_.end() && it->first == uid ? it->second : -1;
    }
    for (size_t i = 0; i < exprSeqs_.size(); ++i) if (exprSeqs_[i].uid == uid) return int(i);   // hand-built graph, not finalized
    return -1;
}

void Graph::fireDucks(int src, uint8_t pitch) noexcept {
    const int pad = pitch % 8;
    for (const auto& d : ducks_) {
        if (d.src != src || (d.pitch >= 0 && d.pitch != pad)) continue;
        auto& chainFx = d.track >= 0 ? tracks_[static_cast<size_t>(d.track)].fx : masterFx_;
        if (static_cast<size_t>(d.fx) < chainFx.size()) chainFx[static_cast<size_t>(d.fx)].dev->trigger();
    }
}

bool Graph::scheduleGate(int64_t frame, uint16_t track, uint32_t noteId) noexcept {
    if (nGates_ >= kMaxGates) return false;  // far beyond any realistic overlap; the note rings until the next stop
    size_t i = nGates_;
    while (i > 0 && gates_[i - 1].frame > frame) { gates_[i] = gates_[i - 1]; --i; }  // stable sorted insert
    gates_[i] = {frame, track, noteId};
    ++nGates_;
    return true;
}

void Graph::fireGatesUpTo(int64_t frame) noexcept {
    size_t n = 0;
    while (n < nGates_ && gates_[n].frame <= frame) { noteOff(gates_[n].track, gates_[n].noteId); ++n; }
    if (n) { std::memmove(&gates_[0], &gates_[n], (nGates_ - n) * sizeof(Gate)); nGates_ -= n; }
}

void Graph::process(float* outL, float* outR, int n, const ProcessContext& ctx, MeterBank* meters, float meterDecay,
                    Metronome* metro) noexcept {
    std::fill_n(mixL_.data(), n, 0.0f);
    std::fill_n(mixR_.data(), n, 0.0f);
    for (auto& t : tracks_) if (t.isBus) { std::fill_n(t.bufL.data(), n, 0.0f); std::fill_n(t.bufR.data(), n, 0.0f); }
    for (auto& rn : returns_) { std::fill_n(rn.bufL.data(), n, 0.0f); std::fill_n(rn.bufR.data(), n, 0.0f); }
    float* tl = tmpL_.data();
    float* tr = tmpR_.data();

    for (const int i : order_) {
        auto& t = tracks_[static_cast<size_t>(i)];
        if (!t.aroutes.empty()) renderRoutes(t, n);
        if (t.isBus) {
            std::memcpy(tl, t.bufL.data(), size_t(n) * sizeof(float));
            std::memcpy(tr, t.bufR.data(), size_t(n) * sizeof(float));
            // the returning signal from a bus cycle (Feedback Chunk)
            for (const auto& e : backEdges_)
                if (e.target == i)
                    for (int k = 0; k < n; ++k) {
                        const int d = std::max(e.delaySamples - k, 1);
                        tl[k] += e.dl.read(d);
                        tr[k] += e.dr.read(d);
                    }
            // Feedback loop: delayed own output re-enters the input
            if (t.fb) {
                for (int k = 0; k < n; ++k) {
                    const float g = t.fb->gain.next();
                    const int d = std::max(t.fb->delaySamples - k, 1);
                    tl[k] += t.fb->dl.read(d) * g;
                    tr[k] += t.fb->dr.read(d) * g;
                }
            }
        } else {
            std::fill_n(tl, n, 0.0f);
            std::fill_n(tr, n, 0.0f);
            if (t.monitorIn && monL_) for (int k = 0; k < n; ++k) { tl[k] += monL_[k]; tr[k] += monR_[k]; }
#ifdef DDAW_GRAPH_PROFILE
            const auto pt0 = std::chrono::steady_clock::now();
#endif
            if (t.inst) t.inst->process(tl, tr, n, ctx, t.instMod.inputs());
#ifdef DDAW_GRAPH_PROFILE
            { const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - pt0).count()); profInstNs[i & 127] += ns; profInstMax[i & 127] = std::max(profInstMax[i & 127], ns); }
#endif
            if (!t.audio.empty()) {
                const double tps = ctx.bpm / 60.0 * kPpq / ctx.sampleRate;
                t.audio.render(tl, tr, n, ctx.positionTicks, tps, ctx.arrangement ? TransportMode::Arrangement : TransportMode::Session,
                               ctx.playing, t.sched.audioSlot(), t.sched.anchor());
            }
        }
        applyGain(tl, tr, n, advance(t.instOut, n));
#ifdef DDAW_GRAPH_PROFILE
        const auto pf0 = std::chrono::steady_clock::now();
#endif
        for (size_t fi = 0; fi < t.fx.size(); ++fi) {
            auto& f = t.fx[fi];
            f.dev->process(tl, tr, n, ctx, inputsFor(f, n));
            applyGain(tl, tr, n, advance(f.out, n));
            if (meters && fi < size_t(kMaxFxMeters) && f.dev->hasReductionMeter()) publishReduction(*meters, i, int(fi), *f.dev);
        }
#ifdef DDAW_GRAPH_PROFILE
        { const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - pf0).count()); profFxNs[i & 127] += ns; profFxMax[i & 127] = std::max(profFxMax[i & 127], ns); }
#endif
        // the modulation tap: after the effects, before pan, fader and mute (a muted track can still modulate)
        if (t.tapped) for (int k = 0; k < n; ++k) t.tap[size_t(k)] = 0.5f * (tl[k] + tr[k]);
        // pan -> fader -> mute, per-sample smoothing
        for (int k = 0; k < n; ++k) {
            float a = tl[k], b = tr[k];
            stereoPan(a, b, t.pan.next());
            const float g = t.fader.next() * t.muteGain.next();
            tl[k] = a * g; tr[k] = b * g;
        }
        // the output route's signal: delayed for compensation when this route is shorter than the longest into its destination
        const float* ol = tl;
        const float* orr = tr;
        if (t.pdcOut.samples > 0) { t.pdcOut.run(tl, tr, pdcL_.data(), pdcR_.data(), n); ol = pdcL_.data(); orr = pdcR_.data(); }
        if (meters) {
            float pk, rm;
            MeterBank::peakRms(ol, orr, n, pk, rm);
            meters->setTrack(i, pk, rm, meterDecay);
        }
        const bool soloMuted = !t.audible;  // mute/solo is baked into muteGain; `audible` only mirrors it for tools
        (void)soloMuted;

        // buses have no A/B sends (engine-tone wireSends skips them)
        const float ga = t.isBus ? 0.0f : advance(t.sendA, n);
        const float gb = t.isBus ? 0.0f : advance(t.sendB, n);
        float busLevel[kMaxBusSends];
        const size_t nBus = t.busSends.size();
        for (size_t s = 0; s < nBus; ++s) busLevel[s] = advance(t.busSends[s].level, n);

        // distribute (post-fader)
        switch (t.outKind) {
            case TrackStrip::Out::Master:
                for (int k = 0; k < n; ++k) { mixL_[size_t(k)] += ol[k]; mixR_[size_t(k)] += orr[k]; }
                break;
            case TrackStrip::Out::Delayed: {
                auto& e = backEdges_[static_cast<size_t>(t.outTarget)];  // the target picks it up a lap later
                for (int k = 0; k < n; ++k) { e.dl.write(ol[k]); e.dr.write(orr[k]); }
                break;
            }
            case TrackStrip::Out::Bus: {
                auto& dst = tracks_[static_cast<size_t>(t.outTarget)];
                for (int k = 0; k < n; ++k) { dst.bufL[size_t(k)] += ol[k]; dst.bufR[size_t(k)] += orr[k]; }
                break;
            }
        }
        // a send: the strip's signal, delayed on this route when the send path needs it, scaled into the destination
        const auto send = [&](PdcLine& line, float* dstL, float* dstR, float level) {
            const float* sl = tl;
            const float* sr = tr;
            if (line.samples > 0) { line.run(tl, tr, sdL_.data(), sdR_.data(), n); sl = sdL_.data(); sr = sdR_.data(); }
            if (level > 0.0f) addScaled(dstL, dstR, sl, sr, n, level);
        };
        for (size_t s = 0; s < nBus; ++s) {
            auto& dst = tracks_[static_cast<size_t>(t.busSends[s].target)];
            send(t.pdcSend[s], dst.bufL.data(), dst.bufR.data(), busLevel[s]);
        }
        if (!t.isBus) {
            if (sendABus_ >= 0) { auto& d = tracks_[static_cast<size_t>(sendABus_)]; send(t.pdcA, d.bufL.data(), d.bufR.data(), ga); }
            else if (!returns_.empty()) send(t.pdcA, returns_[0].bufL.data(), returns_[0].bufR.data(), ga);
            if (sendBBus_ >= 0) { auto& d = tracks_[static_cast<size_t>(sendBBus_)]; send(t.pdcB, d.bufL.data(), d.bufR.data(), gb); }
            else if (returns_.size() > 1) send(t.pdcB, returns_[1].bufL.data(), returns_[1].bufR.data(), gb);
        }
        if (t.fb) for (int k = 0; k < n; ++k) { t.fb->dl.write(ol[k]); t.fb->dr.write(orr[k]); }
    }

    // legacy return channels -> master
    for (auto& rn : returns_) {
        rn.dev->process(rn.bufL.data(), rn.bufR.data(), n, ctx, {});
        const float g = advance(rn.gain, n);
        if (rn.pdcOut.samples > 0) { rn.pdcOut.run(rn.bufL.data(), rn.bufR.data(), sdL_.data(), sdR_.data(), n); addScaled(mixL_.data(), mixR_.data(), sdL_.data(), sdR_.data(), n, g); }
        else addScaled(mixL_.data(), mixR_.data(), rn.bufL.data(), rn.bufR.data(), n, g);
    }

    // the metronome mixes into the master input, ahead of the master gain
    if (metro) metro->process(mixL_.data(), mixR_.data(), n);

    // master: gain -> fx chain
    for (int k = 0; k < n; ++k) {
        const float g = masterGain_.next();
        mixL_[size_t(k)] *= g; mixR_[size_t(k)] *= g;
    }
    for (size_t fi = 0; fi < masterFx_.size(); ++fi) {
        auto& f = masterFx_[fi];
        f.dev->process(mixL_.data(), mixR_.data(), n, ctx, inputsFor(f, n));
        applyGain(mixL_.data(), mixR_.data(), n, advance(f.out, n));
        if (meters && fi < size_t(kMaxFxMeters) && f.dev->hasReductionMeter()) publishReduction(*meters, -1, int(fi), *f.dev);
    }
    std::memcpy(outL, mixL_.data(), size_t(n) * sizeof(float));
    std::memcpy(outR, mixR_.data(), size_t(n) * sizeof(float));
}

void Graph::warmUp(int chunks) {
    std::vector<float> l(kMaxBlock, 0.0f), r(kMaxBlock, 0.0f);
    ProcessContext ctx{sr_, 0.0, 120.0, false, false, 0.0, 0.0, 4, 4};
    ctx.warmup = true;
    for (int c = 0; c < chunks; ++c) process(l.data(), r.data(), kMaxBlock, ctx, nullptr, 1.0f, nullptr);
    // back to the freshly built state (what the devices hold is now warm in cache, not different in value)
    for (auto& t : tracks_) {
        if (t.inst) t.inst->reset();
        for (auto& f : t.fx) f.dev->reset();
    }
    for (auto& f : masterFx_) f.dev->reset();
    for (auto& rn : returns_) rn.dev->reset();
}

void Graph::setParam(const ParamAddr& a, float value) noexcept {
    if (a.target == kTargetMaster) {
        if (a.slot == kSlotMixer) {
            if (a.param == kMixGain) masterGain_.setTarget(dbToLin(value));
        } else if (a.slot >= kSlotFx0 && size_t(a.slot - kSlotFx0) < masterFx_.size()) {
            auto& f = masterFx_[size_t(a.slot - kSlotFx0)];
            if (a.param == kParamOut) f.out.setTarget(dbToLin(value)); else f.dev->setParam(a.param, value);
        }
        return;
    }
    if (a.target >= tracks_.size()) return;
    if (a.slot == kSlotMacro) { if (mod_) mod_->setMacro(a.target, a.param, value); return; }
    if (a.slot == kSlotMorph) { if (mod_) mod_->setMorph(a.target, a.param / 4, uint16_t(a.param % 4), value); return; }
    if (a.slot == kSlotArate) {
        const int spec = a.param / 4, field = a.param % 4;
        for (const uint32_t ri : tracks_[a.target].aroutes) {
            AudioRoute& r = aroutes_[ri];
            if (r.specIndex != spec) continue;
            if (field == kArateDepth) r.depth = std::clamp(value, -1.0f, 1.0f);
            else if (field == kArateHz) r.hz = std::clamp(value, 0.05f, 12000.0f);
        }
        return;
    }
    if (a.slot == kSlotLfo) { if (mod_) mod_->setLfo(a.target, a.param / 4, uint16_t(a.param % 4), value); return; }
    auto& t = tracks_[a.target];
    if (a.slot == kSlotMixer) {
        switch (a.param) {
            case kMixGain: t.fader.setTarget(dbToLin(value)); break;
            case kMixPan: t.pan.setTarget(std::clamp(value, -1.0f, 1.0f)); break;
            case kMixSendA: t.sendA.setTarget(std::max(value, 0.0f)); break;
            case kMixSendB: t.sendB.setTarget(std::max(value, 0.0f)); break;
            case kMixFeedback: if (t.fb) t.fb->gain.setTarget(std::clamp(value, 0.0f, 0.95f)); break;
            default:
                if (a.param >= kMixBusSend0 && size_t(a.param - kMixBusSend0) < t.busSends.size())
                    t.busSends[size_t(a.param - kMixBusSend0)].level.setTarget(std::max(value, 0.0f));
                break;
        }
    } else if (a.slot == kSlotInst) {
        if (a.param == kParamOut) t.instOut.setTarget(dbToLin(value));
        else if (t.inst) t.inst->setParam(a.param, value);
    } else if (a.slot >= kSlotFx0 && size_t(a.slot - kSlotFx0) < t.fx.size()) {
        auto& f = t.fx[size_t(a.slot - kSlotFx0)];
        if (a.param == kParamOut) f.out.setTarget(dbToLin(value)); else f.dev->setParam(a.param, value);
    }
}

void Graph::noteOn(uint16_t track, uint8_t pitch, float vel, uint32_t noteId) noexcept {
    if (track >= tracks_.size() || !tracks_[track].inst) return;
    tracks_[track].inst->noteOn(pitch, vel, noteId);
    if (nActive_ < kMaxActive) active_[nActive_++] = {track, noteId, pitch, vel};
}

void Graph::noteOff(uint16_t track, uint32_t noteId) noexcept {
    if (track >= tracks_.size() || !tracks_[track].inst) return;
    tracks_[track].inst->noteOff(noteId);
    for (size_t i = 0; i < nActive_; ++i)
        if (active_[i].track == track && active_[i].noteId == noteId) { active_[i] = active_[--nActive_]; break; }
}

void Graph::performance(uint16_t track, const PerformanceFrame& f) noexcept {
    if (track < tracks_.size() && tracks_[track].inst) tracks_[track].inst->performance(f);
}

void Graph::startExpression(uint16_t track, uint32_t noteId, double startTick, double durTicks, int seq) noexcept {
    if (seq < 0 || size_t(seq) >= exprSeqs_.size() || track >= tracks_.size()) return;
    if (nPlayers_ >= kMaxExprPlayers) return;   // more expressive notes at once than anyone plays: the extras sound without
    players_[nPlayers_++] = {track, noteId, startTick, startTick + std::max(durTicks, 1.0), seq, {1e30f, 1e30f, 1e30f}, {0, 0, 0}, exprSeqs_[size_t(seq)].uid};
}

void Graph::updateExpression(double nowTick) noexcept {
    for (size_t i = 0; i < nPlayers_;) {
        ExprPlayer& p = players_[i];
        if (nowTick >= p.end) { p = players_[--nPlayers_]; continue; }   // the note is over
        const float t = float(std::max(nowTick - p.start, 0.0));
        const ExprSeq& s = exprSeqs_[size_t(p.seq)];
        for (int d = 0; d < 3; ++d) {
            const auto& pts = s.dim[size_t(d)];
            if (pts.empty()) continue;
            uint32_t& k = p.idx[size_t(d)];
            while (k + 1 < pts.size() && pts[k + 1].t <= t) ++k;                    // curves are sorted: the index only moves forward
            float v;
            if (t <= pts[k].t || k + 1 >= pts.size()) v = pts[k].v;                 // before the first point, or after the last
            else v = pts[k].v + (pts[k + 1].v - pts[k].v) * (t - pts[k].t) / std::max(pts[k + 1].t - pts[k].t, 1e-6f);
            if (std::abs(v - p.last[size_t(d)]) > 1e-4f) {
                p.last[size_t(d)] = v;
                if (tracks_[p.track].inst) tracks_[p.track].inst->noteExpression(p.noteId, d, v);
            }
        }
        ++i;
    }
}

void Graph::allNotesOff() noexcept {
    nPlayers_ = 0;
    while (nActive_ > 0) {
        const Active a = active_[--nActive_];
        if (a.track < tracks_.size() && tracks_[a.track].inst) tracks_[a.track].inst->noteOff(a.noteId);
    }
    nGates_ = 0;
}

}  // namespace ddaw::engine
