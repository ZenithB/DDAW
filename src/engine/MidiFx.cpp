// MIDI effects: port of sf-engine midifx.rs (itself an exact port of synthyy's src/audio/midifx.ts
// applyMidiFx/arpExpand/arpSequence plus the theory helpers snapToScale/getScale).
//
// Pure note-list transforms run on the builder thread (allocation is fine here). All rolls come from
// a XorShift seeded from FNV-1a over the track id, so repeated renders are bit-identical.
#include "engine/MidiFx.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ddaw::engine {
namespace {

// Arp step length in ticks per `rate` index (1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32 at PPQ 96).
constexpr std::array<double, 6> kArpDivTicks{96.0, 48.0, 32.0, 24.0, 16.0, 12.0};

// Working note, mirrors the Rust `Note { p, s, d, v, pr }` (pitch is a double there).
struct MNote {
    double p = 60, s = 0, d = 0, v = 1, pr = 1;
};

struct ScaleDef {
    const char* id;
    std::span<const int> ivs;
};

constexpr int kMajor[] = {0, 2, 4, 5, 7, 9, 11};
constexpr int kMinor[] = {0, 2, 3, 5, 7, 8, 10};
constexpr int kDorian[] = {0, 2, 3, 5, 7, 9, 10};
constexpr int kMixo[] = {0, 2, 4, 5, 7, 9, 10};
constexpr int kPentMaj[] = {0, 2, 4, 7, 9};
constexpr int kPentMin[] = {0, 3, 5, 7, 10};
constexpr int kHarmMin[] = {0, 2, 3, 5, 7, 8, 11};
constexpr int kBlues[] = {0, 3, 5, 6, 7, 10};

// theory.ts SCALES (unknown ids fall back to major).
const std::array<ScaleDef, 8> kScales{{
    {"major", kMajor},
    {"minor", kMinor},
    {"dorian", kDorian},
    {"mixo", kMixo},
    {"pentMaj", kPentMaj},
    {"pentMin", kPentMin},
    {"harmMin", kHarmMin},
    {"blues", kBlues},
}};

std::span<const int> scaleIvs(const std::string& id) {
    for (const auto& s : kScales)
        if (id == s.id) return s.ivs;
    return kScales[0].ivs;
}

// Rust f64::rem_euclid.
double remEuclid(double a, double b) {
    const double r = std::fmod(a, b);
    return r < 0.0 ? r + std::fabs(b) : r;
}

bool inScale(double pitch, double root, std::span<const int> ivs) {
    const double rel = remEuclid(pitch - root, 12.0);
    return std::any_of(ivs.begin(), ivs.end(), [rel](int iv) { return rel == static_cast<double>(iv); });
}

// theory.ts snapToScale: nearest scale tone, ties resolve downward.
double snapToScale(double pitch, double root, std::span<const int> ivs) {
    if (inScale(pitch, root, ivs)) return pitch;
    for (int off = 1; off <= 6; ++off) {
        if (inScale(pitch - off, root, ivs)) return pitch - off;
        if (inScale(pitch + off, root, ivs)) return pitch + off;
    }
    return pitch;
}

// JS `x | 0` as used by the Rust port: truncate toward zero (saturating, NaN -> 0).
int64_t jsInt(double v) {
    if (std::isnan(v)) return 0;
    const double t = std::trunc(v);
    constexpr double lim = 9.2e18;  // just inside the int64 range
    if (t >= lim) return std::numeric_limits<int64_t>::max();
    if (t <= -lim) return std::numeric_limits<int64_t>::min();
    return static_cast<int64_t>(t);
}

double param(const project::DeviceSpec& fx, const char* key, double def) {
    const auto it = fx.params.find(key);
    return it == fx.params.end() ? def : it->second;
}

// Rust v.max(lo).min(hi) (f64::max/min ignore a NaN operand, as fmax/fmin do).
double clampf(double v, double lo, double hi) { return std::fmin(std::fmax(v, lo), hi); }

// midifx.ts arpSequence: the ascending pitch set the arp cycles through before direction is applied
// (pat 0 Octave, 1 5ths, 2 Scale walk, 3 Select bitmask).
std::vector<double> arpSequence(const std::vector<double>& held, int64_t pat, double oct, int64_t sel,
                                double root, std::span<const int> ivs) {
    if (pat == 2) {
        const double s0 = snapToScale(held[0], root, ivs);
        const double rel0 = remEuclid(s0 - root, 12.0);
        size_t deg = 0;
        for (size_t i = 0; i < ivs.size(); ++i)
            if (static_cast<double>(ivs[i]) == rel0) {
                deg = i;
                break;
            }
        const double baseRoot = s0 - rel0;  // the scale root in s0's octave
        const double count = static_cast<double>(ivs.size()) * oct;
        std::vector<double> seq;
        for (size_t k = 0; static_cast<double>(k) < count; ++k) {
            const size_t idx = deg + k;
            seq.push_back(baseRoot + ivs[idx % ivs.size()] + 12.0 * static_cast<double>(idx / ivs.size()));
        }
        return seq;
    }

    std::vector<double> offsets;
    if (pat == 1) {
        offsets = {0.0, 7.0};
    } else if (pat == 3) {
        for (int i = 0; i < 12; ++i)
            if ((sel & (int64_t{1} << i)) != 0) offsets.push_back(i);
    } else {
        offsets = {0.0};
    }
    if (offsets.empty()) offsets.push_back(0.0);  // Select with nothing on: the note itself

    std::vector<double> one;
    one.reserve(held.size() * offsets.size());
    for (double h : held)
        for (double o : offsets) one.push_back(h + o);

    std::vector<double> seq;
    for (size_t o = 0; static_cast<double>(o) < oct; ++o)
        for (double pp : one) seq.push_back(pp + static_cast<double>(o) * 12.0);

    // JS `[...new Set(seq)].sort(asc)`: dedup exact values, ascending.
    std::sort(seq.begin(), seq.end());
    seq.erase(std::unique(seq.begin(), seq.end()), seq.end());
    return seq;
}

// midifx.ts arpExpand. Notes sharing a start tick form one "held" group; each group is replaced by a
// stepped run of the arp sequence.
std::vector<MNote> arpExpand(std::vector<MNote> notes, const project::DeviceSpec& fx, double root,
                             std::span<const int> ivs, XorShift& rng) {
    if (notes.empty()) return notes;
    const int64_t rate = std::clamp<int64_t>(jsInt(param(fx, "rate", 0.0)), 0,
                                             static_cast<int64_t>(kArpDivTicks.size()) - 1);
    const double step = kArpDivTicks[static_cast<size_t>(rate)];
    const int64_t mode = jsInt(param(fx, "mode", 0.0));  // 0 up, 1 down, 2 up-down, 3 random
    const int64_t pat = jsInt(param(fx, "pat", 0.0));
    const double oct = std::fmax(param(fx, "oct", 1.0), 1.0);
    const double gate = param(fx, "gate", 0.8);
    const int64_t sel = jsInt(param(fx, "sel", 1.0));

    // Group by exact start tick, first-seen order (JS Map insertion order).
    std::vector<std::pair<double, std::vector<MNote>>> groups;
    for (const MNote& n : notes) {
        auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == n.s; });
        if (it == groups.end())
            groups.push_back({n.s, {n}});
        else
            it->second.push_back(n);
    }

    std::vector<MNote> out;
    for (const auto& grp : groups) {
        const auto& g = grp.second;
        double end = -std::numeric_limits<double>::infinity();
        for (const MNote& n : g) end = std::fmax(end, n.s + n.d);
        std::vector<double> held;
        held.reserve(g.size());
        for (const MNote& n : g) held.push_back(n.p);
        std::sort(held.begin(), held.end());
        held.erase(std::unique(held.begin(), held.end()), held.end());

        std::vector<double> seq = arpSequence(held, pat, oct, sel, root, ivs);
        if (mode == 1) {
            std::reverse(seq.begin(), seq.end());
        } else if (mode == 2 && seq.size() > 2) {
            std::vector<double> tail(seq.begin() + 1, seq.end() - 1);
            std::reverse(tail.begin(), tail.end());
            seq.insert(seq.end(), tail.begin(), tail.end());
        }

        size_t i = 0;
        double t = g[0].s;
        while (t < end) {
            double pitch;
            if (mode == 3)
                pitch = seq[static_cast<size_t>(rng.nextF64() * static_cast<double>(seq.size())) % seq.size()];
            else
                pitch = seq[i % seq.size()];
            MNote o;
            o.p = clampf(pitch, 0.0, 127.0);
            o.s = t;
            o.d = std::fmax(step * gate, 6.0);
            o.v = g[0].v;
            o.pr = g[0].pr;
            out.push_back(o);
            ++i;
            t += step;
        }
    }
    return out;
}

