#pragma once
// Flush-to-zero / denormals-are-zero for the current thread (ARCH 1: "FTZ/DAZ is set once on the audio
// thread; devices may assume it"). The Engine sets it for the duration of process(), so live callbacks,
// offline renders and tests all run under the same floating-point mode.
#include <cstdint>

#if defined(__aarch64__) || defined(__arm64__)
#elif defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace ddaw {

class ScopedFlushDenormals {
public:
    ScopedFlushDenormals() noexcept {
#if defined(__aarch64__) || defined(__arm64__)
        std::uint64_t fpcr;
        asm volatile("mrs %0, fpcr" : "=r"(fpcr));
        saved_ = fpcr;
        asm volatile("msr fpcr, %0" : : "r"(fpcr | (1ull << 24)));  // FZ
#elif defined(__x86_64__) || defined(__i386__)
        saved_ = _mm_getcsr();
        _mm_setcsr(saved_ | 0x8040u);  // FTZ | DAZ
#endif
    }
    ~ScopedFlushDenormals() {
#if defined(__aarch64__) || defined(__arm64__)
        asm volatile("msr fpcr, %0" : : "r"(saved_));
#elif defined(__x86_64__) || defined(__i386__)
        _mm_setcsr(static_cast<unsigned int>(saved_));
#endif
    }
    ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

private:
    std::uint64_t saved_ = 0;
};

}  // namespace ddaw
