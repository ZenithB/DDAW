#pragma once
// Wait-free single-producer single-consumer ring of floats, moved in blocks. For audio handed between threads
// (a worker thread's output to the audio thread). Fixed capacity (a power of two), no allocation after prepare().
#include <algorithm>
#include <atomic>
#include <vector>

namespace ddaw {

class SpscFloatRing {
public:
    void prepare(size_t capacityPow2) {
        size_t n = 64;
        while (n < capacityPow2) n <<= 1;
        buf_.assign(n, 0.0f);
        mask_ = n - 1;
        head_.store(0);
        tail_.store(0);
    }
    size_t capacity() const noexcept { return buf_.size(); }
    // Producer. Returns the number written (less than n when full).
    size_t push(const float* x, size_t n) noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t room = buf_.size() - (h - tail_.load(std::memory_order_acquire));
        const size_t take = std::min(n, room);
        for (size_t i = 0; i < take; ++i) buf_[(h + i) & mask_] = x[i];
        head_.store(h + take, std::memory_order_release);
        return take;
    }
    // Consumer. Returns the number read.
    size_t pop(float* out, size_t n) noexcept {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t avail = head_.load(std::memory_order_acquire) - t;
        const size_t take = std::min(n, avail);
        for (size_t i = 0; i < take; ++i) out[i] = buf_[(t + i) & mask_];
        tail_.store(t + take, std::memory_order_release);
        return take;
    }
    // Consumer: discard up to n queued samples.
    size_t skip(size_t n) noexcept {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t take = std::min(n, head_.load(std::memory_order_acquire) - t);
        tail_.store(t + take, std::memory_order_release);
        return take;
    }
    size_t available() const noexcept { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire); }
    // Consumer: drop everything currently queued.
    void clear() noexcept { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }

private:
    std::vector<float> buf_;
    size_t mask_ = 0;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

}  // namespace ddaw
