// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The in-game menu in the desktop window: runs menu::Menu over a frosted copy of the game
// picture while the game waits, drawn with anyui at the window's resolution, and the
// frame-rate badge drawn over the game when Show frame rates is on.

#include "menu.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace anypsp::gpu {
class GlHost;
}
namespace anypsp::desktop {
class Controls;
}

namespace anypsp::menu {

// A PARAM.SFO string value ("TITLE", "DISC_ID", "SAVEDATA_TITLE"...), empty when the file or
// key is missing or malformed.
std::string sfo_string(const std::filesystem::path &sfo, const std::string &key);
// The saves in <memstick>/PSP/SAVEDATA whose folder starts with `game_id` (every save when it
// is empty), newest first, with PARAM.SFO titles and ICON0.PNG icons.
std::vector<SaveInfo> scan_saves(const std::filesystem::path &memstick, const std::string &game_id);
// A PNG (ICON0.PNG) as opaque anyui pixels, transparent parts on black; false when it cannot be read.
bool load_icon_pixels(const std::filesystem::path &png, std::vector<std::uint32_t> &pixels, int &width, int &height);

// The Video preview's pictures for a w x h window: the game picture (game_w x game_h) enlarged
// to the size it takes in the window, nearest (before) and as Scaling shows it (after).
struct PreviewPictures {
    std::vector<std::uint32_t> before, after;
    int width{}, height{};
    void make(const std::uint32_t *game, int game_w, int game_h, int w, int h, bool sharp);
    void attach(anyui::Preview &preview) const {
        preview.before = {before.data(), width, height};
        preview.after = {after.data(), width, height};
    }
};

class MenuSession {
public:
    enum class Result { Resume, Exit, Reset, WindowClosed };

    // `ini` is where changed bindings and settings are saved. The menu edits the bindings
    // and [player] settings of `controls`.
    MenuSession(gpu::GlHost &gl, desktop::Controls &controls, std::filesystem::path ini, std::filesystem::path memstick,
                std::string game_id, std::string title);
    ~MenuSession();
    MenuSession(const MenuSession &) = delete;
    MenuSession &operator=(const MenuSession &) = delete;

    // Runs the menu until it closes, over the framebuffer at `address`. The game waits.
    Result run(std::uint32_t address, std::uint32_t stride, std::uint32_t format);
    // Rates for the badge: vblanks per second and frames the game finished per second.
    void set_rates(double hz, double fps);
    // The speed in effect (percent, speed::kUnlimited), shown in the badge when it is not 100%.
    void set_speed(int percent);
    // The display settings the Video tab edits (its Menu FPS paces the menu); `changed` runs
    // after each edit.
    void set_display(display::Settings *settings, std::function<void()> changed);
    // Whether the per-game patches apply; null when no patch file is loaded.
    void set_patches(bool *enabled);

private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace anypsp::menu