// midifx.ts applyMidiFx: run a note list through a MIDI-fx chain. Disabled devices are skipped;
// unknown types pass through; scale/chord/arp don't touch drum tracks (velo/rand do).
// (The Rust port also threads a loop length through to arp_expand, which never reads it; omitted.)
std::vector<MNote> applyMidiFx(const std::vector<project::DeviceSpec>& chain, std::vector<MNote> notes,
                               double root, const std::string& scale, bool isDrum, XorShift& rng) {
    if (chain.empty()) return notes;
    const auto ivs = scaleIvs(scale);
    std::vector<MNote> out = std::move(notes);
    for (const auto& d : chain) {
        if (!d.on) continue;
        if (d.type == "scale" && !isDrum) {
            for (MNote& n : out) n.p = snapToScale(n.p, root, ivs);
        } else if (d.type == "chord" && !isDrum) {
            // [0, i1, i2, i3] keeping the root and dropping zero intervals.
            std::vector<double> ivList{0.0};
            for (const char* k : {"i1", "i2", "i3"}) {
                const double v = param(d, k, 0.0);
                if (v != 0.0) ivList.push_back(v);
            }
            std::vector<MNote> next;
            next.reserve(out.size() * ivList.size());
            for (const MNote& n : out)
                for (double iv : ivList) {
                    MNote c = n;
                    c.p = clampf(n.p + iv, 0.0, 127.0);
                    c.v = iv == 0.0 ? n.v : n.v * 0.85;
                    next.push_back(c);
                }
            out = std::move(next);
        } else if (d.type == "velo") {
            const double s = param(d, "scale", 1.0);
            const double r = param(d, "rand", 0.0);
            for (MNote& n : out) n.v = clampf(n.v * s + (rng.nextF64() * 2.0 - 1.0) * r, 0.05, 1.0);
        } else if (d.type == "rand") {
            const double ch = param(d, "chance", 1.0);
            const double oc = param(d, "octave", 0.0);
            std::vector<MNote> next;
            next.reserve(out.size());
            for (const MNote& n : out) {
                if (ch < 1.0 && rng.nextF64() > ch) continue;
                MNote c = n;
                if (oc > 0.0 && rng.nextF64() < oc)
                    c.p = clampf(c.p + (rng.nextF64() < 0.5 ? 12.0 : -12.0), 0.0, 127.0);
                next.push_back(c);
            }
            out = std::move(next);
        } else if (d.type == "arp" && !isDrum) {
            out = arpExpand(std::move(out), d, root, ivs, rng);
        }
        // else: unknown type / drum-skipped device: pass through
    }
    return out;
}

