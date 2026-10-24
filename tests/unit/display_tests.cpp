// SPDX-License-Identifier: GPL-2.0-or-later
// Display settings and the smoothing decisions of the desktop host.

#include "test_harness.hpp"

#include "display.hpp"

#include <cstdint>
#include <vector>

using anypsp::display::Settings;

TEST_CASE("Display settings default to no smoothing and round-trip through the ini file") {
    Settings s;
    CHECK(!s.smoothing);
    anypsp::display::apply_ini(s, "# comment\n[display]\nSmoothing = on\n", "test.ini");
    CHECK(s.smoothing);
    Settings back;
    anypsp::display::apply_ini(back, anypsp::display::to_ini(s), "round");
    CHECK(back == s);
    anypsp::display::apply_setting(s, "smoothing", "off", "--");
    CHECK(!s.smoothing);
}

TEST_CASE("Mistakes in the display file are refused") {
    Settings s;
    CHECK_THROWS(anypsp::display::apply_ini(s, "Smoothing = on\n", "x"));
    CHECK_THROWS(anypsp::display::apply_ini(s, "[video]\n", "x"));
    CHECK_THROWS(anypsp::display::apply_ini(s, "[display]\nSmoothing = maybe\n", "x"));
    CHECK_THROWS(anypsp::display::apply_ini(s, "[display]\nWarp = on\n", "x"));
}

TEST_CASE("Smoothing blends only the in-between vblank of 30 fps frames, never across a cut") {
    using anypsp::display::blend_on_new_frame;
    CHECK(blend_on_new_frame(true, 2, 0.02));
    CHECK(!blend_on_new_frame(false, 2, 0.02)); // off
    CHECK(!blend_on_new_frame(true, 1, 0.02));  // 60 fps: nothing in between
    CHECK(!blend_on_new_frame(true, 3, 0.02));  // uneven pacing: shown as it is
    CHECK(!blend_on_new_frame(true, 2, 0.5));   // scene cut
}

TEST_CASE("Picture difference is 0 for the same picture and 1 for opposite ones") {
    const std::vector<std::uint8_t> black(64, 0), white(64, 255);
    std::vector<std::uint8_t> half(64, 0);
    for (std::size_t i = 0; i < half.size(); i += 8) half[i] = half[i + 1] = half[i + 2] = 255;
    CHECK_EQ(anypsp::display::mean_difference(black, black), 0.0);
    CHECK_EQ(anypsp::display::mean_difference(black, white), 1.0);
    CHECK_EQ(anypsp::display::mean_difference(black, half), 0.5);
    CHECK_EQ(anypsp::display::mean_difference(black, std::vector<std::uint8_t>(60, 0)), 1.0); // size mismatch
}
