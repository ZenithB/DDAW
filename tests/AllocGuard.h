#pragma once
// Custom allocator hook (PLAN 6): counts global operator new calls made while a
// guard is active on the current thread. Wrap audio-thread code under test.
#include <cstddef>

namespace ddaw::test {

size_t allocationsOnThisThread();

class AllocGuard {
public:
    AllocGuard();
    ~AllocGuard();
    // Allocations (operator new calls) since construction.
    size_t count() const;
private:
    size_t start_;
};

}  // namespace ddaw::test
