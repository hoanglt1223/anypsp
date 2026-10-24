// SPDX-License-Identifier: GPL-2.0-or-later

#include "input_map.hpp"

#include "kernel.hpp"

#include "psprecomp/common.hpp"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>

namespace anypsp::input {
namespace fs = std::filesystem;
namespace {

constexpr std::size_t kCount = static_cast<std::size_t>(Action::Count);

constexpr std::array<const char *, kCount> kActionNames = {
    "Up", "Down", "Left", "Right", "Cross", "Circle", "Square", "Triangle", "L", "R", "Start", "Select",
    "AnalogUp", "AnalogDown", "AnalogLeft", "AnalogRight", "Fullscreen", "Smoothing",
    "Refresh", "VRR", "SyncToMonitor", "FpsCap", "PauseFps", "Pause"};

constexpr std::array<std::uint32_t, 12> kPadBits = {
    pspweb::pad::kUp, pspweb::pad::kDown, pspweb::pad::kLeft, pspweb::pad::kRight, pspweb::pad::kCross,
    pspweb::pad::kCircle, pspweb::pad::kSquare, pspweb::pad::kTriangle, pspweb::pad::kL, pspweb::pad::kR,
    pspweb::pad::kStart, pspweb::pad::kSelect};

const std::pair<const char *, int> kButtonNames[] = {
    {"South", SDL_GAMEPAD_BUTTON_SOUTH}, {"East", SDL_GAMEPAD_BUTTON_EAST}, {"West", SDL_GAMEPAD_BUTTON_WEST},
    {"North", SDL_GAMEPAD_BUTTON_NORTH}, {"Back", SDL_GAMEPAD_BUTTON_BACK}, {"Guide", SDL_GAMEPAD_BUTTON_GUIDE},
    {"Start", SDL_GAMEPAD_BUTTON_START}, {"LeftStick", SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"RightStick", SDL_GAMEPAD_BUTTON_RIGHT_STICK}, {"LeftShoulder", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"RightShoulder", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {"DpadUp", SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"DpadDown", SDL_GAMEPAD_BUTTON_DPAD_DOWN}, {"DpadLeft", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"DpadRight", SDL_GAMEPAD_BUTTON_DPAD_RIGHT}, {"LeftTrigger", kLeftTrigger}, {"RightTrigger", kRightTrigger}};

std::string trim(const std::string &s) {
    const auto first = s.find_first_not_of(" \t\r");
    const auto last = s.find_last_not_of(" \t\r");
    return first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
}

bool same(const std::string &a, const std::string &b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

int button_from_name(const std::string &name) {
    for (const auto &[n, b] : kButtonNames)
        if (same(n, name)) return b;
    return -1;
}

const char *button_name(int button) {
    for (const auto &[n, b] : kButtonNames)
        if (b == button) return n;
    return "?";
}

std::uint8_t axis_to_pad(int value, int deadzone) {
    if (value > -deadzone && value < deadzone) return 128u;
    const int scaled = (std::clamp(value, -32768, 32767) + 32768) >> 8; // 0..255
    return static_cast<std::uint8_t>(scaled);
}

} // namespace

InputMap::InputMap() {
    const auto bind = [this](Action a, std::initializer_list<int> keys, std::initializer_list<int> buttons) {
        keys_[static_cast<std::size_t>(a)] = keys;
        buttons_[static_cast<std::size_t>(a)] = buttons;
    };
    bind(Action::Up, {SDL_SCANCODE_UP}, {SDL_GAMEPAD_BUTTON_DPAD_UP});
    bind(Action::Down, {SDL_SCANCODE_DOWN}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN});
    bind(Action::Left, {SDL_SCANCODE_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT});
    bind(Action::Right, {SDL_SCANCODE_RIGHT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT});
    bind(Action::Cross, {SDL_SCANCODE_Z}, {SDL_GAMEPAD_BUTTON_SOUTH});
    bind(Action::Circle, {SDL_SCANCODE_X}, {SDL_GAMEPAD_BUTTON_EAST});
    bind(Action::Square, {SDL_SCANCODE_A}, {SDL_GAMEPAD_BUTTON_WEST});
    bind(Action::Triangle, {SDL_SCANCODE_S}, {SDL_GAMEPAD_BUTTON_NORTH});
    bind(Action::L, {SDL_SCANCODE_Q}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER});
    bind(Action::R, {SDL_SCANCODE_W}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER});
    bind(Action::Start, {SDL_SCANCODE_RETURN}, {SDL_GAMEPAD_BUTTON_START});
    bind(Action::Select, {SDL_SCANCODE_BACKSPACE}, {SDL_GAMEPAD_BUTTON_BACK});
    bind(Action::AnalogUp, {SDL_SCANCODE_I}, {});
    bind(Action::AnalogDown, {SDL_SCANCODE_K}, {});
    bind(Action::AnalogLeft, {SDL_SCANCODE_J}, {});
    bind(Action::AnalogRight, {SDL_SCANCODE_L}, {});
    bind(Action::Fullscreen, {SDL_SCANCODE_F11}, {});
    bind(Action::Smoothing, {SDL_SCANCODE_F9}, {});
    bind(Action::Refresh, {SDL_SCANCODE_F5}, {});
    bind(Action::Vrr, {SDL_SCANCODE_F6}, {});
    bind(Action::SyncToMonitor, {SDL_SCANCODE_F7}, {});
    bind(Action::FpsCap, {SDL_SCANCODE_F8}, {});
    bind(Action::PauseFps, {SDL_SCANCODE_F4}, {});
    bind(Action::Pause, {SDL_SCANCODE_PAUSE}, {});
}

