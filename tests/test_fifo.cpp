#include <catch2/catch_test_macros.hpp>
#include <thread>

#include "AllocGuard.h"
#include "core/Cmd.h"

using namespace ddaw;

TEST_CASE("SpscFifo: order, full and empty", "[fifo]") {
    SpscFifo<int, 8> f;
    int v;
    CHECK_FALSE(f.pop(v));
    for (int i = 0; i < 8; ++i) CHECK(f.push(i));
    CHECK_FALSE(f.push(99));  // full: never blocks, never overwrites
    CHECK(f.size() == 8);
    for (int i = 0; i < 8; ++i) { REQUIRE(f.pop(v)); CHECK(v == i); }
    CHECK_FALSE(f.pop(v));
    for (int round = 0; round < 100; ++round) {  // wraparound
        CHECK(f.push(round)); REQUIRE(f.pop(v)); CHECK(v == round);
    }
}

TEST_CASE("SpscFifo: two threads deliver every item in order", "[fifo][threads]") {
    constexpr int kN = 1'000'000;
    static SpscFifo<int, 1024> f;
    std::thread prod([] { for (int i = 0; i < kN;) if (f.push(i)) ++i; });
    int expected = 0;
    while (expected < kN) {
        int v;
        if (f.pop(v)) { REQUIRE(v == expected); ++expected; }
    }
    prod.join();
}

TEST_CASE("SpscFifo push/pop do not allocate", "[fifo][realtime]") {
    CommandFifo f;
    Cmd c; c.type = CmdType::SetParam; c.setParam = {{0, kSlotInst, 1}, 0.5f};
    test::AllocGuard guard;
    for (int i = 0; i < 10000; ++i) { f.push(c); Cmd o; f.pop(o); }
    CHECK(guard.count() == 0);
}

TEST_CASE("Cmd layout and epoch rule", "[fifo]") {
    CHECK(sizeof(Cmd) <= 32);
    CHECK(isGraphAddressed(CmdType::NoteOn));
    CHECK(isGraphAddressed(CmdType::SetParam));
    CHECK_FALSE(isGraphAddressed(CmdType::TransportPlay));
    CHECK_FALSE(isGraphAddressed(CmdType::SetTempo));
}
