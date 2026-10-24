// SPDX-License-Identifier: MIT
// anyui (shared by AnyGBA and AnyPSP; keep the copies identical): line icons drawn in code
// at any size, so they stay sharp at every resolution.
#pragma once

#include "canvas.hpp"

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace anyui {

enum class Icon : std::uint8_t {
    None, Game, States, Video, Audio, Controls, Cheats, Ai, Settings,
    Resume, Reset, Exit, Save, Load, Delete, Search, Folder, Speed, Info, Plus, Back, Close,
    ChevronRight, Check, Language, Theme, Mute, Card, Keyboard,
};

namespace detail {

inline std::vector<Point> circle_points(float cx, float cy, float r, int n, bool reverse = false) {
    std::vector<Point> p;
    for (int i = 0; i < n; ++i) {
        const float a = (reverse ? -1.f : 1.f) * 2 * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(n);
        p.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
    }
    return p;
}

inline void arc(Canvas& c, float cx, float cy, float r, float from, float to, float w, Color col) {
    const int n = std::max(6, static_cast<int>(std::abs(to - from) * r / 3));
    for (int i = 0; i < n; ++i) {
        const float a0 = from + (to - from) * static_cast<float>(i) / static_cast<float>(n);
        const float a1 = from + (to - from) * static_cast<float>(i + 1) / static_cast<float>(n);
        c.line(cx + r * std::cos(a0), cy + r * std::sin(a0), cx + r * std::cos(a1), cy + r * std::sin(a1), w, col);
    }
}

inline void gamepad(Canvas& c, float x, float y, float s, float w, Color col) {
    const Rect body{x - s * 0.46f, y - s * 0.24f, s * 0.92f, s * 0.48f};
    c.stroke_round_rect(body, s * 0.22f, w, col);
    c.line(x - s * 0.28f, y, x - s * 0.12f, y, w, col);
    c.line(x - s * 0.20f, y - s * 0.08f, x - s * 0.20f, y + s * 0.08f, w, col);
    c.fill_circle(x + s * 0.16f, y + s * 0.03f, s * 0.045f, col);
    c.fill_circle(x + s * 0.27f, y - s * 0.07f, s * 0.045f, col);
}

}  // namespace detail

// Draws `icon` centred on (x, y) in a box `s` pixels wide.
inline void draw_icon(Canvas& c, Icon icon, float x, float y, float s, Color col) {
    using namespace detail;
    constexpr float pi = std::numbers::pi_v<float>;
    const float w = std::max(1.2f, s * 0.085f);
    switch (icon) {
    case Icon::None: break;
    case Icon::Game:
        c.stroke_circle(x, y, s * 0.42f, w, col);
        c.fill_polygons({{{x - s * 0.12f, y - s * 0.19f}, {x + s * 0.2f, y}, {x - s * 0.12f, y + s * 0.19f}}}, col);
        break;
    case Icon::Resume:
        c.fill_polygons({{{x - s * 0.22f, y - s * 0.32f}, {x + s * 0.32f, y}, {x - s * 0.22f, y + s * 0.32f}}}, col);
        break;
    case Icon::States:
    case Icon::Save: {
        const Rect r{x - s * 0.36f, y - s * 0.36f, s * 0.72f, s * 0.72f};
        c.stroke_round_rect(r, s * 0.08f, w, col);
        c.fill_round_rect({x - s * 0.18f, y - s * 0.36f, s * 0.36f, s * 0.22f}, s * 0.03f, col);
        c.stroke_round_rect({x - s * 0.22f, y + s * 0.04f, s * 0.44f, s * 0.32f}, s * 0.04f, w, col);
        break;
    }
    case Icon::Load:
        c.line(x, y + s * 0.2f, x, y - s * 0.3f, w, col);
        c.line(x - s * 0.18f, y - s * 0.12f, x, y - s * 0.3f, w, col);
        c.line(x + s * 0.18f, y - s * 0.12f, x, y - s * 0.3f, w, col);
        c.line(x - s * 0.34f, y + s * 0.34f, x + s * 0.34f, y + s * 0.34f, w, col);
        break;
    case Icon::Video:
        c.stroke_round_rect({x - s * 0.42f, y - s * 0.32f, s * 0.84f, s * 0.52f}, s * 0.07f, w, col);
        c.line(x - s * 0.18f, y + s * 0.36f, x + s * 0.18f, y + s * 0.36f, w, col);
        c.line(x, y + s * 0.2f, x, y + s * 0.36f, w, col);
        break;
    case Icon::Audio:
    case Icon::Mute:
        c.fill_polygons({{{x - s * 0.4f, y - s * 0.12f}, {x - s * 0.22f, y - s * 0.12f}, {x + s * 0.02f, y - s * 0.34f},
                          {x + s * 0.02f, y + s * 0.34f}, {x - s * 0.22f, y + s * 0.12f}, {x - s * 0.4f, y + s * 0.12f}}},
                        col);
        if (icon == Icon::Audio) {
            arc(c, x + s * 0.04f, y, s * 0.18f, -pi / 4, pi / 4, w, col);
            arc(c, x + s * 0.04f, y, s * 0.34f, -pi / 4, pi / 4, w, col);
        } else {
            c.line(x + s * 0.14f, y - s * 0.14f, x + s * 0.4f, y + s * 0.14f, w, col);
            c.line(x + s * 0.14f, y + s * 0.14f, x + s * 0.4f, y - s * 0.14f, w, col);
        }
        break;
    case Icon::Controls: gamepad(c, x, y, s, w, col); break;
    case Icon::Keyboard:
        c.stroke_round_rect({x - s * 0.44f, y - s * 0.26f, s * 0.88f, s * 0.52f}, s * 0.07f, w, col);
        for (int i = 0; i < 4; ++i) c.fill_circle(x - s * 0.24f + static_cast<float>(i) * s * 0.16f, y - s * 0.08f, s * 0.04f, col);
        c.line(x - s * 0.2f, y + s * 0.1f, x + s * 0.2f, y + s * 0.1f, w, col);
        break;
    case Icon::Cheats: {
        // A four-pointed sparkle and a small one.
        const auto star = [&](float cx, float cy, float r) {
            const float k = r * 0.28f;
            c.fill_polygons({{{cx, cy - r}, {cx + k, cy - k}, {cx + r, cy}, {cx + k, cy + k}, {cx, cy + r}, {cx - k, cy + k},
                              {cx - r, cy}, {cx - k, cy - k}}},
                            col);
        };
        star(x - s * 0.06f, y + s * 0.06f, s * 0.36f);
        star(x + s * 0.28f, y - s * 0.28f, s * 0.14f);
        break;
    }
    case Icon::Ai:
        c.stroke_round_rect({x - s * 0.34f, y - s * 0.24f, s * 0.68f, s * 0.54f}, s * 0.14f, w, col);
        c.fill_circle(x - s * 0.13f, y + s * 0.02f, s * 0.06f, col);
        c.fill_circle(x + s * 0.13f, y + s * 0.02f, s * 0.06f, col);
        c.line(x, y - s * 0.24f, x, y - s * 0.38f, w, col);
        c.fill_circle(x, y - s * 0.4f, s * 0.06f, col);
        break;
    case Icon::Settings: {
        std::vector<Point> gear;
        const int teeth = 8;
        for (int i = 0; i < teeth * 4; ++i) {
            const float a = 2 * pi * (static_cast<float>(i) - 0.5f) / static_cast<float>(teeth * 4);
            const float r = (i % 4 < 2) ? s * 0.42f : s * 0.31f;
            gear.push_back({x + r * std::cos(a), y + r * std::sin(a)});
        }
        c.fill_polygons({gear, circle_points(x, y, s * 0.14f, 20, true)}, col);
        break;
    }
    case Icon::Reset:
        arc(c, x, y, s * 0.32f, -pi * 0.35f, pi * 1.4f, w, col);
        c.fill_polygons({{{x + s * 0.1f, y - s * 0.44f}, {x + s * 0.34f, y - s * 0.3f}, {x + s * 0.08f, y - s * 0.12f}}}, col);
        break;
    case Icon::Exit:
        arc(c, x, y + s * 0.03f, s * 0.32f, -pi * 0.3f, pi * 1.3f, w, col);
        c.line(x, y - s * 0.42f, x, y - s * 0.02f, w, col);
        break;
    case Icon::Delete:
        c.line(x - s * 0.34f, y - s * 0.26f, x + s * 0.34f, y - s * 0.26f, w, col);
        c.line(x - s * 0.1f, y - s * 0.36f, x + s * 0.1f, y - s * 0.36f, w, col);
        c.stroke_round_rect({x - s * 0.24f, y - s * 0.2f, s * 0.48f, s * 0.58f}, s * 0.06f, w, col);
        c.line(x - s * 0.07f, y - s * 0.06f, x - s * 0.07f, y + s * 0.24f, w, col);
        c.line(x + s * 0.07f, y - s * 0.06f, x + s * 0.07f, y + s * 0.24f, w, col);
        break;
    case Icon::Search:
        c.stroke_circle(x - s * 0.06f, y - s * 0.06f, s * 0.26f, w, col);
        c.line(x + s * 0.13f, y + s * 0.13f, x + s * 0.36f, y + s * 0.36f, w * 1.2f, col);
        break;
    case Icon::Folder:
        c.stroke_round_rect({x - s * 0.42f, y - s * 0.24f, s * 0.84f, s * 0.6f}, s * 0.06f, w, col);
        c.fill_round_rect({x - s * 0.42f, y - s * 0.34f, s * 0.36f, s * 0.16f}, s * 0.05f, col);
        break;
    case Icon::Speed:
        c.fill_polygons({{{x - s * 0.4f, y - s * 0.28f}, {x, y}, {x - s * 0.4f, y + s * 0.28f}},
                         {{x, y - s * 0.28f}, {x + s * 0.4f, y}, {x, y + s * 0.28f}}},
                        col);
        break;
    case Icon::Info:
        c.stroke_circle(x, y, s * 0.4f, w, col);
        c.line(x, y - s * 0.04f, x, y + s * 0.2f, w * 1.1f, col);
        c.fill_circle(x, y - s * 0.18f, w * 0.7f, col);
        break;
    case Icon::Plus:
        c.line(x - s * 0.3f, y, x + s * 0.3f, y, w * 1.2f, col);
        c.line(x, y - s * 0.3f, x, y + s * 0.3f, w * 1.2f, col);
        break;
    case Icon::Back:
        c.line(x + s * 0.1f, y - s * 0.24f, x - s * 0.14f, y, w * 1.2f, col);
        c.line(x - s * 0.14f, y, x + s * 0.1f, y + s * 0.24f, w * 1.2f, col);
        break;
    case Icon::ChevronRight:
        c.line(x - s * 0.1f, y - s * 0.24f, x + s * 0.14f, y, w * 1.2f, col);
        c.line(x + s * 0.14f, y, x - s * 0.1f, y + s * 0.24f, w * 1.2f, col);
        break;
    case Icon::Close:
        c.line(x - s * 0.26f, y - s * 0.26f, x + s * 0.26f, y + s * 0.26f, w * 1.2f, col);
        c.line(x - s * 0.26f, y + s * 0.26f, x + s * 0.26f, y - s * 0.26f, w * 1.2f, col);
        break;
    case Icon::Check:
        c.line(x - s * 0.3f, y, x - s * 0.08f, y + s * 0.22f, w * 1.3f, col);
        c.line(x - s * 0.08f, y + s * 0.22f, x + s * 0.32f, y - s * 0.24f, w * 1.3f, col);
        break;
    case Icon::Language:
        c.stroke_circle(x, y, s * 0.4f, w, col);
        c.line(x - s * 0.4f, y, x + s * 0.4f, y, w * 0.8f, col);
        arc(c, x + s * 0.32f, y, s * 0.42f, pi * 0.72f, pi * 1.28f, w * 0.8f, col);
        arc(c, x - s * 0.32f, y, s * 0.42f, -pi * 0.28f, pi * 0.28f, w * 0.8f, col);
        break;
    case Icon::Theme:
        c.fill_circle(x, y, s * 0.18f, col);
        for (int i = 0; i < 8; ++i) {
            const float a = 2 * pi * static_cast<float>(i) / 8;
            c.line(x + s * 0.28f * std::cos(a), y + s * 0.28f * std::sin(a), x + s * 0.4f * std::cos(a),
                   y + s * 0.4f * std::sin(a), w, col);
        }
        break;
    case Icon::Card:
        c.stroke_round_rect({x - s * 0.28f, y - s * 0.4f, s * 0.56f, s * 0.8f}, s * 0.06f, w, col);
        for (int i = 0; i < 3; ++i) c.line(x - s * 0.12f + static_cast<float>(i) * s * 0.12f, y - s * 0.4f, x - s * 0.12f + static_cast<float>(i) * s * 0.12f, y - s * 0.22f, w * 0.8f, col);
        break;
    }
}

}  // namespace anyui
