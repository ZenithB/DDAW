#pragma once
// Shared scheduling types (port of sf-engine scheduler.rs NoteEv / XorShift).
#include <cstdint>

namespace ddaw::engine {

// One expanded note event. `tick` is clip-relative (session pattern) or absolute (arrangement).
struct NoteEv {
    double tick = 0;
    uint8_t pitch = 60;
    double durTicks = 0;
    float vel = 1.0f;
    float pr = 1.0f;  // probability that the note fires, rolled at fire time
};

// Deterministic xorshift64*: humanise/probability/arp-random rolls must repeat across runs.
class XorShift {
public:
    explicit XorShift(uint64_t seed = 0) : s_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}
    uint64_t nextU64() noexcept {
        uint64_t x = s_;
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        s_ = x;
        return x * 0x2545F4914F6CDD1Dull;
    }
    double nextF64() noexcept { return double(nextU64() >> 11) / double(1ull << 53); }  // [0, 1)
private:
    uint64_t s_;
};

}  // namespace ddaw::engine
