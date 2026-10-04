#pragma once
// Coordinate maths shared by the arrangement view and the piano roll: ticks <-> pixels, snapping, and the
// bar/beat label the transport shows. Pure functions, unit tested.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace ddaw::app {

struct TimeScale {
    double pxPerBeat = 40.0;   // horizontal zoom (a beat is 96 ticks)
    double originTick = 0.0;   // tick at x = 0 (horizontal scroll)
    static constexpr double kTicksPerBeat = 96.0;

    double toX(double tick) const { return (tick - originTick) / kTicksPerBeat * pxPerBeat; }
    double toTick(double x) const { return x / pxPerBeat * kTicksPerBeat + originTick; }
    double ticksPerPx() const { return kTicksPerBeat / pxPerBeat; }
    // Zoom about an anchor x so the tick under it stays put.
    void zoomAt(double x, double factor, double minPx = 4.0, double maxPx = 600.0) {
        const double tick = toTick(x);
        pxPerBeat = std::clamp(pxPerBeat * factor, minPx, maxPx);
        originTick = tick - x / pxPerBeat * kTicksPerBeat;
        if (originTick < 0) originTick = 0;
    }
};

// A sensible snap grid (ticks) for a zoom level: the finest subdivision that is still >= `minPx` wide.
inline double autoGrid(const TimeScale& s, double minPx = 14.0) {
    static const double grids[] = {6, 12, 24, 48, 96, 192, 384, 768, 1536};  // 1/64 .. 4 bars
    for (double g : grids)
        if (g / TimeScale::kTicksPerBeat * s.pxPerBeat >= minPx) return g;
    return grids[std::size(grids) - 1];
}

// "bar.beat.sixteenth" for a tick position (1-based, as every DAW shows it).
inline std::string barBeatLabel(double ticks, int beatsPerBar = 4) {
    if (ticks < 0) ticks = 0;
    const long t = static_cast<long>(ticks);
    const long beat = t / 96, bar = beat / beatsPerBar;
    const long sixteenth = (t % 96) / 24;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%ld.%ld.%ld", bar + 1, beat % beatsPerBar + 1, sixteenth + 1);
    return buf;
}

inline std::string noteName(int pitch) {
    static const char* n[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(n[((pitch % 12) + 12) % 12]) + std::to_string(pitch / 12 - 1);
}
inline bool isBlackKey(int pitch) {
    const int p = ((pitch % 12) + 12) % 12;
    return p == 1 || p == 3 || p == 6 || p == 8 || p == 10;
}

}  // namespace ddaw::app
