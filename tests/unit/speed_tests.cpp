// SPDX-License-Identifier: GPL-2.0-or-later
// Speed and fast-forward arithmetic of the desktop player.

#include "test_harness.hpp"

#include "input_map.hpp"
#include "speed.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace speed = anypsp::speed;

TEST_CASE("speed: vblanks per shown picture keep the exact long-run rate") {
    for (const int percent : {100, 150, 200, 300, 400, 500, 800}) {
        speed::VblankBudget budget;
        int total = 0, most = 0, least = 1000;
        for (int i = 0; i < 600; ++i) {
            const int n = budget.next(percent);
            total += n;
            most = std::max(most, n);
            least = std::min(least, n);
        }
        CHECK_EQ(total, 6 * percent);
        CHECK(most - least <= 1);  // evenly spread
    }
    speed::VblankBudget half;
    CHECK_EQ(half.next(150), 1);
    CHECK_EQ(half.next(150), 2);
    CHECK_EQ(half.next(150), 1);
    half.reset();
    CHECK_EQ(half.next(100), 1);
}

TEST_CASE("speed: fast-forward, sound rate, steps and names") {
    CHECK_EQ(speed::effective(200, false, 300), 200);
    CHECK_EQ(speed::effective(200, true, 300), 300);
    CHECK_EQ(speed::effective(100, true, speed::kUnlimited), speed::kUnlimited);
    CHECK(speed::audio_rate(300) == 3.0);
    CHECK(speed::audio_rate(400) == 4.0);
    CHECK(speed::audio_rate(500) == 1.0);
    CHECK(!speed::audible(500));
    CHECK(!speed::audible(speed::kUnlimited));
    CHECK(speed::audible(150));
    CHECK_EQ(speed::cycle(speed::kSpeeds, 400, 1), 100);
    CHECK_EQ(speed::cycle(speed::kSpeeds, 100, -1), 400);
    CHECK_EQ(speed::cycle(speed::kFastForwardSpeeds, 800, 1), speed::kUnlimited);
    CHECK_EQ(speed::cycle(speed::kFastForwardSpeeds, 123, 1), 150);
    CHECK(speed::format(speed::kUnlimited) == "unlimited");
    CHECK(speed::format(300) == "300%");
    CHECK(speed::parse_fast_forward("unlimited") == speed::kUnlimited);
    CHECK(speed::parse_fast_forward("500") == 500);
    CHECK(!speed::parse_fast_forward("250"));
}

TEST_CASE("speed: [player] Speed and FastForwardSpeed and the FastForward binding round-trip") {
    using anypsp::input::Action;
    anypsp::input::InputMap map;
    CHECK_EQ(map.player().speed, 100);
    CHECK_EQ(map.player().fast_forward, speed::kDefaultFastForward);
    CHECK(!map.keys(Action::FastForward).empty());
    CHECK(map.buttons(Action::FastForward) == std::vector<int>{anypsp::input::kRightTrigger});
    map.player().speed = 300;
    map.player().fast_forward = speed::kUnlimited;
    const auto path = std::filesystem::temp_directory_path() / "anypsp-speed-test.ini";
    map.save(path);
    anypsp::input::InputMap again;
    again.load(path);
    CHECK_EQ(again.player().speed, 300);
    CHECK_EQ(again.player().fast_forward, speed::kUnlimited);
    CHECK(again.keys(Action::FastForward) == map.keys(Action::FastForward));
    std::ofstream(path, std::ios::binary) << "[player]\nSpeed = 250\n";
    CHECK_THROWS(again.load(path));
    std::ofstream(path, std::ios::binary) << "[player]\nFastForwardSpeed = 900\n";
    CHECK_THROWS(again.load(path));
    std::filesystem::remove(path);
}
