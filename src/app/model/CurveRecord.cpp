#include "app/model/CurveRecord.h"

#include <algorithm>
#include <cmath>

namespace ddaw::app {

using project::AutoPoint;

std::vector<AutoPoint> simplifyLane(const std::vector<AutoPoint>& pts, double eps) {
    if (pts.size() < 3) return pts;
    std::vector<uint8_t> keep(pts.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<size_t, size_t>> stack{{0, pts.size() - 1}};
    while (!stack.empty()) {
        const auto [a, b] = stack.back();
        stack.pop_back();
        if (b <= a + 1) continue;
        double worst = 0;
        size_t at = a;
        const double dt = pts[b].t - pts[a].t;
        for (size_t i = a + 1; i < b; ++i) {
            const double f = dt > 0 ? (pts[i].t - pts[a].t) / dt : 0.0;
            const double chord = pts[a].v + (pts[b].v - pts[a].v) * f;
            const double d = std::abs(pts[i].v - chord);
            if (d > worst) { worst = d; at = i; }
        }
        if (worst > eps) { keep[at] = 1; stack.push_back({a, at}); stack.push_back({at, b}); }
    }
    std::vector<AutoPoint> out;
    for (size_t i = 0; i < pts.size(); ++i) if (keep[i]) out.push_back(pts[i]);
    return out;
}

std::vector<AutoPoint> buildLane(const std::vector<engine::Engine::TimedPerf>& frames, PerfSource src, double lo, double hi, const CurveTiming& t, double eps, size_t maxPoints) {
    if (lo == 0.0 && hi == 0.0) perfDefaultRange(src, lo, hi);
    std::vector<AutoPoint> raw;
    raw.reserve(frames.size());
    double lastTick = -1e18;
    for (const auto& tp : frames) {
        if (tp.inputFrame < t.startInput) continue;
        float u;
        if (!perfNormalize(src, tp.frame, lo, hi, u)) continue;      // unvoiced: hold
        const double tick = t.startTick + (tp.inputFrame - t.startInput - t.compFrames) * t.ticksPerFrame;
        if (tick < 0.0 || tick <= lastTick) continue;
        raw.push_back({tick, double(u)});
        lastTick = tick;
    }
    auto out = simplifyLane(raw, eps);
    for (double e = eps; out.size() > maxPoints && e < 0.5; e *= 1.5) out = simplifyLane(raw, e);   // keep lanes a sane size
    return out;
}

std::vector<AutoPoint> mergeLane(const std::vector<AutoPoint>& existing, const std::vector<AutoPoint>& recorded) {
    if (recorded.empty()) return existing;
    const double a = recorded.front().t, b = recorded.back().t;
    std::vector<AutoPoint> out;
    for (const auto& p : existing) if (p.t < a) out.push_back(p);
    out.insert(out.end(), recorded.begin(), recorded.end());
    for (const auto& p : existing) if (p.t > b) out.push_back(p);
    return out;
}

}  // namespace ddaw::app
