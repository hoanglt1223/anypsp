// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The in-game menu in the desktop window: runs menu::Menu over a frosted copy of the game
// picture while the game waits, drawn with anyui at the window's resolution, and the
// frame-rate badge drawn over the game when Show frame rates is on.

#include "menu.hpp"

#include <cstdint>
#include <filesystem>
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

private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace anypsp::menu
