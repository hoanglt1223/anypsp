// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The in-game menu of the desktop player (docs/user/desktop.md "Menu"): a rail of tabs
// (Game, Saves, Video, Audio, Controls, Settings) and the current tab's page. The game is
// paused while it is open. It is driven by the PSP buttons through the input bindings
// (d-pad, Cross to choose, Circle to go back, L/R for tabs, Select to delete), a few fixed
// keys (Escape, Tab, Page Up/Down) and pointer clicks on what anyui::MenuView drew. Rows are
// identified by string ids; actions tell the host what to do. No SDL here, so it can be
// unit tested.

#include "display.hpp"
#include "input_map.hpp"
#include "ui/anyui/menu_view.hpp"
#include "ui/anyui/strings.hpp"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace anypsp::menu {

struct MenuInput {
    std::uint32_t pad{};       // PSP buttons held (pspweb::pad bits), from the bindings
    std::vector<int> keys;     // SDL scancodes pressed this frame
    std::vector<int> buttons;  // gamepad buttons pressed this frame (SDL_GamepadButton or the triggers)
    bool shift{};
    bool menu{};               // the Menu binding (Escape, F1) or Back+Start was pressed
};

enum class Action : std::uint8_t {
    Close,
    Reset,              // start the game again
    Exit,
    Fullscreen,         // the fullscreen setting changed; apply it
    SettingsChanged,    // bindings or [player] settings changed; save the input file
    DisplayChanged,     // a display setting (anypsp-display.ini) or the patches switch changed
};

// One game save on the memory stick (the Saves tab).
struct SaveInfo {
    std::string title;   // from PARAM.SFO, else the folder name
    std::string detail;  // folder and date
    std::vector<std::uint32_t> icon;  // ICON0.PNG as anyui pixels, may be empty
    int icon_width{}, icon_height{};
};

// The translations (ui/strings.tsv), parsed on first use.
anyui::Strings &strings();
const anyui::Font &regular_font();
const anyui::Font &bold_font();

class Menu {
public:
    // `fullscreen` mirrors the window; `running_scale` is the internal resolution in use.
    Menu(input::InputMap &map, bool &fullscreen, int running_scale)
        : map_(map), fullscreen_(fullscreen), running_scale_(running_scale) {}

    void open();
    void close() { open_ = false; }
    [[nodiscard]] bool is_open() const { return open_; }
    void set_saves(std::vector<SaveInfo> saves) { saves_ = std::move(saves); }
    void set_game_title(std::string title) { game_title_ = std::move(title); }
    // The display settings the Video tab edits; null hides those rows.
    void set_display(display::Settings *settings) { display_ = settings; }
    // Whether the per-game patches apply; null when no patch file is loaded.
    void set_patches(bool *enabled) { patches_ = enabled; }
    void say(std::string text, bool error = false) {
        message_ = std::move(text);
        message_error_ = error;
    }

    std::vector<Action> update(const MenuInput &in);
    std::vector<Action> click(anyui::Hit hit);

    [[nodiscard]] anyui::Frame view() const;
    // The visible text, title first (tests and headless debugging).
    [[nodiscard]] std::vector<std::string> lines() const;

private:
    enum class Page : std::uint8_t { Game, Saves, Video, Audio, Controls, Capture, Settings };
    struct TabInfo {
        Page page;
        const char *id;
        anyui::Icon icon;
    };

    [[nodiscard]] std::vector<TabInfo> tabs() const;
    [[nodiscard]] std::vector<anyui::Row> rows() const;
    [[nodiscard]] std::string title() const;
    [[nodiscard]] std::string hint() const;
    [[nodiscard]] std::string tr(std::string_view key) const;
    [[nodiscard]] std::string fmt(std::string_view key, std::initializer_list<std::string_view> args) const;
    [[nodiscard]] int tab_index() const;
    std::uint32_t pressed_with_repeat(std::uint32_t held);
    void go_tab(int index);
    void move_cursor(int delta);
    void activate(const anyui::Row &row, std::vector<Action> &out);
    void step(const anyui::Row &row, int dir, std::vector<Action> &out);
    void update_capture(const MenuInput &in, std::vector<Action> &out);
    void close_menu(std::vector<Action> &out);

    input::InputMap &map_;
    bool &fullscreen_;
    int running_scale_;
    std::vector<SaveInfo> saves_;
    std::string game_title_;
    display::Settings *display_{};
    bool *patches_{};

    bool open_{};
    Page page_{Page::Game};
    bool rail_{};
    int cursor_{};
    std::string confirm_;
    input::Action capture_{input::Action::Cross};
    std::string message_;
    bool message_error_{};
    std::uint32_t last_held_{};
    bool ignore_held_{};
    int repeat_frames_{};
};

} // namespace anypsp::menu
