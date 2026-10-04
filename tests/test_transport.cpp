#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/Transport.h"

using ddaw::engine::Transport;

TEST_CASE("transport: ticks, frames and play-from", "[transport]") {
    Transport t; t.prepare(48000.0); t.setTempo(120.0);
    CHECK(t.framesPerTick() == Catch::Approx(48000.0 * 60.0 / 120.0 / 96.0));   // 250
    CHECK_FALSE(t.playing());
    t.play(96.0);                                      // start at beat 1
    CHECK(t.positionTicks() == 96.0);
    CHECK(t.framesUntilTick(96.0 + 96.0) == 24000);    // one beat later = 0.5 s
    t.advance(12000);
    CHECK(t.positionTicks() == Catch::Approx(96.0 + 48.0));
    CHECK(t.framesUntilTick(96.0 + 96.0) == 12000);
    CHECK(t.framesUntilTick(96.0) < 0);                // already passed
}

TEST_CASE("transport: tempo change re-anchors without moving the past or the playhead", "[transport]") {
    Transport t; t.prepare(48000.0); t.setTempo(120.0);
    t.play(0.0);
    t.advance(24000);                                  // one beat
    const double before = t.positionTicks();
    t.setTempo(60.0);                                  // half speed from here
    CHECK(t.positionTicks() == Catch::Approx(before));
    CHECK(t.framesUntilTick(before + 96.0) == 48000);  // next beat now takes a second
    t.advance(48000);
    CHECK(t.positionTicks() == Catch::Approx(before + 96.0));
}

TEST_CASE("transport holds its position while stopped", "[transport]") {
    Transport t; t.prepare(44100.0);
    t.play(10.0); t.advance(4410); const double p = t.positionTicks();
    t.stop(); t.advance(100000);
    CHECK(t.positionTicks() == p);
    CHECK_FALSE(t.playing());
}
