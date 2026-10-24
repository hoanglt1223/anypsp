// SPDX-License-Identifier: GPL-2.0-or-later
// Pairing of primitives between game frames for frame interpolation.

#include "test_harness.hpp"

#include "ge_interp.hpp"

#include <vector>

using pspweb::FrameMatcher;
using pspweb::PrimKey;

namespace {
std::vector<float> at(float x) { return {x, 0.0f, 0.0f, 1.0f}; }
} // namespace

TEST_CASE("Interpolation pairs nothing in the first frame") {
    FrameMatcher m;
    CHECK(m.match(PrimKey{0x100, 0, 1, 1, 3, 1}, at(1.0f)) == nullptr);
}

TEST_CASE("Interpolation pairs the n-th draw from the same vertex address") {
    FrameMatcher m;
    const PrimKey a{0x100, 0, 1, 1, 3, 1}, b{0x200, 0, 1, 1, 3, 1};
    m.match(a, at(1.0f));
    m.match(b, at(2.0f));
    m.match(a, at(3.0f));
    m.next_frame();
    // Drawn in another order: still paired by address and occurrence.
    const float *p = m.match(b, at(20.0f));
    CHECK(p != nullptr && p[0] == 2.0f);
    p = m.match(a, at(10.0f));
    CHECK(p != nullptr && p[0] == 1.0f);
    p = m.match(a, at(30.0f));
    CHECK(p != nullptr && p[0] == 3.0f);
    CHECK(m.totals().matched == 3u);
}

TEST_CASE("Interpolation pairs moved vertex buffers by shape in drawing order") {
    FrameMatcher m;
    m.match(PrimKey{0x1000, 0, 1, 1, 3, 1}, at(1.0f));
    m.match(PrimKey{0x1010, 0, 2, 1, 3, 1}, at(2.0f));
    m.next_frame();
    // New addresses (built every frame): the first unused primitive of the same shape.
    const float *p = m.match(PrimKey{0x5000, 0, 2, 1, 3, 1}, at(5.0f));
    CHECK(p != nullptr && p[0] == 2.0f);
    CHECK(m.match(PrimKey{0x5010, 0, 9, 1, 3, 1}, at(6.0f)) == nullptr); // no such shape
}

TEST_CASE("Interpolation keeps only the frame before") {
    FrameMatcher m;
    const PrimKey a{0x100, 0, 1, 1, 3, 1};
    m.match(a, at(1.0f));
    m.next_frame();
    m.next_frame(); // a frame without it
    CHECK(m.match(a, at(2.0f)) == nullptr);
}
