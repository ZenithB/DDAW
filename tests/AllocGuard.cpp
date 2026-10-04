#include "AllocGuard.h"

#include <cstdlib>
#include <new>

namespace {
thread_local size_t gCount = 0;
void* alloc(std::size_t n) {
    ++gCount;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
}  // namespace

void* operator new(std::size_t n) { return alloc(n); }
void* operator new[](std::size_t n) { return alloc(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { ++gCount; return std::malloc(n ? n : 1); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { ++gCount; return std::malloc(n ? n : 1); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace ddaw::test {
size_t allocationsOnThisThread() { return gCount; }
AllocGuard::AllocGuard() : start_(gCount) {}
AllocGuard::~AllocGuard() = default;
size_t AllocGuard::count() const { return gCount - start_; }
}  // namespace ddaw::test