void InputMap::load(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw psprecomp::Error("Cannot open " + path.string());
    std::string section, line;
    for (int number = 1; std::getline(in, line); ++number) {
        const auto where = [&] { return path.string() + ":" + std::to_string(number) + ": "; };
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[') {
            if (line.back() != ']') throw psprecomp::Error(where() + "unterminated section");
            section = trim(line.substr(1, line.size() - 2));
            if (!same(section, "keyboard") && !same(section, "gamepad") && !same(section, "player"))
                throw psprecomp::Error(where() + "unknown section [" + section + "]");
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) throw psprecomp::Error(where() + "expected name = value");
        if (section.empty()) throw psprecomp::Error(where() + "binding outside a section");
        const std::string name = trim(line.substr(0, equals)), value = trim(line.substr(equals + 1));

        if (same(section, "player")) {
            int number_value = 0;
            try {
                std::size_t used = 0;
                number_value = std::stoi(value, &used);
                if (used != value.size()) throw std::invalid_argument(value);
            } catch (const std::logic_error &) {
                throw psprecomp::Error(where() + "'" + value + "' is not a number");
            }
            if (same(name, "StickDeadzone")) {
                if (number_value < 0 || number_value > 32767) throw psprecomp::Error(where() + "StickDeadzone is 0-32767");
                deadzone_ = number_value;
            } else if (same(name, "Scale")) {
                if (number_value < 1 || number_value > 4) throw psprecomp::Error(where() + "Scale is 1-4");
                scale_ = number_value;
            } else {
                throw psprecomp::Error(where() + "unknown setting " + name);
            }
            continue;
        }

        std::size_t action = kCount;
        for (std::size_t i = 0; i < kCount; ++i)
            if (same(kActionNames[i], name)) action = i;
        if (action == kCount) throw psprecomp::Error(where() + "unknown name " + name);
        const bool keyboard = same(section, "keyboard");
        std::vector<int> bound;
        std::stringstream items(value);
        for (std::string item; std::getline(items, item, ',');) {
            item = trim(item);
            if (item.empty()) continue;
            if (keyboard) {
                const SDL_Scancode code = SDL_GetScancodeFromName(item.c_str());
                if (code == SDL_SCANCODE_UNKNOWN) throw psprecomp::Error(where() + "unknown key " + item);
                bound.push_back(static_cast<int>(code));
            } else {
                const int button = button_from_name(item);
                if (button < 0) throw psprecomp::Error(where() + "unknown gamepad button " + item);
                bound.push_back(button);
            }
        }
        (keyboard ? keys_ : buttons_)[action] = std::move(bound);
    }
}

std::string InputMap::to_ini() const {
    std::string out = "# AnyPSP input bindings. Keys are SDL key names; gamepad buttons are positional.\n[keyboard]\n";
    for (std::size_t i = 0; i < kCount; ++i) {
        out += std::string(kActionNames[i]) + " =";
        for (std::size_t k = 0; k < keys_[i].size(); ++k)
            out += std::string(k ? ", " : " ") + SDL_GetScancodeName(static_cast<SDL_Scancode>(keys_[i][k]));
        out += "\n";
    }
    out += "\n[gamepad]\n";
    for (std::size_t i = 0; i < kCount; ++i) {
        out += std::string(kActionNames[i]) + " =";
        for (std::size_t k = 0; k < buttons_[i].size(); ++k) out += std::string(k ? ", " : " ") + button_name(buttons_[i][k]);
        out += "\n";
    }
    out += "\n[player]\n# Internal resolution, 1-4 times 480x272 (--scale overrides).\nScale = " + std::to_string(scale_) +
           "\n# Left stick travel ignored around the centre (0-32767).\nStickDeadzone = " + std::to_string(deadzone_) + "\n";
    return out;
}

void InputMap::save(const fs::path &path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << to_ini();
    if (!out) throw psprecomp::Error("Cannot write " + path.string());
}

bool InputMap::held(Action a, const std::function<bool(int)> &key, const std::function<bool(int)> &button) const {
    const auto i = static_cast<std::size_t>(a);
    return std::any_of(keys_[i].begin(), keys_[i].end(), key) || std::any_of(buttons_[i].begin(), buttons_[i].end(), button);
}

PadState InputMap::pad(const std::function<bool(int)> &key, const std::function<bool(int)> &button, int stick_x,
                       int stick_y) const {
    PadState state;
    for (std::size_t i = 0; i < kPadBits.size(); ++i)
        if (held(static_cast<Action>(i), key, button)) state.buttons |= kPadBits[i];
    state.lx = axis_to_pad(stick_x, deadzone_);
    state.ly = axis_to_pad(stick_y, deadzone_);
    if (held(Action::AnalogLeft, key, button)) state.lx = 0u;
    if (held(Action::AnalogRight, key, button)) state.lx = 255u;
    if (held(Action::AnalogUp, key, button)) state.ly = 0u;
    if (held(Action::AnalogDown, key, button)) state.ly = 255u;
    return state;
}

fs::path find_input_file(const fs::path &program_dir, const fs::path &user_dir) {
    for (const fs::path &dir : {program_dir, user_dir}) {
        if (dir.empty()) continue;
        const fs::path candidate = dir / kInputFileName;
        if (fs::is_regular_file(candidate)) return candidate;
    }
    return {};
}

} // namespace anypsp::input
