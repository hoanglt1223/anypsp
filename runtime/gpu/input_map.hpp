#pragma once

// Keyboard and gamepad bindings for the PSP buttons, the analog stick and the
// window hotkeys, loaded from an ini file (format in docs/user/desktop.md).
// Independent of SDL initialisation so it can be unit tested.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace anypsp::input {

enum class Action {
    Up, Down, Left, Right, Cross, Circle, Square, Triangle, L, R, Start, Select,
    AnalogUp, AnalogDown, AnalogLeft, AnalogRight,
    Fullscreen,
    Smoothing,
    Refresh, Vrr, SyncToMonitor, FpsCap, PauseFps, Pause,
    Count
};

// Gamepad "buttons" beyond SDL_GamepadButton: the triggers, pressed past half travel.
inline constexpr int kLeftTrigger = 1000, kRightTrigger = 1001;

struct PadState {
    std::uint32_t buttons{};
    std::uint8_t lx{128}, ly{128};
};

class InputMap {
public:
    InputMap(); // the defaults

    // Applies a file over the current bindings. A name not in the file keeps its
    // binding. Throws psprecomp::Error naming the file and line on any mistake.
    void load(const std::filesystem::path &path);
    void save(const std::filesystem::path &path) const;
    [[nodiscard]] std::string to_ini() const;

    [[nodiscard]] const std::vector<int> &keys(Action a) const { return keys_[static_cast<std::size_t>(a)]; }
    [[nodiscard]] const std::vector<int> &buttons(Action a) const { return buttons_[static_cast<std::size_t>(a)]; }
    [[nodiscard]] int stick_deadzone() const noexcept { return deadzone_; }
    [[nodiscard]] int scale() const noexcept { return scale_; }

    // True when any key or gamepad button bound to `a` is held. `key` receives an
    // SDL_Scancode, `button` an SDL_GamepadButton or kLeftTrigger/kRightTrigger.
    [[nodiscard]] bool held(Action a, const std::function<bool(int)> &key, const std::function<bool(int)> &button) const;

    // PSP pad from the held bindings and the left stick (SDL axis values,
    // -32768..32767). Digital analog bindings push the stick fully.
    [[nodiscard]] PadState pad(const std::function<bool(int)> &key, const std::function<bool(int)> &button,
                               int stick_x, int stick_y) const;

private:
    std::vector<int> keys_[static_cast<std::size_t>(Action::Count)];
    std::vector<int> buttons_[static_cast<std::size_t>(Action::Count)];
    int deadzone_{8000};
    int scale_{2};
};

// Where an input file is looked up when none is named: next to the program, then
// in the per-user data folder. Returns an empty path when neither exists.
std::filesystem::path find_input_file(const std::filesystem::path &program_dir, const std::filesystem::path &user_dir);
inline constexpr const char *kInputFileName = "anypsp-input.ini";

} // namespace anypsp::input
