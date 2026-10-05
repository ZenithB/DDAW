#include "engine/Modulation.h"

#include <algorithm>
#include <cmath>

#include "dsp/Lfo.h"
#include "engine/Graph.h"

namespace ddaw::engine {

double lfoDivTicks(double rate) {
    const auto idx = static_cast<long long>(std::trunc(rate));
    return (idx >= 0 && idx < 9) ? kLfoDivTicks[idx] : 384.0;
}

float envValueAt(const std::vector<ModPt>& pts, double pos) {
    if (pts.size() == 1 || pos <= pts.front().t) return pts.front().v;
    const ModPt last = pts.back();
    if (pos >= last.t) return last.v;
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const ModPt a = pts[i], b = pts[i + 1];
        if (pos >= a.t && pos <= b.t) {
            const double f = (pos - a.t) / std::max(b.t - a.t, 1.0);
            return a.v + (b.v - a.v) * static_cast<float>(f);
        }
    }
    return last.v;
}

namespace {

// schema.ts valueFromSpec over the route's captured curve fields.
float valueFrom(const ModState::Route& rt, float n) {
    const float c = std::clamp(n, 0.0f, 1.0f);
    if (rt.log) return std::exp(std::log(rt.min) + c * (std::log(rt.max) - std::log(rt.min)));
    return rt.min + c * (rt.max - rt.min);
}

// Automation base + LFO offset, clamped to the spec range. `auto` is the normalised lane value when a
// lane is active this block; otherwise the stored base is the anchor.
float combinedValue(const ModState::Route& rt, const float* autoNorm, float lfoOff) {
    const float base = autoNorm ? valueFrom(rt, *autoNorm) : rt.base;
    const float off = lfoOff * (rt.max - rt.min) * 0.5f;
    return std::clamp(base + off, rt.min, rt.max);
}

}  // namespace

void ModState::finalize() {
    const size_t n = routes.size();
    autoNorm_.assign(n, 0.0f);
    lfoOff_.assign(n, 0.0f);
    autoSet_.assign(n, 0);
    active_.assign(n, 0);
    prevActive_.assign(n, 0);
    perfHas_.assign(n, 0);
    perfLast_.assign(n, 0.0f);
}

void ModState::setMacro(size_t track, uint16_t idx, float v) noexcept {
    if (track >= tracks.size()) return;
    for (auto& m : tracks[track].macros) if (m.idx == idx) m.value = std::clamp(v, 0.0f, 1.0f);
}

void ModState::setMorph(size_t track, size_t idx, uint16_t field, float v) noexcept {
    for (auto& m : morphs)
        if (m.track == track && m.idx == idx) {
            const float c = std::clamp(v, 0.0f, 1.0f);
            if (field == kMorphX) m.x = c; else if (field == kMorphY) m.y = c;
        }
}

void ModState::setLfo(size_t track, size_t idx, uint16_t field, float v) noexcept {
    if (track >= tracks.size() || idx >= tracks[track].lfos.size()) return;
    auto& l = tracks[track].lfos[idx];
    switch (field) {
        case kLfoDepth: l.depth = std::clamp(v, 0.0f, 1.0f); break;
        case kLfoHz: l.hz = std::clamp(double(v), 0.01, 30.0); if (!l.rated) l.effHz = l.hz; break;
        case kLfoPhase: l.phase = double(v); break;
        default: break;
    }
}

void ModState::applyRoute(Graph& g, const Route& rt, float v) noexcept {
    switch (rt.kind) {
        case Kind::Param: g.setParam(rt.addr, v); break;
        case Kind::Macro:
            for (uint32_t i = 0; i < rt.subLen; ++i) {
                const MacroSub& sub = subs[rt.subStart + i];
                g.setParam(sub.addr, std::clamp(sub.min + (sub.max - sub.min) * v, sub.min, sub.max));
            }
            break;
        case Kind::LfoRate: break;  // handled in the combine pass: it mutates ModState
    }
}

