#pragma once
// Parameters of the synth families added in B6. Hand-written - Schema.generated.h is generated from synthyy's registry and
// must not be edited. Header-only so the UI catalog can list the devices. Parameters with the last field true accept
// audio-rate modulation; their A-rate ordinals are their positions among those, in order (noted per device).
#include "core/Device.h"

namespace ddaw::schema {

// `harmnoise`: a hand-controlled harmonic-plus-noise synth, the DDSP signal model without the network. Up to 48 harmonics
// whose levels are set by a spectral tilt, an odd/even balance, a formant bump and an inharmonic stretch, plus noise through
// a band-pass. Everything is one smooth control, so a morph map or the joystick can move the whole spectrum.
inline constexpr ParamSpec kInstHarmnoise[] = {
    {0, "harmonics", 1.0f, 48.0f, 24.0f, Curve::Linear, 15.0f, false},
    {1, "tilt", -24.0f, 6.0f, -6.0f, Curve::Linear, 15.0f, false},          // dB per octave of partial number
    {2, "oddEven", -1.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},         // +1: odd partials only (hollow), -1: even only
    {3, "stretch", 0.0f, 0.02f, 0.0f, Curve::Linear, 15.0f, false},         // inharmonicity B: f_k = k f0 sqrt(1 + B k^2)
    {4, "formant", 200.0f, 8000.0f, 1200.0f, Curve::Exponential, 15.0f, false},
    {5, "fWidth", 0.1f, 3.0f, 1.0f, Curve::Linear, 15.0f, false},           // octaves (one standard deviation)
    {6, "fGain", 0.0f, 24.0f, 0.0f, Curve::Linear, 15.0f, false},           // dB at the formant centre
    {7, "noise", 0.0f, 1.0f, 0.1f, Curve::Linear, 15.0f, false},
    {8, "noiseHz", 100.0f, 12000.0f, 3000.0f, Curve::Exponential, 15.0f, false},
    {9, "noiseQ", 0.3f, 12.0f, 1.0f, Curve::Exponential, 15.0f, false},
    {10, "noiseTrack", 0.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},      // how much the noise band follows the pitch
    {11, "chiff", 0.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},           // extra noise burst at the start of a note
    {12, "vibRate", 0.5f, 12.0f, 5.0f, Curve::Exponential, 15.0f, false},
    {13, "vibAmt", 0.0f, 100.0f, 0.0f, Curve::Linear, 15.0f, false},        // cents
    {14, "attack", 0.001f, 2.0f, 0.02f, Curve::Exponential, 15.0f, false},
    {15, "decay", 0.01f, 4.0f, 0.3f, Curve::Exponential, 15.0f, false},
    {16, "sustain", 0.0f, 1.0f, 0.8f, Curve::Linear, 15.0f, false},
    {17, "release", 0.01f, 4.0f, 0.3f, Curve::Exponential, 15.0f, false},
    {18, "level", 0.0f, 1.5f, 0.7f, Curve::Linear, 15.0f, false},
};

// `subtractive`: two oscillators (PolyBLEP saw, pulse, triangle, sine), a sub oscillator and noise into a resonant ladder
// filter with its own ADSR, key tracking and velocity response, then the amplitude ADSR. A-rate: cutoff 0, pitch 1.
inline constexpr ParamSpec kInstSubtractive[] = {
    {0, "wave1", 0.0f, 3.0f, 0.0f, Curve::Stepped, 0.0f, false},           // 0 saw, 1 pulse, 2 triangle, 3 sine
    {1, "wave2", 0.0f, 3.0f, 1.0f, Curve::Stepped, 0.0f, false},
    {2, "pw", 0.05f, 0.95f, 0.5f, Curve::Linear, 15.0f, false},            // pulse width
    {3, "semi2", -24.0f, 24.0f, 0.0f, Curve::Linear, 15.0f, false},
    {4, "detune2", -50.0f, 50.0f, 7.0f, Curve::Linear, 15.0f, false},      // cents
    {5, "mix2", 0.0f, 1.0f, 0.5f, Curve::Linear, 15.0f, false},
    {6, "sub", 0.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},             // square one octave down
    {7, "noise", 0.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},
    {8, "cutoff", 40.0f, 16000.0f, 2500.0f, Curve::Exponential, 15.0f, true},
    {9, "res", 0.0f, 1.0f, 0.2f, Curve::Linear, 15.0f, false},
    {10, "slope", 0.0f, 1.0f, 1.0f, Curve::Stepped, 0.0f, false},          // 0: 12 dB/oct, 1: 24 dB/oct
    {11, "drive", 0.0f, 1.0f, 0.1f, Curve::Linear, 15.0f, false},
    {12, "keytrack", 0.0f, 1.0f, 0.5f, Curve::Linear, 15.0f, false},
    {13, "envAmt", -1.0f, 1.0f, 0.4f, Curve::Linear, 15.0f, false},        // filter envelope, 6 octaves at 1
    {14, "fAttack", 0.001f, 2.0f, 0.005f, Curve::Exponential, 15.0f, false},
    {15, "fDecay", 0.01f, 4.0f, 0.3f, Curve::Exponential, 15.0f, false},
    {16, "fSustain", 0.0f, 1.0f, 0.3f, Curve::Linear, 15.0f, false},
    {17, "fRelease", 0.01f, 4.0f, 0.3f, Curve::Exponential, 15.0f, false},
    {18, "velFilt", 0.0f, 1.0f, 0.3f, Curve::Linear, 15.0f, false},        // how much velocity opens the filter
    {19, "attack", 0.001f, 2.0f, 0.01f, Curve::Exponential, 15.0f, false},
    {20, "decay", 0.01f, 4.0f, 0.3f, Curve::Exponential, 15.0f, false},
    {21, "sustain", 0.0f, 1.0f, 0.7f, Curve::Linear, 15.0f, false},
    {22, "release", 0.01f, 4.0f, 0.3f, Curve::Exponential, 15.0f, false},
    {23, "pitch", -24.0f, 24.0f, 0.0f, Curve::Linear, 15.0f, true},        // semitones, added to every oscillator
    {24, "level", 0.0f, 1.0f, 0.6f, Curve::Linear, 15.0f, false},
};

// `wavetable`: four built-in banks of eight single-cycle frames (classic shapes, vowels, metallic, organ), mip-mapped so a
// high note never reads partials above the Nyquist; `pos` morphs across the frames. Up to five unison oscillators with
// detune and stereo spread, a sub, a state-variable low-pass. A-rate: pos 0, pitch 1.
inline constexpr ParamSpec kInstWavetable[] = {
    {0, "bank", 0.0f, 3.0f, 0.0f, Curve::Stepped, 0.0f, false},
    {1, "pos", 0.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, true},
    {2, "posEnv", -1.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},         // how far the position envelope moves it
    {3, "posDecay", 0.01f, 4.0f, 0.5f, Curve::Exponential, 15.0f, false},
    {4, "unison", 1.0f, 5.0f, 1.0f, Curve::Stepped, 0.0f, false},
    {5, "detune", 0.0f, 50.0f, 12.0f, Curve::Linear, 15.0f, false},        // cents, outermost oscillator
    {6, "spread", 0.0f, 1.0f, 0.5f, Curve::Linear, 15.0f, false},
    {7, "sub", 0.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},
    {8, "cutoff", 40.0f, 16000.0f, 16000.0f, Curve::Exponential, 15.0f, false},
    {9, "res", 0.0f, 1.0f, 0.1f, Curve::Linear, 15.0f, false},
    {10, "keytrack", 0.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},
    {11, "attack", 0.001f, 2.0f, 0.01f, Curve::Exponential, 15.0f, false},
    {12, "decay", 0.01f, 4.0f, 0.3f, Curve::Exponential, 15.0f, false},
    {13, "sustain", 0.0f, 1.0f, 0.8f, Curve::Linear, 15.0f, false},
    {14, "release", 0.01f, 4.0f, 0.3f, Curve::Exponential, 15.0f, false},
    {15, "pitch", -24.0f, 24.0f, 0.0f, Curve::Linear, 15.0f, true},
    {16, "level", 0.0f, 1.0f, 0.6f, Curve::Linear, 15.0f, false},
};

// `waveshaper`: a sine, triangle or saw through a transfer function at four times the host rate. Shapes: 0 soft clip, 1 sine
// fold, 2 Chebyshev (h2..h5 add exactly those harmonics), 3 diode (asymmetric), 4 hard clip, 5 drawn curve (k1..k5 are its
// heights at -1, -0.5, 0, 0.5, 1). `drive` sets how hard the shaper is hit and falls over each note (dDecay, dSus).
// A-rate: drive 0, bias 1, pitch 2.
inline constexpr ParamSpec kInstWaveshaper[] = {
    {0, "src", 0.0f, 2.0f, 0.0f, Curve::Stepped, 0.0f, false},
    {1, "shape", 0.0f, 5.0f, 0.0f, Curve::Stepped, 0.0f, false},
    {2, "drive", 0.0f, 10.0f, 2.0f, Curve::Linear, 15.0f, true},
    {3, "bias", -1.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, true},
    {4, "h2", 0.0f, 1.0f, 0.5f, Curve::Linear, 15.0f, false},
    {5, "h3", 0.0f, 1.0f, 0.3f, Curve::Linear, 15.0f, false},
    {6, "h4", 0.0f, 1.0f, 0.15f, Curve::Linear, 15.0f, false},
    {7, "h5", 0.0f, 1.0f, 0.1f, Curve::Linear, 15.0f, false},
    {8, "k1", -1.0f, 1.0f, -0.9f, Curve::Linear, 15.0f, false},
    {9, "k2", -1.0f, 1.0f, -0.5f, Curve::Linear, 15.0f, false},
    {10, "k3", -1.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},
    {11, "k4", -1.0f, 1.0f, 0.5f, Curve::Linear, 15.0f, false},
    {12, "k5", -1.0f, 1.0f, 0.9f, Curve::Linear, 15.0f, false},
    {13, "dDecay", 0.01f, 4.0f, 0.5f, Curve::Exponential, 15.0f, false},
    {14, "dSus", 0.0f, 1.0f, 0.4f, Curve::Linear, 15.0f, false},
    {15, "velDrive", 0.0f, 1.0f, 0.5f, Curve::Linear, 15.0f, false},
    {16, "attack", 0.001f, 2.0f, 0.005f, Curve::Exponential, 15.0f, false},
    {17, "decay", 0.01f, 4.0f, 0.4f, Curve::Exponential, 15.0f, false},
    {18, "sustain", 0.0f, 1.0f, 0.7f, Curve::Linear, 15.0f, false},
    {19, "release", 0.01f, 4.0f, 0.3f, Curve::Exponential, 15.0f, false},
    {20, "pitch", -24.0f, 24.0f, 0.0f, Curve::Linear, 15.0f, true},
    {21, "level", 0.0f, 1.0f, 0.6f, Curve::Linear, 15.0f, false},
};

// `modal`: up to 24 damped resonances struck by a mallet, noise burst or impulse. Models: 0 string (harmonic), 1 free bar,
// 2 marimba bar (1:4:10), 3 membrane, 4 plate, 5 bell, 6 closed tube (odd harmonics). Each mode rings for `decay` seconds
// scaled by its frequency (`damp`), is weighted by the strike position (`pos`), and rings out in `release` when the note ends.
inline constexpr ParamSpec kInstModal[] = {
    {0, "model", 0.0f, 6.0f, 1.0f, Curve::Stepped, 0.0f, false},
    {1, "modes", 2.0f, 24.0f, 16.0f, Curve::Stepped, 0.0f, false},
    {2, "inharm", -0.3f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},
    {3, "decay", 0.05f, 12.0f, 1.5f, Curve::Exponential, 15.0f, false},
    {4, "damp", 0.0f, 1.0f, 0.5f, Curve::Linear, 15.0f, false},
    {5, "pos", 0.02f, 0.5f, 0.2f, Curve::Linear, 15.0f, false},
    {6, "tone", -1.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},
    {7, "exciter", 0.0f, 2.0f, 2.0f, Curve::Stepped, 0.0f, false},         // 0 impulse, 1 noise burst, 2 mallet
    {8, "hard", 0.0f, 1.0f, 0.5f, Curve::Linear, 15.0f, false},
    {9, "release", 0.01f, 4.0f, 0.15f, Curve::Exponential, 15.0f, false},
    {10, "spread", 0.0f, 1.0f, 0.3f, Curve::Linear, 15.0f, false},
    {11, "level", 0.0f, 1.0f, 0.7f, Curve::Linear, 15.0f, false},
};

// `perc`: a hybrid percussion voice - a sine body with a pitch sweep, a filtered noise burst (with clap re-strikes), a
// cluster of six metallic squares and a click. Kick, snare, toms, hats and claps are settings of one voice.
inline constexpr ParamSpec kInstPerc[] = {
    {0, "tune", 20.0f, 2000.0f, 120.0f, Curve::Exponential, 15.0f, false},   // Hz of the body at note 60
    {1, "track", 0.0f, 1.0f, 1.0f, Curve::Linear, 15.0f, false},             // how much the note moves the body pitch
    {2, "sweep", 0.0f, 4.0f, 2.0f, Curve::Linear, 15.0f, false},             // octaves above the tune at the start
    {3, "sweepTime", 0.002f, 0.5f, 0.04f, Curve::Exponential, 15.0f, false},
    {4, "body", 0.0f, 1.0f, 0.8f, Curve::Linear, 15.0f, false},
    {5, "bodyDecay", 0.02f, 3.0f, 0.3f, Curve::Exponential, 15.0f, false},
    {6, "noise", 0.0f, 1.0f, 0.15f, Curve::Linear, 15.0f, false},
    {7, "noiseMode", 0.0f, 2.0f, 1.0f, Curve::Stepped, 0.0f, false},         // 0 low-pass, 1 band-pass, 2 high-pass
    {8, "noiseHz", 100.0f, 16000.0f, 4000.0f, Curve::Exponential, 15.0f, false},
    {9, "noiseQ", 0.4f, 10.0f, 1.0f, Curve::Exponential, 15.0f, false},
    {10, "noiseDecay", 0.005f, 2.0f, 0.12f, Curve::Exponential, 15.0f, false},
    {11, "clap", 0.0f, 4.0f, 0.0f, Curve::Stepped, 0.0f, false},             // extra noise strikes, 9 ms apart
    {12, "metal", 0.0f, 1.0f, 0.0f, Curve::Linear, 15.0f, false},
    {13, "metalTune", 0.5f, 2.0f, 1.0f, Curve::Exponential, 15.0f, false},
    {14, "metalDecay", 0.01f, 2.0f, 0.1f, Curve::Exponential, 15.0f, false},
    {15, "click", 0.0f, 1.0f, 0.3f, Curve::Linear, 15.0f, false},
    {16, "drive", 0.0f, 1.0f, 0.2f, Curve::Linear, 15.0f, false},
    {17, "gate", 0.0f, 1.0f, 0.0f, Curve::Stepped, 0.0f, false},             // 1: note-off damps the voice
    {18, "release", 0.005f, 2.0f, 0.05f, Curve::Exponential, 15.0f, false},
    {19, "level", 0.0f, 1.0f, 0.8f, Curve::Linear, 15.0f, false},
};

}  // namespace ddaw::schema
