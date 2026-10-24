// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Emulation speed of the desktop player (docs/user/desktop.md "Speed"): a set speed, and a
// fast-forward speed used while the fast-forward key is held. Faster speeds run several PSP
// vblanks per shown picture. No SDL here, so it can be unit tested.

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace anypsp::speed {

inline constexpr int kUnlimited = 0;  // as fast as the host can, sound muted
inline constexpr std::array<int, 4> kSpeeds = {100, 200, 300, 400};
inline constexpr std::array<int, 7> kFastForwardSpeeds = {150, 200, 300, 400, 500, 800, kUnlimited};
inline constexpr int kDefaultFastForward = 300;
// Above this the sound is muted rather than sped up.
inline constexpr int kMaxAudible = 400;

// The speed in effect: the fast-forward speed while its key is held, else the set speed.
[[nodiscard]] constexpr int effective(int speed, bool fast_forward_held, int fast_forward) {
    return fast_forward_held ? fast_forward : speed;
}
[[nodiscard]] constexpr bool audible(int percent) { return percent != kUnlimited && percent <= kMaxAudible; }
// Playback rate of the sound at `percent` (1 at 100%); 1 when it is muted.
[[nodiscard]] constexpr double audio_rate(int percent) { return audible(percent) ? percent / 100.0 : 1.0; }

// The next value of `allowed` in direction `dir`, wrapping around; a value not in the list
// starts from the first one.
[[nodiscard]] int cycle(std::span<const int> allowed, int value, int dir);
// "300%" or "unlimited"; without the percent sign for the settings file.
[[nodiscard]] std::string format(int percent, bool with_percent = true);
// A [player] FastForwardSpeed value ("300", "unlimited"), nullopt when it is not in the list.
[[nodiscard]] std::optional<int> parse_fast_forward(std::string_view text);

// PSP vblanks to run for each shown picture at a speed: 1 at 100%, 3 at 300%, 1 and 2 in
// turn at 150%. The fraction carries over, so the long-run rate is exact.
class VblankBudget {
public:
    [[nodiscard]] int next(int percent);  // percent >= 100; kUnlimited is not paced by this
    void reset() noexcept { carry_ = 0; }

private:
    int carry_ = 0;  // hundredths of a vblank
};

} // namespace anypsp::speed
