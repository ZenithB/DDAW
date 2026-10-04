#pragma once
// A stand-in for an audio interface: runs an Engine in fixed blocks on its own thread, feeding the
// engine's output back as its input after `loopback` frames (the round trip a real interface adds), as
// fast as the machine allows. Counts allocations made on that thread.
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

#include "AllocGuard.h"
#include "engine/Engine.h"

namespace ddaw::test {

// A stand-in for the device: runs the engine in blocks, feeding back its own output after `loopback`
// frames (the round trip a real interface adds), as fast as the machine allows.
struct FakeDevice {
    engine::Engine& e;
    int loopback;
    int block;
    bool paced = false;                      // run against the clock (real time) instead of as fast as possible
    std::function<float(uint64_t)> inputFn;   // when set, the input is this signal (by input frame) instead of the loopback
    std::atomic<bool> run{true};
    std::atomic<uint64_t> frames{0};
    std::thread t;
    FakeDevice(engine::Engine& eng, int loop, int blk) : e(eng), loopback(loop), block(blk) {
        t = std::thread([this] {
            const size_t nb = static_cast<size_t>(block);
            std::vector<float> l(nb), r(nb), il(nb);
            std::vector<float> line(static_cast<size_t>(loopback), 0.0f);   // a fixed ring: the fake device itself must not allocate
            size_t pos = 0;
            const auto t0 = std::chrono::steady_clock::now();
            test::AllocGuard guard;
            while (run.load(std::memory_order_acquire)) {
                for (int i = 0; i < block; ++i) { il[size_t(i)] = inputFn ? inputFn(frames.load() + uint64_t(i)) : line[(pos + size_t(i)) % line.size()]; }   // input = output of `loopback` frames ago
                e.processIO(il.data(), il.data(), l.data(), r.data(), block);
                for (int i = 0; i < block; ++i) line[(pos + size_t(i)) % line.size()] = l[size_t(i)];
                pos = (pos + size_t(block)) % line.size();
                frames += uint64_t(block);
                if (paced) std::this_thread::sleep_until(t0 + std::chrono::microseconds(int64_t(double(frames.load()) * 1e6 / 48000.0)));
                else if ((frames.load() / uint64_t(block)) % 8 == 0) std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            allocs = guard.count();
        });
    }
    ~FakeDevice() { stop(); }
    void stop() { if (t.joinable()) { run = false; t.join(); } }
    size_t allocs = 0;
};


}  // namespace ddaw::test
