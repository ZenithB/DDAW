#pragma once
// Operations on a set of notes, as the MIDI tools of Ableton Live and the Tools menu of FL Studio's piano roll offer them: quantize,
// legato, chop, flip, transpose, stretch, humanize, velocity scaling, and the chord shapes the stamp tool places. Pure functions over
// copies of notes: the piano roll applies them to the selection (or to every note when nothing is selected) and commits the result as
// one undoable group. Times are ticks (96 per beat).
#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "project/Project.h"

namespace ddaw::app::noteops {

using project::Note;
constexpr double kMinDur = 6.0;       // the shortest note the editor makes (1/64 note)

inline double snapTo(double t, double grid) { return grid > 0.0 ? std::round(t / grid) * grid : t; }

enum class QuantizeMode { Starts, StartsAndEnds };
// Moves each note towards the grid by `strength` (1: onto it, 0.5: half way). Starts keep the note's length; StartsAndEnds also pulls the
// end onto the grid (a note never gets shorter than a grid step).
inline std::vector<Note> quantize(std::vector<Note> notes, double grid, double strength = 1.0, QuantizeMode mode = QuantizeMode::Starts) {
    if (grid <= 0.0) return notes;
    strength = std::clamp(strength, 0.0, 1.0);
    for (auto& n : notes) {
        const double s0 = n.startTicks, e0 = n.startTicks + n.durTicks;
        const double s1 = std::max(0.0, s0 + (snapTo(s0, grid) - s0) * strength);
        double e1 = s1 + n.durTicks;
        if (mode == QuantizeMode::StartsAndEnds) {
            e1 = e0 + (snapTo(e0, grid) - e0) * strength;
            e1 = std::max(e1, s1 + std::min(grid, n.durTicks));
        }
        n.startTicks = s1;
        n.durTicks = std::max(kMinDur, e1 - s1);
    }
    return notes;
}

// Extends each target note to the start of the next note (of any pitch) among `all`; the last one keeps its length. Notes that start
// together do not shorten each other.
inline std::vector<Note> legato(std::vector<Note> targets, const std::vector<Note>& all) {
    for (auto& n : targets) {
        double next = -1.0;
        for (const auto& o : all) if (o.startTicks > n.startTicks + 1e-6 && (next < 0.0 || o.startTicks < next)) next = o.startTicks;
        if (next > 0.0) n.durTicks = std::max(kMinDur, next - n.startTicks);
    }
    return targets;
}

struct Chopped { std::vector<Note> edited, added; };
// Splits each note into `parts` equal pieces: the first is the edited original, the rest are new notes (uid 0).
inline Chopped chop(const std::vector<Note>& notes, int parts) {
    Chopped r;
    parts = std::clamp(parts, 1, 64);
    for (const auto& n : notes) {
        const double piece = n.durTicks / parts;
        if (parts == 1 || piece < kMinDur) { r.edited.push_back(n); continue; }
        auto first = n; first.durTicks = piece; r.edited.push_back(first);
        for (int i = 1; i < parts; ++i) {
            auto p = n; p.uid = 0; p.startTicks = n.startTicks + piece * i; p.durTicks = piece;
            p.bend.clear(); p.slide.clear(); p.pressure.clear();      // the curves belong to the whole note
            r.added.push_back(std::move(p));
        }
    }
    return r;
}

// Cuts every note that spans `tick` in two, when its pitch is in [lo, hi]: the original ends at `tick`, a new note carries on.
inline Chopped slice(const std::vector<Note>& notes, double tick, int lo, int hi) {
    Chopped r;
    for (const auto& n : notes) {
        if (n.pitch < lo || n.pitch > hi) continue;
        if (!(n.startTicks + kMinDur <= tick && tick <= n.startTicks + n.durTicks - kMinDur)) continue;
        auto a = n; a.durTicks = tick - n.startTicks;
        auto b = n; b.uid = 0; b.startTicks = tick; b.durTicks = n.startTicks + n.durTicks - tick;
        b.bend.clear(); b.slide.clear(); b.pressure.clear();
        r.edited.push_back(std::move(a)); r.added.push_back(std::move(b));
    }
    return r;
}

// Mirrors the notes in time within the span they cover (Ableton's Reverse, FL's Flip).
inline std::vector<Note> flipTime(std::vector<Note> notes) {
    if (notes.empty()) return notes;
    double lo = 1e18, hi = 0;
    for (const auto& n : notes) { lo = std::min(lo, n.startTicks); hi = std::max(hi, n.startTicks + n.durTicks); }
    for (auto& n : notes) n.startTicks = lo + (hi - (n.startTicks + n.durTicks));
    return notes;
}
// Mirrors the pitches around the middle of the range they cover.
inline std::vector<Note> flipPitch(std::vector<Note> notes) {
    if (notes.empty()) return notes;
    int lo = 127, hi = 0;
    for (const auto& n : notes) { lo = std::min(lo, n.pitch); hi = std::max(hi, n.pitch); }
    for (auto& n : notes) n.pitch = lo + hi - n.pitch;
    return notes;
}
inline std::vector<Note> transpose(std::vector<Note> notes, int semitones, int maxPitch = 127) {
    for (auto& n : notes) n.pitch = std::clamp(n.pitch + semitones, 0, maxPitch);
    return notes;
}
// Scales the timing of the notes around the first one's start (2: twice as slow, 0.5: twice as fast).
inline std::vector<Note> stretch(std::vector<Note> notes, double factor) {
    if (notes.empty() || factor <= 0.0) return notes;
    double first = 1e18;
    for (const auto& n : notes) first = std::min(first, n.startTicks);
    for (auto& n : notes) { n.startTicks = first + (n.startTicks - first) * factor; n.durTicks = std::max(kMinDur, n.durTicks * factor); }
    return notes;
}
// Loosens the timing (+-`timing` ticks) and the velocities (+-`velocity`, 0..1) at random; the same seed gives the same result.
inline std::vector<Note> humanize(std::vector<Note> notes, double timing, double velocity, unsigned seed = 1) {
    std::mt19937 g(seed);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    for (auto& n : notes) {
        n.startTicks = std::max(0.0, n.startTicks + u(g) * timing);
        n.velocity = std::clamp(n.velocity + u(g) * velocity, 0.05, 1.0);
    }
    return notes;
}
inline std::vector<Note> scaleVelocity(std::vector<Note> notes, double factor) {
    for (auto& n : notes) n.velocity = std::clamp(n.velocity * factor, 0.02, 1.0);
    return notes;
}
inline std::vector<Note> setVelocity(std::vector<Note> notes, double v) {
    for (auto& n : notes) n.velocity = std::clamp(v, 0.02, 1.0);
    return notes;
}
// Notes that end past the clip are shortened to fit, and notes that start past its end are dropped (returned as `drop`).
inline void fitToLength(std::vector<Note>& notes, double len, std::vector<Note>& drop) {
    std::vector<Note> keep;
    for (auto n : notes) {
        if (n.startTicks >= len) { drop.push_back(n); continue; }
        n.durTicks = std::max(kMinDur, std::min(n.durTicks, len - n.startTicks));
        keep.push_back(n);
    }
    notes = std::move(keep);
}

// The chord shapes of the stamp tool: semitones above the clicked note.
struct ChordShape { const char* name; std::vector<int> ivs; };
inline const std::vector<ChordShape>& chordShapes() {
    static const std::vector<ChordShape> c = {
        {"Major", {0, 4, 7}}, {"Minor", {0, 3, 7}}, {"Diminished", {0, 3, 6}}, {"Augmented", {0, 4, 8}},
        {"Major 7", {0, 4, 7, 11}}, {"Minor 7", {0, 3, 7, 10}}, {"Dominant 7", {0, 4, 7, 10}}, {"Minor 7 b5", {0, 3, 6, 10}},
        {"Major 9", {0, 4, 7, 11, 14}}, {"Minor 9", {0, 3, 7, 10, 14}}, {"Sus 2", {0, 2, 7}}, {"Sus 4", {0, 5, 7}},
        {"Power (5th)", {0, 7}}, {"Octave", {0, 12}}, {"Major 6", {0, 4, 7, 9}}, {"Add 9", {0, 4, 7, 14}},
    };
    return c;
}

}  // namespace ddaw::app::noteops
