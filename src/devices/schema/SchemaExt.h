#pragma once
// Parameters added to ported devices after the synthyy port. Hand-written - Schema.generated.h is generated from synthyy's
// registry and must not be edited; a table here starts with the generated entries unchanged (the same keys, ranges and
// defaults, so projects and fixtures stay valid) and appends. Header-only so the UI catalog can list them.
#include "core/Device.h"

namespace ddaw::schema {

// `autotune`: the generated four, plus a key of its own. `root` 0..11 (C..B) and `scale` (dsp/Scales.h index) override the
// project's key for this device; -1 (the default) follows the project.
inline constexpr ParamSpec kFxAutotuneKey[] = {
    {0, "amount", 0.0f, 1.0f, 1.0f, Curve::Linear, 15.0f, false},
    {1, "speed", 1.0f, 200.0f, 20.0f, Curve::Exponential, 15.0f, false},
    {2, "mix", 0.0f, 1.0f, 1.0f, Curve::Linear, 15.0f, false},
    {3, "mode", 0.0f, 1.0f, 0.0f, Curve::Stepped, 0.0f, false},
    {4, "root", -1.0f, 11.0f, -1.0f, Curve::Stepped, 0.0f, false},
    {5, "scale", -1.0f, 7.0f, -1.0f, Curve::Stepped, 0.0f, false},
};

// `chordres` (Chord Resonator): steep, high-Q band-pass filters tuned to equal-tempered intervals above the note being played
// (the tonic), boosted and compressed into a chord. Intervals are semitones from the tonic, -12..+36; levels in dB.
//   source    0 Auto (the track's notes when it has an instrument playing them, else the audio), 1 Notes, 2 Audio (FFT pitch)
//   filters   how many of i1..i6 are active (1..6);  tonic: an extra resonance on the tonic itself, level `tonicLevel`
//   boost     how hard the resonances are boosted (dB); more boost lowers the original a little and compresses each resonance harder
//   q, steep  bandwidth (Q) and skirt steepness (stepped: 12, 24, 36, 48 dB/oct)
//   excite    light harmonic distortion ahead of the filters, which gives them harmonics to ring on
//   sat       a saturation-and-compression stage at the end of the chain (0 = off, bit-exact)
//   mix       the original against the resonated sound;  glide: how fast the filters follow a new tonic (ms)
//   thresh    audio mode: the level (dB) below which the pitch tracker holds the last tonic
inline constexpr ParamSpec kFxChordres[] = {
    {0, "source", 0.0f, 2.0f, 0.0f, Curve::Stepped, 0.0f, false},
    {1, "filters", 1.0f, 6.0f, 3.0f, Curve::Stepped, 0.0f, false},
    {2, "tonic", 0.0f, 1.0f, 0.0f, Curve::Stepped, 0.0f, false},
    {3, "tonicLevel", -18.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {4, "i1", -12.0f, 36.0f, 4.0f, Curve::Stepped, 0.0f, false},
    {5, "i2", -12.0f, 36.0f, 7.0f, Curve::Stepped, 0.0f, false},
    {6, "i3", -12.0f, 36.0f, 11.0f, Curve::Stepped, 0.0f, false},
    {7, "i4", -12.0f, 36.0f, 12.0f, Curve::Stepped, 0.0f, false},
    {8, "i5", -12.0f, 36.0f, 16.0f, Curve::Stepped, 0.0f, false},
    {9, "i6", -12.0f, 36.0f, 19.0f, Curve::Stepped, 0.0f, false},
    {10, "l1", -18.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {11, "l2", -18.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {12, "l3", -18.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {13, "l4", -18.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {14, "l5", -18.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {15, "l6", -18.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {16, "boost", 0.0f, 36.0f, 12.0f, Curve::Linear, 15.0f, false},
    {17, "q", 2.0f, 500.0f, 40.0f, Curve::Exponential, 15.0f, false},
    {18, "steep", 0.0f, 3.0f, 1.0f, Curve::Stepped, 0.0f, false},
    {19, "excite", 0.0f, 1.0f, 0.3f, Curve::Linear, 15.0f, false},
    {20, "sat", 0.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},
    {21, "mix", 0.0f, 1.0f, 1.0f, Curve::Linear, 15.0f, false},
    {22, "glide", 1.0f, 500.0f, 25.0f, Curve::Exponential, 15.0f, false},
    {23, "thresh", -80.0f, -20.0f, -55.0f, Curve::Linear, 15.0f, false},
};

// ---- new controls on ported effects (ARCH 27): each table is the generated one unchanged, then the additions at their neutral values ----

// `delay`: tone (a low-pass in the feedback path; 20 kHz = off), duck (the echoes drop while you play; 0 = off), wobble (tape drift of the time; 0 = off).
inline constexpr ParamSpec kFxDelayExt[] = {
    {0, "time", 0.0f, 5.0f, 2.0f, Curve::Stepped, 0.0f, false},
    {1, "fb", 0.0f, 0.85f, 0.35f, Curve::Linear, 15.0f, false},
    {2, "mix", 0.0f, 1.0f, 0.35f, Curve::Linear, 15.0f, false},
    {3, "tone", 200.0f, 20000.0f, 20000.0f, Curve::Exponential, 15.0f, false},
    {4, "duck", 0.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},
    {5, "wobble", 0.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},
};
inline constexpr ParamSpec kFxPingpongExt[] = {
    {0, "time", 0.0f, 5.0f, 2.0f, Curve::Stepped, 0.0f, false},
    {1, "fb", 0.0f, 0.85f, 0.4f, Curve::Linear, 15.0f, false},
    {2, "mix", 0.0f, 1.0f, 0.3f, Curve::Linear, 15.0f, false},
    {3, "tone", 200.0f, 20000.0f, 20000.0f, Curve::Exponential, 15.0f, false},
    {4, "duck", 0.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},
    {5, "wobble", 0.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},
};

// `reverb`: predelay (was fixed at 20 ms), tone (a low-pass on the tail; 20 kHz = off), lowcut (a high-pass on the tail; 20 Hz = off), duck.
inline constexpr ParamSpec kFxReverbExt[] = {
    {0, "size", 0.2f, 10.0f, 2.2f, Curve::Exponential, 15.0f, false},
    {1, "mix", 0.0f, 1.0f, 0.3f, Curve::Linear, 15.0f, false},
    {2, "predelay", 0.0f, 0.2f, 0.02f, Curve::Linear, 15.0f, false},
    {3, "tone", 500.0f, 20000.0f, 20000.0f, Curve::Exponential, 15.0f, false},
    {4, "lowcut", 20.0f, 1000.0f, 20.0f, Curve::Exponential, 15.0f, false},
    {5, "duck", 0.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},
};
// `plate`: lowcut and duck, as on the reverb.
inline constexpr ParamSpec kFxPlateExt[] = {
    {0, "decay", 0.3f, 8.0f, 1.8f, Curve::Exponential, 15.0f, false},
    {1, "predelay", 0.0f, 0.2f, 0.02f, Curve::Linear, 15.0f, false},
    {2, "damp", 500.0f, 16000.0f, 6000.0f, Curve::Exponential, 15.0f, false},
    {3, "mix", 0.0f, 1.0f, 0.3f, Curve::Linear, 15.0f, false},
    {4, "lowcut", 20.0f, 1000.0f, 20.0f, Curve::Exponential, 15.0f, false},
    {5, "duck", 0.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},
};


// ---- LFO phase (and shape) on every effect LFO, phase on the instruments' LFOs (ARCH 27): degrees, 0 = the old start phase ----
// `chorus`: phase (degrees) and the LFO's shape (sine, triangle, saw up, saw down, square, sample-and-hold, smooth random).
inline constexpr ParamSpec kFxChorusExt[] = {
    {0, "rate", 0.01f, 300.0f, 1.5f, Curve::Exponential, 15.0f, false},  // rateSwitch: Sync/Low/High rate modes
    {1, "depth", 0.0f, 1.0f, 0.5f, Curve::Linear, 15.0f, false},
    {2, "mix", 0.0f, 1.0f, 0.5f, Curve::Linear, 15.0f, false},
    {3, "rateMode", 0.0f, 2.0f, 1.0f, Curve::Stepped, 0.0f, false},  // 0 Sync, 1 Low (rate), 2 High (rateHi)
    {4, "rateSync", 0.0f, 8.0f, 5.0f, Curve::Stepped, 0.0f, false},  // tempo division index
    {5, "rateHi", 301.0f, 1000.0f, 440.0f, Curve::Stepped, 0.0f, false},  // Hz, High mode
    {6, "phase", 0.0f, 360.0f, 0.0f, Curve::Linear, 15.0f, false},
    {7, "shape", 0.0f, 6.0f, 0.0f, Curve::Stepped, 0.0f, false},
};
// `phaser`: phase (degrees) and the LFO's shape (sine, triangle, saw up, saw down, square, sample-and-hold, smooth random).
inline constexpr ParamSpec kFxPhaserExt[] = {
    {0, "rate", 0.01f, 300.0f, 0.8f, Curve::Exponential, 15.0f, false},  // rateSwitch: Sync/Low/High rate modes
    {1, "octaves", 1.0f, 6.0f, 3.0f, Curve::Stepped, 0.0f, false},
    {2, "mix", 0.0f, 1.0f, 0.5f, Curve::Linear, 15.0f, false},
    {3, "rateMode", 0.0f, 2.0f, 1.0f, Curve::Stepped, 0.0f, false},  // 0 Sync, 1 Low (rate), 2 High (rateHi)
    {4, "rateSync", 0.0f, 8.0f, 5.0f, Curve::Stepped, 0.0f, false},  // tempo division index
    {5, "rateHi", 301.0f, 1000.0f, 440.0f, Curve::Stepped, 0.0f, false},  // Hz, High mode
    {6, "phase", 0.0f, 360.0f, 0.0f, Curve::Linear, 15.0f, false},
    {7, "shape", 0.0f, 6.0f, 0.0f, Curve::Stepped, 0.0f, false},
};
// `vib`: phase (degrees) and the LFO's shape (sine, triangle, saw up, saw down, square, sample-and-hold, smooth random).
inline constexpr ParamSpec kFxVibExt[] = {
    {0, "rate", 0.01f, 300.0f, 5.0f, Curve::Exponential, 15.0f, false},  // rateSwitch: Sync/Low/High rate modes
    {1, "depth", 0.0f, 0.6f, 0.15f, Curve::Linear, 15.0f, false},
    {2, "mix", 0.0f, 1.0f, 1.0f, Curve::Linear, 15.0f, false},
    {3, "rateMode", 0.0f, 2.0f, 1.0f, Curve::Stepped, 0.0f, false},  // 0 Sync, 1 Low (rate), 2 High (rateHi)
    {4, "rateSync", 0.0f, 8.0f, 5.0f, Curve::Stepped, 0.0f, false},  // tempo division index
    {5, "rateHi", 301.0f, 1000.0f, 440.0f, Curve::Stepped, 0.0f, false},  // Hz, High mode
    {6, "phase", 0.0f, 360.0f, 0.0f, Curve::Linear, 15.0f, false},
    {7, "shape", 0.0f, 6.0f, 0.0f, Curve::Stepped, 0.0f, false},
};
// `trem`: phase (degrees) and the LFO's shape (sine, triangle, saw up, saw down, square, sample-and-hold, smooth random).
inline constexpr ParamSpec kFxTremExt[] = {
    {0, "rate", 0.01f, 300.0f, 5.0f, Curve::Exponential, 15.0f, false},  // rateSwitch: Sync/Low/High rate modes
    {1, "depth", 0.0f, 1.0f, 0.6f, Curve::Linear, 15.0f, false},
    {2, "mix", 0.0f, 1.0f, 1.0f, Curve::Linear, 15.0f, false},
    {3, "rateMode", 0.0f, 2.0f, 1.0f, Curve::Stepped, 0.0f, false},  // 0 Sync, 1 Low (rate), 2 High (rateHi)
    {4, "rateSync", 0.0f, 8.0f, 5.0f, Curve::Stepped, 0.0f, false},  // tempo division index
    {5, "rateHi", 301.0f, 1000.0f, 440.0f, Curve::Stepped, 0.0f, false},  // Hz, High mode
    {6, "phase", 0.0f, 360.0f, 0.0f, Curve::Linear, 15.0f, false},
    {7, "shape", 0.0f, 6.0f, 0.0f, Curve::Stepped, 0.0f, false},
};
// `autopan`: phase (degrees) and the LFO's shape (sine, triangle, saw up, saw down, square, sample-and-hold, smooth random).
inline constexpr ParamSpec kFxAutopanExt[] = {
    {0, "rate", 0.01f, 300.0f, 1.5f, Curve::Exponential, 15.0f, false},  // rateSwitch: Sync/Low/High rate modes
    {1, "depth", 0.0f, 1.0f, 0.8f, Curve::Linear, 15.0f, false},
    {2, "rateMode", 0.0f, 2.0f, 1.0f, Curve::Stepped, 0.0f, false},  // 0 Sync, 1 Low (rate), 2 High (rateHi)
    {3, "rateSync", 0.0f, 8.0f, 5.0f, Curve::Stepped, 0.0f, false},  // tempo division index
    {4, "rateHi", 301.0f, 1000.0f, 440.0f, Curve::Stepped, 0.0f, false},  // Hz, High mode
    {5, "phase", 0.0f, 360.0f, 0.0f, Curve::Linear, 15.0f, false},
    {6, "shape", 0.0f, 6.0f, 0.0f, Curve::Stepped, 0.0f, false},
};
// `autofilt`: phase (degrees) and the LFO's shape (sine, triangle, saw up, saw down, square, sample-and-hold, smooth random).
inline constexpr ParamSpec kFxAutofiltExt[] = {
    {0, "rate", 0.01f, 300.0f, 1.0f, Curve::Exponential, 15.0f, false},  // rateSwitch: Sync/Low/High rate modes
    {1, "depth", 0.0f, 1.0f, 0.7f, Curve::Linear, 15.0f, false},
    {2, "base", 80.0f, 4000.0f, 350.0f, Curve::Exponential, 15.0f, false},
    {3, "mix", 0.0f, 1.0f, 1.0f, Curve::Linear, 15.0f, false},
    {4, "rateMode", 0.0f, 2.0f, 1.0f, Curve::Stepped, 0.0f, false},  // 0 Sync, 1 Low (rate), 2 High (rateHi)
    {5, "rateSync", 0.0f, 8.0f, 5.0f, Curve::Stepped, 0.0f, false},  // tempo division index
    {6, "rateHi", 301.0f, 1000.0f, 440.0f, Curve::Stepped, 0.0f, false},  // Hz, High mode
    {7, "phase", 0.0f, 360.0f, 0.0f, Curve::Linear, 15.0f, false},
    {8, "shape", 0.0f, 6.0f, 0.0f, Curve::Stepped, 0.0f, false},
};
// `poly`: the LFO's phase (degrees).
inline constexpr ParamSpec kInstPolyExt[] = {
    {0, "wave", 0.0f, 6.0f, 0.0f, Curve::Stepped, 0.0f, false},
    {1, "spread", 0.0f, 60.0f, 18.0f, Curve::Linear, 15.0f, false},
    {2, "cutoff", 80.0f, 14000.0f, 7000.0f, Curve::Exponential, 15.0f, false},
    {3, "res", 0.0f, 10.0f, 0.7f, Curve::Linear, 15.0f, false},
    {4, "slope", 0.0f, 2.0f, 0.0f, Curve::Stepped, 0.0f, false},
    {5, "attack", 0.001f, 2.0f, 0.01f, Curve::Exponential, 15.0f, false},
    {6, "decay", 0.01f, 2.0f, 0.15f, Curve::Exponential, 15.0f, false},
    {7, "sustain", 0.0f, 1.0f, 0.6f, Curve::Linear, 15.0f, false},
    {8, "release", 0.01f, 4.0f, 0.4f, Curve::Exponential, 15.0f, false},
    {9, "lfoShape", 0.0f, 3.0f, 0.0f, Curve::Stepped, 0.0f, false},
    {10, "lfoRate", 0.05f, 16.0f, 2.0f, Curve::Exponential, 15.0f, false},
    {11, "lfoAmt", 0.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},
    {12, "lfoPhase", 0.0f, 360.0f, 0.0f, Curve::Linear, 15.0f, false},
};
// `mono`: the LFO's phase (degrees).
inline constexpr ParamSpec kInstMonoExt[] = {
    {0, "wave", 0.0f, 3.0f, 0.0f, Curve::Stepped, 0.0f, false},
    {1, "cutoff", 40.0f, 8000.0f, 900.0f, Curve::Exponential, 15.0f, false},
    {2, "res", 0.0f, 10.0f, 2.0f, Curve::Linear, 15.0f, false},
    {3, "slope", 0.0f, 2.0f, 1.0f, Curve::Stepped, 0.0f, false},
    {4, "envAmt", 0.0f, 6.0f, 2.5f, Curve::Linear, 15.0f, false},
    {5, "glide", 0.0f, 0.3f, 0.02f, Curve::Linear, 15.0f, false},
    {6, "attack", 0.001f, 2.0f, 0.003f, Curve::Exponential, 15.0f, false},
    {7, "decay", 0.01f, 2.0f, 0.2f, Curve::Exponential, 15.0f, false},
    {8, "sustain", 0.0f, 1.0f, 0.45f, Curve::Linear, 15.0f, false},
    {9, "release", 0.01f, 4.0f, 0.25f, Curve::Exponential, 15.0f, false},
    {10, "lfoShape", 0.0f, 3.0f, 0.0f, Curve::Stepped, 0.0f, false},
    {11, "lfoRate", 0.05f, 16.0f, 2.0f, Curve::Exponential, 15.0f, false},
    {12, "lfoAmt", 0.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},
    {13, "lfoPhase", 0.0f, 360.0f, 0.0f, Curve::Linear, 15.0f, false},
};


// ---- the synthesized drum machines (ARCH 29): eight pads of tune (semitones), decay (seconds) and level (dB), the decays defaulting to each machine's own ----
inline constexpr ParamSpec kInstDrum808[] = {
    {0, "p0_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {1, "p0_decay", 0.03f, 2.0f, 0.55f, Curve::Exponential, 15.0f, false},
    {2, "p0_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
    {3, "p1_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {4, "p1_decay", 0.03f, 2.0f, 0.22f, Curve::Exponential, 15.0f, false},
    {5, "p1_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
    {6, "p2_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {7, "p2_decay", 0.03f, 2.0f, 0.28f, Curve::Exponential, 15.0f, false},
    {8, "p2_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
    {9, "p3_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {10, "p3_decay", 0.03f, 2.0f, 0.05f, Curve::Exponential, 15.0f, false},
    {11, "p3_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
    {12, "p4_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {13, "p4_decay", 0.03f, 2.0f, 0.4f, Curve::Exponential, 15.0f, false},
    {14, "p4_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
    {15, "p5_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {16, "p5_decay", 0.03f, 2.0f, 0.35f, Curve::Exponential, 15.0f, false},
    {17, "p5_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
    {18, "p6_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {19, "p6_decay", 0.03f, 2.0f, 0.2f, Curve::Exponential, 15.0f, false},
    {20, "p6_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
    {21, "p7_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {22, "p7_decay", 0.03f, 2.0f, 1.2f, Curve::Exponential, 15.0f, false},
    {23, "p7_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
};
inline constexpr ParamSpec kInstDrum909[] = {
    {0, "p0_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {1, "p0_decay", 0.03f, 2.0f, 0.32f, Curve::Exponential, 15.0f, false},
    {2, "p0_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
    {3, "p1_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {4, "p1_decay", 0.03f, 2.0f, 0.2f, Curve::Exponential, 15.0f, false},
    {5, "p1_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
    {6, "p2_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {7, "p2_decay", 0.03f, 2.0f, 0.25f, Curve::Exponential, 15.0f, false},
    {8, "p2_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
    {9, "p3_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {10, "p3_decay", 0.03f, 2.0f, 0.04f, Curve::Exponential, 15.0f, false},
    {11, "p3_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
    {12, "p4_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {13, "p4_decay", 0.03f, 2.0f, 0.35f, Curve::Exponential, 15.0f, false},
    {14, "p4_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
    {15, "p5_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {16, "p5_decay", 0.03f, 2.0f, 0.3f, Curve::Exponential, 15.0f, false},
    {17, "p5_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
    {18, "p6_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {19, "p6_decay", 0.03f, 2.0f, 0.06f, Curve::Exponential, 15.0f, false},
    {20, "p6_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
    {21, "p7_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {22, "p7_decay", 0.03f, 2.0f, 1.4f, Curve::Exponential, 15.0f, false},
    {23, "p7_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
};
inline constexpr ParamSpec kInstDrumLinn[] = {
    {0, "p0_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {1, "p0_decay", 0.03f, 2.0f, 0.25f, Curve::Exponential, 15.0f, false},
    {2, "p0_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
    {3, "p1_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {4, "p1_decay", 0.03f, 2.0f, 0.22f, Curve::Exponential, 15.0f, false},
    {5, "p1_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
    {6, "p2_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {7, "p2_decay", 0.03f, 2.0f, 0.2f, Curve::Exponential, 15.0f, false},
    {8, "p2_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
    {9, "p3_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {10, "p3_decay", 0.03f, 2.0f, 0.05f, Curve::Exponential, 15.0f, false},
    {11, "p3_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
    {12, "p4_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {13, "p4_decay", 0.03f, 2.0f, 0.3f, Curve::Exponential, 15.0f, false},
    {14, "p4_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
    {15, "p5_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {16, "p5_decay", 0.03f, 2.0f, 0.32f, Curve::Exponential, 15.0f, false},
    {17, "p5_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
    {18, "p6_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {19, "p6_decay", 0.03f, 2.0f, 0.18f, Curve::Exponential, 15.0f, false},
    {20, "p6_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
    {21, "p7_tune", -12.0f, 12.0f, 0.0f, Curve::Linear, 15.0f, false},
    {22, "p7_decay", 0.03f, 2.0f, 0.28f, Curve::Exponential, 15.0f, false},
    {23, "p7_level", -24.0f, 6.0f, 0.0f, Curve::Linear, 15.0f, false},
};

}  // namespace ddaw::schema
