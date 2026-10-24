// SPDX-License-Identifier: GPL-2.0-or-later
// Display settings and the smoothing decisions of the desktop host.

#include "test_harness.hpp"

#include "display.hpp"

#include <cmath>
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

TEST_CASE("Display settings for refresh, VRR, sync and fps caps parse and round-trip") {
    Settings s;
    CHECK_EQ(s.refresh, 0);
    CHECK_EQ(s.menu_fps, 30);
    anypsp::display::apply_ini(s, "[display]\nRefresh = 144\nVRR = on\nSyncToMonitor = on\nPauseFps = 60\nFpsCap = 30\n", "t");
    CHECK_EQ(s.refresh, 144);
    CHECK(s.vrr);
    CHECK(s.sync_to_monitor);
    CHECK_EQ(s.menu_fps, 60);
    CHECK_EQ(s.fps_cap, 30);
    Settings back;
    anypsp::display::apply_ini(back, anypsp::display::to_ini(s), "round");
    CHECK(back == s);
    anypsp::display::apply_setting(s, "Refresh", "auto", "--");
    CHECK_EQ(s.refresh, 0);
    CHECK_THROWS(anypsp::display::apply_setting(s, "Refresh", "100", "--"));
    CHECK_THROWS(anypsp::display::apply_setting(s, "FpsCap", "45", "--"));
    CHECK_THROWS(anypsp::display::apply_setting(s, "PauseFps", "off", "--"));
    CHECK_EQ(anypsp::display::next_value(anypsp::display::kRefreshRates, 0), 60);
    CHECK_EQ(anypsp::display::next_value(anypsp::display::kRefreshRates, 240), 0);
    CHECK_EQ(anypsp::display::next_value(anypsp::display::kFpsCaps, 120), 0);
}

TEST_CASE("PSP vblanks are spread evenly over 60, 75, 120, 144 and 165 Hz monitors") {
    using anypsp::display::Cadence;
    for (const double hz : {60.0, 75.0, 120.0, 144.0, 165.0}) {
        const Cadence c(anypsp::display::kPspHz, hz);
        const double ratio = hz / anypsp::display::kPspHz;
        for (std::uint64_t n = 0; n < 36000; ++n) {
            const unsigned v = c.vblanks(n);
            CHECK((v == static_cast<unsigned>(std::floor(ratio)) || v == static_cast<unsigned>(std::ceil(ratio))));
            CHECK(std::abs(static_cast<double>(c.first_vblank(n)) - static_cast<double>(n) * ratio) < 1.0);
        }
    }
    // 60 Hz: one vblank each, a repeat about every 17 s (0.1% apart).
    const Cadence c60(anypsp::display::kPspHz, 60.0);
    std::uint64_t repeats = 0;
    for (std::uint64_t n = 0; n < 36000; ++n) repeats += c60.vblanks(n) == 2u;
    CHECK(repeats >= 30u);
    CHECK(repeats <= 40u);
}

TEST_CASE("Sync to monitor, fps cap intervals and sound steering") {
    using anypsp::display::kPspHz;
    CHECK_EQ(anypsp::display::synced_game_hz(kPspHz, 60.0), 60.0);
    CHECK_EQ(anypsp::display::synced_game_hz(kPspHz, 120.0), 60.0);
    CHECK_EQ(anypsp::display::synced_game_hz(kPspHz, 144.0), kPspHz);
    CHECK_EQ(anypsp::display::synced_game_hz(kPspHz, 0.0), kPspHz);
    CHECK_EQ(anypsp::display::vsync_interval(60.0, 30), 2);
    CHECK_EQ(anypsp::display::vsync_interval(144.0, 0), 1);
    CHECK_EQ(anypsp::display::vsync_interval(144.0, 60), 3);
    CHECK(anypsp::display::same_rate(59.95, 60.0));
    CHECK(!anypsp::display::same_rate(144.0, 0.0));
    CHECK_EQ(anypsp::display::audio_ratio(1.0, 100, 100), 1.0);
    CHECK(anypsp::display::audio_ratio(1.0, 1000000, 100) <= 1.005 + 1e-12);
    CHECK(anypsp::display::audio_ratio(1.0, 0, 100) >= 0.995 - 1e-12);
}

TEST_CASE("Interpolation shows the in-between picture only between 30 fps frames, never across a cut") {
    using anypsp::display::in_between_on_new_frame;
    CHECK(in_between_on_new_frame(true, 2, 0.02));
    CHECK(!in_between_on_new_frame(false, 2, 0.02));
    CHECK(!in_between_on_new_frame(true, 1, 0.02));
    CHECK(!in_between_on_new_frame(true, 3, 0.02));
    CHECK(!in_between_on_new_frame(true, 2, 0.5));
    Settings s;
    CHECK(!s.interpolate);
    anypsp::display::apply_setting(s, "Interpolate", "on", "--");
    Settings back;
    anypsp::display::apply_ini(back, anypsp::display::to_ini(s), "round");
    CHECK(back.interpolate);
}

TEST_CASE("The frame tracker reports how long the frame before was up") {
    anypsp::display::FrameTracker t;
    CHECK(t.after_vblank(1) == 0u);
    CHECK(t.after_vblank(1) == 0u);
    CHECK(t.after_vblank(2) == 2u);
    CHECK(t.after_vblank(3) == 1u);
}
