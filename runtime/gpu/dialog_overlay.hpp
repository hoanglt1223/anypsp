// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The PSP's system dialogs (pspweb::SystemDialog: messages, the savedata list and questions)
// drawn over the running game with anyui in the menu's style (docs/user/desktop.md "System
// dialogs"), and the PSP buttons routed to them. Frame is what to show, already translated;
// View draws it at any picture size; Keys turns held PSP buttons into dialog answers;
// Overlay ties them to the menu's language and theme. No SDL here except the PNG icons.

#include "input_map.hpp"
#include "system_dialog.hpp"
#include "ui/anyui/menu_view.hpp"
#include "ui/anyui/strings.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace anypsp::dialog {

struct Row {
    std::string title, subtitle, detail, date;
    bool new_data{};
    anyui::Thumb thumb;  // ICON0.PNG, may be empty
};

struct Frame {
    anyui::Icon icon{anyui::Icon::None};
    std::string heading;  // "Load", "Save", "Delete"; empty for game messages
    bool list{};          // rows are the choices; else at most one row, the save asked about
    std::vector<Row> rows;
    std::string message;  // may hold line breaks
    std::vector<std::string> buttons;  // "OK", or "Yes" and "No"; none for a list
    int focus{};          // the list row or the button
    bool cross_confirms{true};
    std::string confirm_hint, back_hint;  // beside the Cross and Circle glyphs at the bottom
};

// "2026-10-24 21:05" in local time; empty for 0.
std::string format_time(std::int64_t unix_seconds);
// The frame for `d` in the strings' current language. `icon` gives an ICON0.PNG's pixels.
Frame make_frame(const pspweb::SystemDialog &d, const anyui::Strings &strings,
                 const std::function<anyui::Thumb(const std::filesystem::path &)> &icon);
// `text` broken into lines no wider than `width` at `px`, at spaces and line breaks.
std::vector<std::string> wrap(const anyui::Font &font, const std::string &text, float px, float width);

struct Hit {
    enum class Kind : std::uint8_t { None, Row, Button };
    Kind kind{Kind::None};
    int index{-1};
    bool operator==(const Hit &) const = default;
};

class View {
public:
    View(const anyui::Font &regular, const anyui::Font &bold) : regular_(regular), bold_(bold) {}
    // Draws `f` over the picture in `out` (dimmed). `dt` is the time since the last call, in seconds.
    void render(anyui::Canvas &out, const Frame &f, const anyui::ViewOptions &o, float dt);
    // The next render fades in again.
    void reset() { open_ = 0; }
    [[nodiscard]] Hit hit(float x, float y) const;
    [[nodiscard]] anyui::Rect rect_of(Hit h) const;

private:
    void draw_row(anyui::Canvas &out, const anyui::Theme &t, const Row &r, anyui::Rect rr, float a, bool selected) const;
    float center_baseline(const anyui::Font &font, float cy, float px) const { return cy + (font.ascent(px) - font.descent(px)) / 2; }

    const anyui::Font &regular_;
    const anyui::Font &bold_;
    float open_{};
    float u_{1};
    float scroll_{};
    std::vector<std::pair<anyui::Rect, Hit>> hits_;
};

// The PSP buttons for an open dialog: d-pad moves (repeating while held), the confirm button
// (Cross, or Circle in the Japanese layout) confirms, the other goes back. While a dialog is
// open the game sees no buttons, and buttons still held when it closes stay hidden from the
// game until released, so one press never acts twice.
class Keys {
public:
    struct Out {
        std::uint32_t game{};  // the buttons the game sees
        int move{};            // dialog_move
        bool confirm{}, back{};
    };
    Out update(const pspweb::SystemDialog *d, std::uint32_t held);

private:
    std::uint32_t last_{}, hidden_{};
    int repeat_frames_{};
};

// Loads ICON0.PNG files through SDL once each.
class Icons {
public:
    anyui::Thumb get(const std::filesystem::path &png);
    void clear() { cache_.clear(); }

private:
    struct Picture {
        std::vector<std::uint32_t> pixels;
        int width{}, height{};
    };
    std::map<std::filesystem::path, Picture> cache_;
};

// Draws the open dialog over a picture in the language, theme, UI scale and motion setting
// of the menu (`map`'s [player] settings).
class Overlay {
public:
    explicit Overlay(const input::InputMap &map);
    // `pixels` is w x h anyui pixels (R, G, B, A bytes); the dialog is drawn into them.
    void draw(const pspweb::SystemDialog &d, std::uint32_t *pixels, int w, int h, float dt);
    // No dialog is open: the next one fades in.
    void hide();
    [[nodiscard]] Hit hit(float x, float y) const { return view_.hit(x, y); }
    [[nodiscard]] anyui::ViewOptions look() const;

private:
    const input::InputMap &map_;
    View view_;
    Icons icons_;
    anyui::Canvas canvas_;
    std::uint64_t serial_{};
};

} // namespace anypsp::dialog
