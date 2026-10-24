// SPDX-License-Identifier: GPL-2.0-or-later

#include "speed.hpp"

#include <algorithm>

namespace anypsp::speed {

int cycle(std::span<const int> allowed, int value, int dir) {
    const int n = static_cast<int>(allowed.size());
    const auto at = std::find(allowed.begin(), allowed.end(), value);
    if (at == allowed.end()) return allowed.front();
    const int i = static_cast<int>(at - allowed.begin());
    return allowed[static_cast<std::size_t>(((i + dir) % n + n) % n)];
}

std::string format(int percent, bool with_percent) {
    if (percent == kUnlimited) return "unlimited";
    return std::to_string(percent) + (with_percent ? "%" : "");
}

std::optional<int> parse_fast_forward(std::string_view text) {
    for (const int s : kFastForwardSpeeds)
        if (text == format(s, false)) return s;
    return std::nullopt;
}

int VblankBudget::next(int percent) {
    carry_ += std::max(percent, 100);
    const int whole = carry_ / 100;
    carry_ %= 100;
    return whole;
}

} // namespace anypsp::speed