void ModState::apply(Graph& g, double now, bool playing, TransportMode mode, int frames, const PerformanceFrame* perf) noexcept {
    // morph maps first: the parameters they own are written before automation and LFOs, which win on a shared parameter
    if (!morphs.empty()) {
        const float a = 1.0f - std::exp(-float(std::min(double(frames) / sampleRate, 0.1)) / 0.025f);   // 25 ms stick smoothing
        for (auto& m : morphs) {
            m.sx += (m.x - m.sx) * a;
            m.sy += (m.y - m.sy) * a;
            const size_t nt = m.targets.size(), na = m.ax.size();
            if (nt == 0 || na == 0) continue;
            dsp::morphWeights(m.method, m.power, m.width, m.ax.data(), m.ay.data(), int(na), m.sx, m.sy, m.w.data());
            for (size_t t = 0; t < nt; ++t) {
                float u = 0.0f;
                for (size_t i = 0; i < na; ++i) u += m.w[i] * m.au[i * nt + t];
                const MorphTarget& tg = m.targets[t];
                u = dsp::morphShape(u, tg.curve);
                const float v = tg.log ? std::exp(std::log(tg.min) + u * (std::log(tg.max) - std::log(tg.min))) : tg.min + u * (tg.max - tg.min);
                g.setParam(tg.addr, std::clamp(v, tg.min, tg.max));
            }
        }
    }
    if (routes.empty()) return;
    std::fill(autoSet_.begin(), autoSet_.end(), 0);
    std::fill(lfoOff_.begin(), lfoOff_.end(), 0.0f);
    std::fill(active_.begin(), active_.end(), 0);
    const bool inSession = playing && mode == TransportMode::Session;
    const bool inArr = playing && mode == TransportMode::Arrangement;
    const double dt = std::min(double(frames) / sampleRate, 0.1);

    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        TrackMod& tm = tracks[ti];
        for (const auto& mc : tm.macros) {   // a macro knob is always on; automation of the macro (below) overrides it
            autoNorm_[mc.route] = mc.value;
            autoSet_[mc.route] = 1;
            active_[mc.route] = 1;
        }
        if (inSession) {
            // the launched clip's looping envelopes
            const TrackSched& sched = g.sched(static_cast<int>(ti));
            if (const auto scene = sched.launchedScene()) {
                const double anchor = sched.anchor();
                for (const auto& lane : tm.env) {
                    if (lane.scene != *scene) continue;
                    const double len = std::max(lane.loopLen, 1.0);
                    double pos = std::fmod(now - anchor, len);
                    if (pos < 0) pos += len;  // rem_euclid
                    autoNorm_[lane.route] = envValueAt(lane.pts, pos);
                    autoSet_[lane.route] = 1;
                    active_[lane.route] = 1;
                }
            }
        } else if (inArr) {
            for (const auto& lane : tm.autoLanes) {  // timeline lanes first...
                autoNorm_[lane.route] = envValueAt(lane.pts, now);
                autoSet_[lane.route] = 1;
                active_[lane.route] = 1;
            }
            for (const auto& lane : tm.arrEnv) {      // ...then clip envelopes under the playhead override them
                if (now < lane.start || now >= lane.start + lane.len) continue;
                double pos = std::fmod(now - lane.start, lane.loopLen);
                if (pos < 0) pos += lane.loopLen;
                autoNorm_[lane.route] = envValueAt(lane.pts, pos);
                autoSet_[lane.route] = 1;
                active_[lane.route] = 1;
            }
        }

        // performance routes: the live input's pitch / loudness / envelope as a 0..1 control, whatever the
        // transport is doing; an unvoiced frame holds the last pitch value
        if (perf)
            for (const auto& pl : tm.perfs) {
                float u;
                if (perfNormalize(pl.source, *perf, pl.lo, pl.hi, u)) { perfLast_[pl.route] = u; perfHas_[pl.route] = 1; }
                if (!perfHas_[pl.route]) continue;
                autoNorm_[pl.route] = perfLast_[pl.route];
                autoSet_[pl.route] = 1;
                active_[pl.route] = 1;
            }
        else
            for (const auto& pl : tm.perfs) perfHas_[pl.route] = 0;

        // LFOs: bipolar offsets on top; the free phase integrates even for off LFOs (as in the browser loop)
        for (auto& lfo : tm.lfos) {
            float raw;
            if (lfo.sync) {
                raw = dsp::lfoShapeValue(lfo.shape, now / lfo.divTicks + lfo.phase);
            } else {
                const double hz = lfo.rated ? lfo.effHz : lfo.hz;  // free-run: integrate the (possibly overridden) rate
                lfo.freePhase += dt * hz;
                if (lfo.freePhase > 1e6) lfo.freePhase -= 1e6;
                raw = dsp::lfoShapeValue(lfo.shape, lfo.freePhase + lfo.phase);
            }
            if (!lfo.on || lfo.targets.empty()) continue;
            for (const uint32_t r : lfo.targets) {
                lfoOff_[r] += raw * lfo.depth;
                active_[r] = 1;
            }
        }
    }

    if (inArr)  // master-bus arrangement automation
        for (const auto& lane : masterAuto) {
            autoNorm_[lane.route] = envValueAt(lane.pts, now);
            autoSet_[lane.route] = 1;
            active_[lane.route] = 1;
        }

    // rate overrides live for exactly one combine pass
    for (auto& tm : tracks) for (auto& lfo : tm.lfos) lfo.rated = false;

    // combine + apply; deactivated mappings snap back to their base
    for (size_t ri = 0; ri < routes.size(); ++ri) {
        const bool isActive = active_[ri] != 0, wasActive = prevActive_[ri] != 0;
        prevActive_[ri] = active_[ri];
        const Route& rt = routes[ri];
        const float* an = autoSet_[ri] ? &autoNorm_[ri] : nullptr;
        if (rt.kind == Kind::LfoRate) {
            // no refresh -> `rated` stays false -> the stored Hz next block
            if (isActive) {
                auto& l = tracks[rt.lfoTrack].lfos[rt.lfoIdx];
                l.effHz = double(combinedValue(rt, an, lfoOff_[ri]));
                l.rated = true;
            }
        } else if (isActive) {
            applyRoute(g, rt, combinedValue(rt, an, lfoOff_[ri]));
        } else if (wasActive) {
            applyRoute(g, rt, rt.base);
        }
    }
}

}  // namespace ddaw::engine
