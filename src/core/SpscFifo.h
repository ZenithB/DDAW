#pragma once
// Lock-free single-producer single-consumer ring (ARCH 1, 9). Fixed capacity,
// allocation-free after construction, wait-free push/pop. T must be trivially
// copyable so a push/pop is a plain copy.
#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace ddaw {

template <class T, size_t Capacity>
class SpscFifo {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "messages must be trivially copyable");

public:
    // Producer thread only. Returns false when full; never blocks.
    bool push(const T& v) noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) == Capacity) return false;
        buf_[h & (Capacity - 1)] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    // Consumer thread only. Returns false when empty; never blocks.
    bool pop(T& out) noexcept {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        out = buf_[t & (Capacity - 1)];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
    // Approximate from either side; exact when called by the consumer with a quiescent producer.
    size_t size() const noexcept { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire); }
    static constexpr size_t capacity() noexcept { return Capacity; }

private:
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    std::array<T, Capacity> buf_{};
};

}  // namespace ddaw
