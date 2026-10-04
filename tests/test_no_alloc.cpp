#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "AllocGuard.h"
#include "core/Constants.h"
#include "devices/effects/stub_gain.h"

using namespace ddaw;

TEST_CASE("StubGain does not allocate on the audio path", "[realtime]") {
    StubGain d;
    d.prepare(48000.0, kMaxBlock);  // may allocate
    std::vector<float> l(kMaxBlock, 0.3f), r(kMaxBlock, 0.3f);
    ProcessContext c{48000.0, 0.0, 120.0, true, false, 0.0, 0.0, 4, 4};

    test::AllocGuard guard;
    for (int i = 0; i < 1000; ++i) {
        d.setParam(StubGain::Gain, float(i % 3));
        d.process(l.data(), r.data(), kMaxBlock, c, {});
    }
    d.reset();
    CHECK(guard.count() == 0);
}

TEST_CASE("AllocGuard detects allocation (the guard itself works)", "[realtime]") {
    test::AllocGuard guard;
    int* volatile p = new int(7);  // volatile pointer: the allocation cannot be elided
    delete p;
    CHECK(guard.count() >= 1);
}
