// SpscFloatRing: the sample hand-off from the DDSP worker thread to the audio thread. Run under ThreadSanitizer in the
// nightly sanitizer job (ctest -L threads); here it also checks that no sample is lost, duplicated or reordered.
#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <vector>

#include "core/SpscFloatRing.h"

using ddaw::SpscFloatRing;

TEST_CASE("SpscFloatRing: push, pop, skip and clear keep order and counts", "[spscring]") {
    SpscFloatRing r;
    r.prepare(64);
    REQUIRE(r.capacity() == 64);
    std::vector<float> in(100), out(100);
    for (size_t i = 0; i < in.size(); ++i) in[i] = float(i);
    CHECK(r.push(in.data(), 100) == 64);          // full: the rest is refused, never overwritten
    CHECK(r.available() == 64);
    CHECK(r.pop(out.data(), 10) == 10);
    for (size_t i = 0; i < 10; ++i) CHECK(out[i] == float(i));
    CHECK(r.skip(20) == 20);
    CHECK(r.pop(out.data(), 100) == 34);          // 64 - 10 - 20
    CHECK(out[0] == 30.0f);
    CHECK(r.push(in.data(), 5) == 5);
    r.clear();
    CHECK(r.available() == 0);
    CHECK(r.pop(out.data(), 1) == 0);
    // wraps around the end of the buffer
    for (int round = 0; round < 20; ++round) {
        CHECK(r.push(in.data(), 50) == 50);
        CHECK(r.pop(out.data(), 50) == 50);
        CHECK(out[49] == 49.0f);
    }
}

TEST_CASE("SpscFloatRing: two threads move a counting sequence in odd block sizes without loss", "[spscring][threads]") {
    constexpr size_t kN = 2'000'000;
    static SpscFloatRing r;
    r.prepare(4096);
    std::thread prod([] {
        std::vector<float> blk(331);
        size_t next = 0;
        while (next < kN) {
            const size_t n = std::min<size_t>(kN - next, 17 + next % 300);
            for (size_t i = 0; i < n; ++i) blk[i] = float((next + i) % 100000);
            size_t sent = 0;
            while (sent < n) sent += r.push(blk.data() + sent, n - sent);
            next += n;
        }
    });
    std::vector<float> got(256);
    size_t seen = 0;
    bool ordered = true;
    while (seen < kN) {
        const size_t n = r.pop(got.data(), 1 + seen % 200);
        for (size_t i = 0; i < n; ++i) ordered = ordered && got[i] == float((seen + i) % 100000);
        seen += n;
    }
    prod.join();
    CHECK(ordered);
    CHECK(r.available() == 0);
}
