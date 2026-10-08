#pragma once
// The scales the project knows (synthyy's theory.ts SCALES): root-relative semitone sets, shared by the `scale` MIDI effect, the
// arpeggiator's scale walk and `autotune`. The project's key is `Meta::root` (a pitch class, 9 = A) with `Meta::scale` (an id).
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace ddaw::dsp {

struct ScaleDef {
    const char* id;     // the project's name for it
    const char* name;   // what a person reads
    std::span<const int> ivs;
};

namespace detail {
inline constexpr int kMajor[] = {0, 2, 4, 5, 7, 9, 11};
inline constexpr int kMinor[] = {0, 2, 3, 5, 7, 8, 10};
inline constexpr int kDorian[] = {0, 2, 3, 5, 7, 9, 10};
inline constexpr int kMixo[] = {0, 2, 4, 5, 7, 9, 10};
inline constexpr int kPentMaj[] = {0, 2, 4, 7, 9};
inline constexpr int kPentMin[] = {0, 3, 5, 7, 10};
inline constexpr int kHarmMin[] = {0, 2, 3, 5, 7, 8, 11};
inline constexpr int kBlues[] = {0, 3, 5, 6, 7, 10};
}  // namespace detail

inline constexpr int kScaleCount = 8;

// theory.ts SCALES, in the project's order (unknown ids fall back to major, index 0).
inline const std::array<ScaleDef, kScaleCount>& scales() {
    static const std::array<ScaleDef, kScaleCount> s{{
        {"major", "Major", detail::kMajor},
        {"minor", "Minor", detail::kMinor},
        {"dorian", "Dorian", detail::kDorian},
        {"mixo", "Mixolydian", detail::kMixo},
        {"pentMaj", "Pentatonic major", detail::kPentMaj},
        {"pentMin", "Pentatonic minor", detail::kPentMin},
        {"harmMin", "Harmonic minor", detail::kHarmMin},
        {"blues", "Blues", detail::kBlues},
    }};
    return s;
}

inline int scaleIndex(std::string_view id) {
    for (int i = 0; i < kScaleCount; ++i) if (id == scales()[size_t(i)].id) return i;
    return 0;
}

// The scale as a 12-bit mask of root-relative pitch classes (bit d set: d semitones above the root is in the scale).
inline uint32_t scaleMask(int index) {
    uint32_t m = 0;
    for (int iv : scales()[size_t(index < 0 || index >= kScaleCount ? 0 : index)].ivs) m |= 1u << unsigned(iv);
    return m;
}

inline const char* noteName(int pitchClass) {
    static const char* n[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return n[((pitchClass % 12) + 12) % 12];
}

}  // namespace ddaw::dsp