// FNV-1a over the track identity: a stable per-track RNG seed so the velo/rand rolls decorrelate
// between tracks yet repeat across renders.
uint64_t trackSeed(const std::string& id) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const char c : id) {
        h ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
        h *= 0x00000100000001b3ull;
    }
    return h;
}

}  // namespace

std::vector<NoteEv> expandMidi(const std::vector<project::DeviceSpec>& chain,
                               const std::vector<project::Note>& notes, const MidiFxContext& ctx) {
    XorShift rng(trackSeed(ctx.trackId));
    std::vector<MNote> in;
    in.reserve(notes.size());
    for (const auto& n : notes) {
        MNote m;
        m.p = n.pitch;
        m.s = n.startTicks;
        m.d = n.durTicks;
        m.v = n.velocity;
        m.pr = n.probability;
        in.push_back(m);
    }
    const std::vector<MNote> expanded = applyMidiFx(chain, std::move(in), ctx.root, ctx.scale, ctx.isDrum, rng);

    std::vector<NoteEv> evs;
    evs.reserve(expanded.size());
    for (const MNote& n : expanded) {
        NoteEv e;
        e.tick = n.s;
        // Rust `n.p.clamp(0, 127) as u8`: clamp then truncate (NaN -> 0).
        e.pitch = std::isnan(n.p) ? uint8_t{0} : static_cast<uint8_t>(clampf(n.p, 0.0, 127.0));
        e.durTicks = std::fmax(n.d, 0.0);
        e.vel = static_cast<float>(n.v);
        e.pr = static_cast<float>(n.pr);
        evs.push_back(e);
    }
    std::stable_sort(evs.begin(), evs.end(), [](const NoteEv& a, const NoteEv& b) { return a.tick < b.tick; });
    return evs;
}

}  // namespace ddaw::engine
