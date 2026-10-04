#pragma once
// Block-rate modulation and automation: port of sf-engine modulation.rs (itself the engine-tone.ts
// modulation loop, evaluated once per rendered chunk instead of per animation frame).
//
// Sources (resolved to numeric addresses at graph build; live edits arrive as a graph swap):
//   - session mode: the launched clip's envelopes (keys "dest|fxId|pkey", values normalised 0..1,
//     looping over the clip length)
//   - arrangement mode: track automation lanes (absolute ticks), then envelopes of arrangement
//     clips under the playhead override same-key lanes; plus master automation
//   - per-track LFOs (targets plus the legacy single dest): synced = tick-division phase, free = Hz
//     phase integrated per block
//   - macros: one 0..1 value fanning out linearly to its sub-targets
//
// Combination (applyModulation): automation gives the BASE through valueFromSpec (log curve for
// exponential specs); each LFO adds a bipolar offset raw*depth*(max-min)/2; the sum clamps to
// [min, max]. When a mapping deactivates (clip stopped, playhead left the clip, transport stopped)
// the parameter snaps back to its stored base value.
//
// Not ported yet: dest "midi" (modulating a MIDI-fx parameter, which regenerates patterns on a
// control thread). The builder reports it as unsupported.
#include <cstdint>
#include <vector>

#include "core/Cmd.h"
#include "core/Performance.h"
#include "engine/Scheduler.h"

namespace ddaw::engine {

class Graph;

// schema.ts LFO_DIV_TICKS: synced-LFO cycle length in ticks per rate index (8 bar ... 1/16 at PPQ 96).
constexpr double kLfoDivTicks[9] = {3072, 1536, 768, 384, 192, 96, 48, 32, 24};
double lfoDivTicks(double rate);  // out-of-range indices fall back to one bar

struct ModPt { double t = 0; float v = 0; };

// engine-tone.ts envValueAt: linear interpolation between breakpoints, clamped outside their range.
float envValueAt(const std::vector<ModPt>& pts, double pos);

class ModState {
public:
    enum class Kind { Param, Macro, LfoRate };

    struct Route {
        Kind kind = Kind::Param;
        ParamAddr addr{};
        uint32_t subStart = 0, subLen = 0;  // Macro
        uint16_t lfoTrack = 0, lfoIdx = 0;  // LfoRate
        float min = 0, max = 1, base = 0;
        bool log = false;
    };
    struct MacroSub { ParamAddr addr; float min, max; };
    struct EnvLane { uint32_t route; uint16_t scene; double loopLen; std::vector<ModPt> pts; };
    struct ArrLane { uint32_t route; double start, len, loopLen; std::vector<ModPt> pts; };
    struct AutoLane { uint32_t route; std::vector<ModPt> pts; };
    struct LfoState {
        bool on = true, sync = false, rated = false;
        uint32_t shape = 0;
        double divTicks = 384, hz = 1, phase = 0, freePhase = 0, effHz = 1;
        float depth = 0.5f;
        std::vector<uint32_t> targets;  // route indices
    };
    struct MacroState { uint32_t route; uint16_t idx; float value; };      // a macro's own knob, applied every block
    struct PerfLane { uint32_t route; PerfSource source; double lo, hi; };   // a performance route (B2)
    struct TrackMod {
        std::vector<EnvLane> env;
        std::vector<ArrLane> arrEnv;
        std::vector<AutoLane> autoLanes;
        std::vector<LfoState> lfos;
        std::vector<PerfLane> perfs;
        std::vector<MacroState> macros;
    };

    // ---- build side ----
    std::vector<Route> routes;
    std::vector<MacroSub> subs;
    std::vector<TrackMod> tracks;
    std::vector<AutoLane> masterAuto;
    double sampleRate = 44100;
    void finalize();  // sizes the per-block scratch

    bool empty() const noexcept { return routes.empty(); }

    // Live edits of a macro knob / an LFO field (audio thread, from Graph::setParam).
    void setMacro(size_t track, uint16_t idx, float v) noexcept;
    void setLfo(size_t track, size_t idx, uint16_t field, float v) noexcept;

    // ---- audio side (real-time safe) ----
    // Evaluate every source at `now` and push the combined values through Graph::setParam.
    // `perf` is the newest tracked frame while the tracker is live, else null (the routes then deactivate).
    void apply(Graph& g, double now, bool playing, TransportMode mode, int frames, const PerformanceFrame* perf = nullptr) noexcept;

private:
    void applyRoute(Graph& g, const Route& rt, float v) noexcept;
    std::vector<float> autoNorm_, lfoOff_;
    std::vector<uint8_t> autoSet_, active_, prevActive_, perfHas_;
    std::vector<float> perfLast_;
};

}  // namespace ddaw::engine
