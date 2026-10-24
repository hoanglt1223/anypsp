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
    Menu,
    Count
};

// The [player] settings the in-game menu changes (besides Scale and StickDeadzone).
struct PlayerSettings {
    int volume{100};          // Volume: percent, 0-100 in steps of 10
    bool mute{};              // Mute = on|off
    bool show_fps{};          // ShowFps = on|off: frame rates in the corner of the picture
    bool sharp{};             // Filter = smooth|sharp: how the picture is scaled to the window
    std::string language{"en"};  // Language = en|vi (the menu)
    bool light_theme{};       // Theme = dark|light
    int ui_scale{100};        // UiScale = 75|100|125|150 (the menu's size in percent)
    bool reduce_motion{};     // ReduceMotion = on|off: the menu does not animate
    bool operator==(const PlayerSettings &) const = default;
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
    void set_scale(int scale) { scale_ = scale < 1 ? 1 : scale > 4 ? 4 : scale; }
    [[nodiscard]] const PlayerSettings &player() const noexcept { return player_; }
    [[nodiscard]] PlayerSettings &player() noexcept { return player_; }

    // Binds one key (SDL_Scancode) or gamepad button to `a`, replacing its bindings on that
    // device. An action that had the same key or button gets `a`'s previous one instead (a
    // swap) or none; its name is returned, or an empty string.
    std::string rebind(Action a, bool keyboard, int code);
    // The default keyboard and gamepad bindings; the [player] settings stay.
    void reset_bindings();

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
    PlayerSettings player_;
};

[[nodiscard]] const char *action_name(Action a);
// SDL key name of a scancode, gamepad button name ("South", "LeftTrigger"...).
[[nodiscard]] std::string key_name(int scancode);
[[nodiscard]] const char *button_name(int button);

// Where an input file is looked up when none is named: next to the program, then
// in the per-user data folder. Returns an empty path when neither exists.
std::filesystem::path find_input_file(const std::filesystem::path &program_dir, const std::filesystem::path &user_dir);
inline constexpr const char *kInputFileName = "anypsp-input.ini";

} // namespace anypsp::input
