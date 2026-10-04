#pragma once
// Audio-clip playback for audio tracks (port of sf-engine graph.rs DerivedClip / AudioVoice / render_audio,
// itself synthyy's audioclip.ts). A clip is DERIVED off the audio thread (crop to offset/dur, loop crossfade,
// reverse, pitch rate); a small voice pool then plays derived clips: a session slot loops or re-fires one-shot
// each clip length from the launch anchor, arrangement clips start and stop at absolute ticks with a fade-out.
// All voice state is plain data: no allocation on the audio thread.
#include <array>
#include <cmath>
#include <optional>
#include <vector>

#include "core/Sample.h"
#include "engine/Scheduler.h"
#include "project/Project.h"

namespace ddaw::engine {

struct DerivedClip {
    std::vector<float> l, r;  // r empty = mono
    double step = 1.0;        // source frames per engine sample (pitch, cents, sample-rate ratio)
    bool looped = false;
    float gain = 1.0f;
    double fadeIn = 0, fadeOut = 0;  // engine samples

    double frames() const noexcept { return static_cast<double>(l.size()); }
    // Linear-interpolated read; a looped clip wraps its tail back to its head.
    void read(double pos, float& outL, float& outR) const noexcept;

    // audioclip.ts semantics. nullopt for an empty or unusable buffer.
    static std::optional<DerivedClip> build(const project::AudioClipData& a, const SampleBuf& buf, double engineSr, double bpm);
};

using AudioSlot = AudioSlotRef;

// A bounded arrangement audio clip.
struct AudioArrEv {
    double tick = 0, stopTick = 0;
    int clip = 0;
};

class AudioPlayer {
public:
    // ---- build side ----
    std::vector<DerivedClip> clips;
    std::vector<AudioArrEv> arr;  // sorted by tick
    bool empty() const noexcept { return clips.empty(); }

    // ---- audio side ----
    // Mix this block into l/r. `pos` is the playhead in ticks at the first frame, `tps` ticks per sample.
    void render(float* l, float* r, int n, double pos, double tps, TransportMode mode, bool playing,
                const std::optional<AudioSlot>& launched, double anchor) noexcept;

private:
    static constexpr int kVoices = 6;           // voice 0 is the session voice, 1.. are arrangement voices
    struct Voice {
        bool active = false;
        int clip = 0;
        double pos = 0, played = 0, beginTick = 0;
        double stopTick = INFINITY, nextFire = INFINITY, firePeriod = 0, anchor = 0;
    };
    void initSessionVoice(const AudioSlot& s, double anchor, double now, double tps) noexcept;
    void activateArr(const AudioArrEv& ev, double pos, double played) noexcept;

    std::array<Voice, kVoices> voices_{};
    size_t arrIdx_ = 0;
    double expected_ = NAN;
    TransportMode mode_ = TransportMode::Session;
};

}  // namespace ddaw::engine
