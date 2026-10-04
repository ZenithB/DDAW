#pragma once
// Input capture ring (ARCH 1): the audio thread writes the device input here while a take is running,
// and the recorder thread drains it to disk. Wait-free for both sides, fixed capacity, no allocation
// after prepare(). A full ring drops frames and counts them: the audio thread never waits.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace ddaw::engine {

class InputTap {
public:
    // Control thread, before audio runs. `frames` is rounded up to a power of two.
    void prepare(size_t frames) {
        size_t n = 1024;
        while (n < frames) n <<= 1;
        l_.assign(n, 0.0f);
        r_.assign(n, 0.0f);
        mask_ = n - 1;
        head_.store(0);
        tail_.store(0);
        dropped_.store(0);
    }
    size_t capacity() const noexcept { return l_.size(); }

    // Producer (audio thread). Returns the frames stored; the rest are dropped and counted.
    size_t push(const float* l, const float* r, size_t n) noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t used = h - tail_.load(std::memory_order_acquire);
        const size_t room = capacity() - used;
        const size_t take = std::min(n, room);
        for (size_t i = 0; i < take; ++i) {
            l_[(h + i) & mask_] = l[i];
            r_[(h + i) & mask_] = r ? r[i] : l[i];
        }
        head_.store(h + take, std::memory_order_release);
        if (take < n) dropped_.fetch_add(n - take, std::memory_order_relaxed);
        return take;
    }
    // Consumer (recorder thread). Returns the frames read.
    size_t pop(float* l, float* r, size_t maxFrames) noexcept {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t avail = head_.load(std::memory_order_acquire) - t;
        const size_t take = std::min(maxFrames, avail);
        for (size_t i = 0; i < take; ++i) {
            l[i] = l_[(t + i) & mask_];
            r[i] = r_[(t + i) & mask_];
        }
        tail_.store(t + take, std::memory_order_release);
        return take;
    }
    size_t available() const noexcept { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire); }
    uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    void clearDropped() noexcept { dropped_.store(0); }

private:
    std::vector<float> l_, r_;
    size_t mask_ = 0;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    std::atomic<uint64_t> dropped_{0};
};

}  // namespace ddaw::engine
