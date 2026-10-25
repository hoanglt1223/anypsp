// SPDX-License-Identifier: GPL-2.0-or-later
// The in-game menu without a window: scripted navigation, rebinding, settings in the input
// file, the Saves tab's PARAM.SFO reader, and golden pictures of every tab at 1x (960x540)
// and 2x (1920x1080) in the dark and light themes, drawn offscreen, and the system dialogs drawn
// over the game (dialog_overlay.hpp).
// ANYPSP_UPDATE_GOLDENS=1 rewrites tests/golden/menu; ANYPSP_MENU_SHOTS=<dir> also writes
// every rendered picture there.

#include "test_harness.hpp"

#include "desktop.hpp"
#include "dialog_overlay.hpp"
#include "frame_output.hpp"
#include "gl_host.hpp"
#include "input_map.hpp"
#include "kernel.hpp"
#include "menu.hpp"
#include "menu_host.hpp"

#include "psprecomp/guest_memory.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_surface.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using anypsp::input::Action;
using anypsp::input::InputMap;
using anypsp::menu::Menu;
using anypsp::menu::MenuInput;
namespace pad = pspweb::pad;

namespace {

fs::path scratch(const std::string &name) {
    const fs::path dir = fs::temp_directory_path() / ("anypsp-menu-tests-" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

// One host frame with nothing held, then one with `buttons` (edge detection needs both).
std::vector<anypsp::menu::Action> press(Menu &menu, std::uint32_t buttons) {
    menu.update({});
    MenuInput in;
    in.pad = buttons;
    return menu.update(in);
}

std::vector<anypsp::menu::Action> key(Menu &menu, int scancode) {
    MenuInput in;
    in.keys = {scancode};
    return menu.update(in);
}

// A PARAM.SFO with string entries, as the PSP writes them.
void write_sfo(const fs::path &path, const std::vector<std::pair<std::string, std::string>> &entries) {
    std::string keys, data;
    std::vector<std::array<std::uint32_t, 4>> index;
    for (const auto &[k, v] : entries) {
        const std::uint32_t size = static_cast<std::uint32_t>(v.size() + 1);
        const std::uint32_t max = (size + 3u) & ~3u;
        index.push_back({static_cast<std::uint32_t>(keys.size()), size, max, static_cast<std::uint32_t>(data.size())});
        keys += k + '\0';
        data += v + std::string(max - v.size(), '\0');
    }
    while (keys.size() % 4) keys += '\0';
    std::string out("\0PSF", 4);
    const auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out += static_cast<char>((v >> (8 * i)) & 0xFFu);
    };
    const auto u16 = [&](std::uint32_t v) {
        out += static_cast<char>(v & 0xFFu);
        out += static_cast<char>((v >> 8) & 0xFFu);
    };
    const std::uint32_t key_table = 20u + 16u * static_cast<std::uint32_t>(entries.size());
    u32(0x0101u);
    u32(key_table);
    u32(key_table + static_cast<std::uint32_t>(keys.size()));
    u32(static_cast<std::uint32_t>(entries.size()));
    for (const auto &e : index) {
        u16(e[0]);
        u16(0x0204u);
        u32(e[1]);
        u32(e[2]);
        u32(e[3]);
    }
    out += keys + data;
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << out;
}

bool approx_equal(float a, float b) { return std::abs(a - b) < 0.011f; }

anyui::Canvas scene(int w, int h);

// The menu's frame with the Video preview's pictures made as MenuSession makes them, from a
// made-up game picture at 1x.
anyui::Frame with_preview(const Menu &menu, const InputMap &map, int w, int h) {
    static const anyui::Canvas game = scene(480, 272);
    static anypsp::menu::PreviewPictures pictures;
    static std::uint64_t version = 0;
    anyui::Frame f = menu.view();
    if (f.page.preview.shown) {
        pictures.make(game.data(), 480, 272, w, h, map.player().sharp);
        pictures.attach(f.page.preview);
        f.page.preview.version = ++version;
    }
    return f;
}

// A made-up game scene for the backdrop.
anyui::Canvas scene(int w, int h) {
    anyui::Canvas c(w, h);
    for (int y = 0; y < h; ++y) {
        const std::uint32_t px = y < h / 2 ? anyui::pack(0.3f, 0.35f, 0.6f) : anyui::pack(0.55f, 0.4f, 0.3f);
        std::fill(c.row(y), c.row(y) + w, px);
    }
    const float u = static_cast<float>(h) / 272.f;
    c.fill_circle(380 * u, 60 * u, 28 * u, anyui::Color::rgb(0xF4E27A));
    c.fill_round_rect({60 * u, 150 * u, 120 * u, 90 * u}, 12 * u, anyui::Color::rgb(0x2E8B57));
    c.fill_round_rect({260 * u, 170 * u, 150 * u, 70 * u}, 12 * u, anyui::Color::rgb(0xB0413E));
    return c;
}

anyui::Canvas halve(const anyui::Canvas &c) {
    anyui::Canvas h(c.width() / 2, c.height() / 2);
    for (int y = 0; y < h.height(); ++y)
        for (int x = 0; x < h.width(); ++x) {
            unsigned sum[3] = {0, 0, 0};
            for (int k = 0; k < 4; ++k) {
                const std::uint32_t p = c.row(2 * y + k / 2)[2 * x + k % 2];
                for (int ch = 0; ch < 3; ++ch) sum[ch] += (p >> (8 * ch)) & 0xFFu;
            }
            h.row(y)[x] = (sum[0] + 2) / 4 | ((sum[1] + 2) / 4) << 8 | ((sum[2] + 2) / 4) << 16 | 0xFF000000u;
        }
    return h;
}

std::vector<std::uint8_t> bytes(const anyui::Canvas &c) {
    std::vector<std::uint8_t> out(c.pixels().size() * 4u);
    std::memcpy(out.data(), c.data(), out.size());
    return out;
}

// Mean squared error of at most 1 per channel against the golden picture (2x pictures are
// compared at half size, which keeps the goldens small).
void check_golden(const std::string &name, const anyui::Canvas &full) {
    if (const char *shots = std::getenv("ANYPSP_MENU_SHOTS"); shots != nullptr && *shots != '\0') {
        fs::create_directories(shots);
        anypsp::gpu::write_png((fs::path(shots) / (name + ".png")).string(), bytes(full), full.width(), full.height());
    }
    const anyui::Canvas c = full.width() > 1000 ? halve(full) : full;
    const fs::path path = fs::path(ANYPSP_MENU_GOLDEN_DIR) / (name + ".png");
    if (const char *update = std::getenv("ANYPSP_UPDATE_GOLDENS"); update != nullptr && std::string(update) == "1") {
        fs::create_directories(path.parent_path());
        anypsp::gpu::write_png(path.string(), bytes(c), c.width(), c.height());
        return;
    }
    SDL_Surface *loaded = SDL_LoadPNG(path.string().c_str());
    if (loaded == nullptr) anypsp_test::fail(__FILE__, __LINE__, "missing golden " + path.string() + " (ANYPSP_UPDATE_GOLDENS=1)");
    SDL_Surface *rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_ABGR8888);
    SDL_DestroySurface(loaded);
    CHECK(rgba != nullptr && rgba->w == c.width() && rgba->h == c.height());
    double sum = 0;
    for (int y = 0; y < c.height(); ++y) {
        const auto *row = reinterpret_cast<const std::uint32_t *>(static_cast<const std::uint8_t *>(rgba->pixels) + y * rgba->pitch);
        for (int x = 0; x < c.width(); ++x)
            for (int ch = 0; ch < 3; ++ch) {
                const double d = static_cast<double>((c.row(y)[x] >> (8 * ch)) & 0xFFu) - static_cast<double>((row[x] >> (8 * ch)) & 0xFFu);
                sum += d * d;
            }
    }
    SDL_DestroySurface(rgba);
    const double mse = sum / (3.0 * c.width() * c.height());
    if (mse > 1.0) {
        const fs::path out = fs::absolute("menu-golden-" + name + ".png");
        anypsp::gpu::write_png(out.string(), bytes(c), c.width(), c.height());
        anypsp_test::fail(__FILE__, __LINE__, name + " differs from its golden picture (MSE " + std::to_string(mse) + "), actual in " + out.string());
    }
}

} // namespace

TEST_CASE("menu: tabs, settings, rebinding and the input file") {
    InputMap map;
    bool fullscreen = false;
    Menu menu(map, fullscreen, 2);
    menu.open();
    CHECK(menu.lines()[0] == "== Game ==");
    CHECK(menu.lines()[1] == "> Resume");
    // The Speed section: the set speed and the fast-forward speed, both saved as [player] settings.
    CHECK(menu.lines()[2] == "  -- Speed --");
    press(menu, pad::kDown);
    CHECK(menu.lines()[3] == "> Speed  < 100% >");
    auto speed_actions = press(menu, pad::kRight);
    CHECK(map.player().speed == 200);
    CHECK(speed_actions.size() == 1 && speed_actions[0] == anypsp::menu::Action::SettingsChanged);
    press(menu, pad::kLeft);
    press(menu, pad::kLeft);
    CHECK(map.player().speed == 400);
    press(menu, pad::kRight);
    press(menu, pad::kDown);
    CHECK(menu.lines()[4] == "> Fast-forward (hold)  < 300% >");
    for (int i = 0; i < 4; ++i) press(menu, pad::kRight);
    CHECK(map.player().fast_forward == 0);
    CHECK(menu.lines()[4] == "> Fast-forward (hold)  < unlimited >");
    for (int i = 0; i < 4; ++i) press(menu, pad::kLeft);
    CHECK(map.player().fast_forward == 300);
    // Exit wants a second Cross.
    press(menu, pad::kDown);
    press(menu, pad::kDown);
    CHECK(press(menu, pad::kCross).empty());
    CHECK(menu.lines()[7] == "> Exit game  Press again to confirm");
    const auto exit = press(menu, pad::kCross);
    CHECK(exit.size() == 2 && exit[0] == anypsp::menu::Action::Exit);
    CHECK(!menu.is_open());

    menu.open();
    press(menu, pad::kR);  // Saves
    CHECK(menu.lines()[0] == "== Saves ==");
    CHECK(menu.lines()[1] == "  No saves for this game yet");
    press(menu, pad::kR);  // Video
    CHECK(menu.lines()[1] == "> Graphics preset  < Medium >");
    press(menu, pad::kDown);
    CHECK(menu.lines()[2] == "> Internal resolution  < 2x (960x544) >");
    press(menu, pad::kRight);
    CHECK(map.scale() == 3);
    CHECK(menu.lines().back() == "! Applies from the next start");
    CHECK(menu.lines()[1] == "  Graphics preset  < High >");  // without display settings, scale and scaling decide
    press(menu, pad::kDown);
    const auto fs_actions = press(menu, pad::kCross);
    CHECK(fullscreen);
    CHECK(fs_actions.size() == 1 && fs_actions[0] == anypsp::menu::Action::Fullscreen);
    press(menu, pad::kDown);
    press(menu, pad::kRight);
    CHECK(map.player().sharp);

    // Audio: volume and mute.
    press(menu, pad::kR);
    CHECK(menu.lines()[0] == "== Audio ==");
    press(menu, pad::kLeft);
    press(menu, pad::kLeft);
    CHECK(map.player().volume == 80);
    press(menu, pad::kDown);
    press(menu, pad::kCross);
    CHECK(map.player().mute);

    // Controls: rebind Cross to C, then Circle to a gamepad button.
    key(menu, SDL_SCANCODE_TAB);
    CHECK(menu.lines()[0] == "== Controls ==");
    CHECK(menu.lines()[1] == "  -- PSP buttons --");
    CHECK(menu.lines()[2] == "> Up  Up | DpadUp");
    CHECK(menu.view().page.highlight == "Up");
    for (int i = 0; i < 4; ++i) press(menu, pad::kDown);
    CHECK(menu.view().page.highlight == "Cross");
    press(menu, pad::kCross);
    CHECK(menu.view().page.modal == "Press a key or button for Cross");
    key(menu, SDL_SCANCODE_C);
    CHECK(map.keys(Action::Cross) == std::vector<int>{SDL_SCANCODE_C});
    press(menu, pad::kDown);
    press(menu, pad::kCross);
    MenuInput pad_in;
    pad_in.buttons = {SDL_GAMEPAD_BUTTON_SOUTH};  // Cross's button: they swap
    menu.update(pad_in);
    CHECK(map.buttons(Action::Circle) == std::vector<int>{SDL_GAMEPAD_BUTTON_SOUTH});
    CHECK(map.buttons(Action::Cross) == std::vector<int>{SDL_GAMEPAD_BUTTON_EAST});
    CHECK(menu.lines().back() == "! Circle = South (swapped with Cross)");

    // Left on a row with no value goes to the rail; up/down there switch tabs.
    press(menu, pad::kLeft);
    CHECK(menu.lines()[1] == "[tabs]");
    press(menu, pad::kDown);
    CHECK(menu.lines()[0] == "== Cheats ==");
    press(menu, pad::kDown);
    CHECK(menu.lines()[0] == "== Settings ==");
    press(menu, pad::kRight);
    press(menu, pad::kRight);  // Language
    CHECK(map.player().language == "vi");
    CHECK(menu.lines()[0] == "== C\xC3\xA0i \xC4\x91\xE1\xBA\xB7t ==");
    press(menu, pad::kLeft);
    for (int i = 0; i < 4; ++i) press(menu, pad::kDown);
    press(menu, pad::kCross);
    CHECK(map.player().show_fps);

    // Clicks: a tab, a row.
    menu.click({anyui::HitKind::Tab, 0});
    CHECK(menu.lines()[0] == "== Game ==");
    const auto closed = menu.click({anyui::HitKind::Row, 0});
    CHECK(closed.size() == 1 && closed[0] == anypsp::menu::Action::Close);

    // Everything survives the input file.
    const fs::path dir = scratch("ini");
    map.save(dir / "anypsp-input.ini");
    InputMap again;
    again.load(dir / "anypsp-input.ini");
    CHECK(again.keys(Action::Cross) == std::vector<int>{SDL_SCANCODE_C});
    CHECK(again.buttons(Action::Circle) == std::vector<int>{SDL_GAMEPAD_BUTTON_SOUTH});
    CHECK(again.player() == map.player());
    CHECK(again.scale() == 3);
    CHECK(again.keys(Action::Menu) == (std::vector<int>{SDL_SCANCODE_ESCAPE, SDL_SCANCODE_F1}));
    std::ofstream(dir / "bad.ini", std::ios::binary) << "[player]\nVolume = 55\n";
    CHECK_THROWS(again.load(dir / "bad.ini"));
    std::ofstream(dir / "bad2.ini", std::ios::binary) << "[player]\nTheme = blue\n";
    CHECK_THROWS(again.load(dir / "bad2.ini"));
    map.reset_bindings();
    CHECK(map.keys(Action::Cross) == std::vector<int>{SDL_SCANCODE_Z});
    CHECK(map.player().show_fps);  // settings stay
}

TEST_CASE("menu: the Saves tab reads PARAM.SFO titles, newest first") {
    const fs::path ms = scratch("saves");
    write_sfo(ms / "PSP" / "SAVEDATA" / "ULUS10001DATA00" / "PARAM.SFO",
              {{"TITLE", "Test Quest"}, {"SAVEDATA_TITLE", "Chapter 1"}});
    write_sfo(ms / "PSP" / "SAVEDATA" / "ULES00002SYS" / "PARAM.SFO", {{"TITLE", "Other Game"}});
    fs::create_directories(ms / "PSP" / "SAVEDATA" / "ULUS10001DATA01");
    CHECK(anypsp::menu::sfo_string(ms / "PSP" / "SAVEDATA" / "ULUS10001DATA00" / "PARAM.SFO", "SAVEDATA_TITLE") == "Chapter 1");
    CHECK(anypsp::menu::sfo_string(ms / "nope.sfo", "TITLE").empty());
    const auto mine = anypsp::menu::scan_saves(ms, "ULUS10001");
    CHECK(mine.size() == 2);
    bool titled = false;
    for (const auto &s : mine) titled = titled || (s.title == "Test Quest" && s.detail.rfind("Chapter 1", 0) == 0);
    CHECK(titled);
    CHECK(anypsp::menu::scan_saves(ms, "").size() == 3);

    InputMap map;
    bool fullscreen = false;
    Menu menu(map, fullscreen, 2);
    menu.set_saves(mine);
    menu.open();
    press(menu, pad::kR);
    CHECK(menu.view().page.rows.size() == 2);
}

TEST_CASE("menu: the Video tab edits the display settings and the patches switch") {
    InputMap map;
    bool fullscreen = false;
    Menu menu(map, fullscreen, 2);
    menu.open();
    press(menu, pad::kR);
    press(menu, pad::kR);  // Video
    CHECK(menu.lines().back() == "  Game patches  No patch file");

    anypsp::display::Settings display;
    bool patches_on = true;
    menu.set_display(&display);
    menu.set_patches(&patches_on);
    const auto lines = menu.lines();
    CHECK(lines.size() == 13);
    if (lines.size() != 13) return;
    CHECK(lines[1] == "> Graphics preset  < Medium >");
    CHECK(lines[5] == "  Smoothing (30 fps games)  Off");
    CHECK(lines[6] == "  Frame interpolation  Off");
    CHECK(lines[7] == "  Refresh  < Auto (match monitor) >");
    CHECK(lines[12] == "  Game patches  On");
    const auto changed = [](const std::vector<anypsp::menu::Action> &a) {
        return a.size() == 1 && a[0] == anypsp::menu::Action::DisplayChanged;
    };
    for (int i = 0; i < 4; ++i) press(menu, pad::kDown);
    CHECK(changed(press(menu, pad::kCross)));
    CHECK(display.smoothing);
    press(menu, pad::kDown);
    CHECK(changed(press(menu, pad::kCross)));
    CHECK(display.interpolate);
    press(menu, pad::kDown);
    CHECK(changed(press(menu, pad::kRight)));
    CHECK(display.refresh == 60);
    CHECK(changed(press(menu, pad::kLeft)));
    press(menu, pad::kLeft);
    CHECK(display.refresh == 240);
    press(menu, pad::kDown);
    press(menu, pad::kCross);
    CHECK(display.vrr);
    press(menu, pad::kDown);
    press(menu, pad::kCross);
    CHECK(display.sync_to_monitor);
    press(menu, pad::kDown);
    press(menu, pad::kRight);
    CHECK(display.menu_fps == 60);
    press(menu, pad::kDown);
    press(menu, pad::kRight);
    CHECK(display.fps_cap == 30);
    CHECK(menu.lines()[11] == "> FPS cap  < 30 >");
    press(menu, pad::kDown);
    CHECK(changed(press(menu, pad::kCross)));
    CHECK(!patches_on);
}

// A cheat file as PPSSPP keeps them: two cheats and one AnyPSP refuses.
const char *const kCheatText = "_S ULUS-10001\n_G Test Quest Portable\n"
                               "_C1 Infinite health\n_L 0x10123456 0x000003E7\n"
                               "_C0 Max money\n_L 0x20123458 0x0098967F\n_L 0x20123460 0x0098967F\n"
                               "_C0 Rumble on hit\n_L 0xA0000000 0x00000000\n";

TEST_CASE("menu: the Cheats tab turns cheats on and off and says where cheat files go") {
    InputMap map;
    bool fullscreen = false;
    Menu menu(map, fullscreen, 2);
    menu.set_cheats(nullptr, "/data/anypsp/cheats", "ULUS10001.ini");
    menu.open();
    menu.click({anyui::HitKind::Tab, 5});
    CHECK(menu.lines()[0] == "== Cheats ==");
    auto rows = menu.view().page.rows;
    CHECK(rows.size() == 2);
    if (rows.size() != 2) return;
    CHECK(rows[0].label == "No cheat file for this game");
    CHECK(rows[1].label == "Import cheat file");
    CHECK(rows[1].detail == "Copy a CWCheat file (PPSSPP's cheats/ULUS10001.ini) into /data/anypsp/cheats");
    CHECK(menu.view().hint == "Cross on/off   L/R tabs   Circle close");

    const fs::path dir = scratch("cheats");
    const fs::path file = dir / "ULUS10001.ini";
    std::ofstream(file, std::ios::binary) << kCheatText;
    anypsp::cheats::CheatFile cheats = anypsp::cheats::CheatFile::load(file, "ULUS10001");
    menu.set_cheats(&cheats, dir.string(), "ULUS10001.ini");
    menu.click({anyui::HitKind::Tab, 5});
    rows = menu.view().page.rows;
    CHECK(rows.size() == 4);
    if (rows.size() != 4) return;
    CHECK(menu.lines()[1] == "> [x] Infinite health  1 code");
    CHECK(menu.lines()[2] == "  [ ] Max money  2 codes");
    CHECK(rows[2].kind == anyui::RowKind::Check);
    CHECK(!rows[2].enabled);
    CHECK(rows[2].detail.find("vibration") != std::string::npos);
    CHECK(rows[3].kind == anyui::RowKind::Info);

    // Cross turns a cheat off and on; the host saves the file.
    auto actions = press(menu, pad::kCross);
    CHECK(actions.size() == 1 && actions[0] == anypsp::menu::Action::CheatsChanged);
    CHECK(!cheats.cheats()[0].enabled);
    CHECK(menu.lines()[1] == "> [ ] Infinite health  1 code");
    CHECK(menu.lines().back() == "! Off: Infinite health");
    press(menu, pad::kDown);
    actions = menu.click({anyui::HitKind::Row, 1});
    CHECK(actions.size() == 1 && actions[0] == anypsp::menu::Action::CheatsChanged);
    CHECK(cheats.cheats()[1].enabled);
    cheats.save();
    const auto saved = anypsp::cheats::CheatFile::load(file, "ULUS10001");
    CHECK(!saved.cheats()[0].enabled);
    CHECK(saved.cheats()[1].enabled);
    // A refused cheat stays off and says why.
    press(menu, pad::kDown);
    CHECK(press(menu, pad::kCross).empty());
    CHECK(!cheats.cheats()[2].enabled);
    CHECK(menu.lines().back() ==
          "! Cannot turn on: vibration and post-processing codes (0xA) change no memory and are not supported (line 9: _L "
          "0xA0000000 0x00000000)");
    // Vietnamese.
    map.player().language = "vi";
    CHECK(menu.lines()[0] == "== M\xC3\xA3 gian l\xE1\xBA\xADn ==");
    CHECK(menu.lines()[2] == "  [x] Max money  2 m\xC3\xA3");
}

TEST_CASE("menu: graphics presets set the quality settings and show Custom after a change") {
    using anypsp::menu::kCustom;
    using anypsp::menu::kPresets;
    using anypsp::menu::preset_of;
    InputMap map;
    bool fullscreen = false;
    anypsp::display::Settings display;
    Menu menu(map, fullscreen, 2);
    menu.set_display(&display);
    menu.open();
    press(menu, pad::kR);
    press(menu, pad::kR);  // Video, on the preset row
    CHECK(menu.lines()[1] == "> Graphics preset  < Medium >");
    CHECK(preset_of(map, &display) == 2);

    // Right walks up the ladder and sets every covered value; refresh and the others stay.
    display.refresh = 144;
    const auto actions = press(menu, pad::kRight);
    CHECK((actions == std::vector<anypsp::menu::Action>{anypsp::menu::Action::SettingsChanged, anypsp::menu::Action::DisplayChanged}));
    CHECK(menu.lines()[1] == "> Graphics preset  < High >");
    CHECK(map.scale() == 3);
    CHECK(!map.player().sharp);
    CHECK(display.smoothing);
    CHECK(!display.interpolate);
    CHECK(display.refresh == 144);
    CHECK(menu.lines().back() == "! Applies from the next start");
    press(menu, pad::kRight);
    CHECK(menu.lines()[1] == "> Graphics preset  < Very high >");
    CHECK(display.interpolate);
    CHECK(!display.interpolate_auto);
    press(menu, pad::kRight);
    CHECK(map.scale() == 4);
    CHECK(menu.lines()[1] == "> Graphics preset  < Ultra >");
    press(menu, pad::kRight);  // wraps to the cheapest
    CHECK(menu.lines()[1] == "> Graphics preset  < Very low >");
    CHECK(map.scale() == 1);
    CHECK(map.player().sharp);
    CHECK(!display.smoothing);
    CHECK(!display.interpolate);
    for (int i = 0; i < 6; ++i) {
        CHECK(preset_of(map, &display) == (i + 0) % 6);
        press(menu, pad::kRight);
    }

    // A manual change of a covered row shows Custom; changing it back shows the preset again.
    CHECK(menu.lines()[1] == "> Graphics preset  < Very low >");
    press(menu, pad::kDown);
    press(menu, pad::kDown);
    press(menu, pad::kDown);  // Scaling
    press(menu, pad::kRight);
    CHECK(!map.player().sharp);
    CHECK(menu.lines()[1] == "  Graphics preset  < Low >");  // sharp off at 1x is Low
    press(menu, pad::kDown);   // Smoothing
    press(menu, pad::kCross);
    CHECK(menu.lines()[1] == "  Graphics preset  < Custom >");
    press(menu, pad::kCross);
    CHECK(menu.lines()[1] == "  Graphics preset  < Low >");
    CHECK(menu.view().page.preview.title == "Smoothing (30 fps games): Off");

    // From Custom, Right starts at Very low and Left at Ultra.
    display.vrr = true;  // not covered
    display.smoothing = true;
    CHECK(preset_of(map, &display) == kCustom);
    press(menu, pad::kUp);
    press(menu, pad::kUp);
    press(menu, pad::kUp);
    press(menu, pad::kUp);
    press(menu, pad::kLeft);
    CHECK(preset_of(map, &display) == 5);
    CHECK(map.scale() == kPresets[5].scale);
    CHECK(display.vrr);

    // Interpolate = auto counts as what it resolved to, and is kept when it already matches.
    anypsp::display::Settings autod;
    autod.interpolate_auto = true;
    autod.interpolate = true;  // resolved on for this game
    map.set_scale(3);
    map.player().sharp = false;
    autod.smoothing = true;
    CHECK(preset_of(map, &autod) == 4);
    anypsp::menu::apply_preset(4, map, &autod);
    CHECK(autod.interpolate_auto);
    anypsp::menu::apply_preset(3, map, &autod);
    CHECK(!autod.interpolate);
    CHECK(!autod.interpolate_auto);
}

TEST_CASE("menu: the Video preview's keys, descriptions and pointer") {
    InputMap map;
    bool fullscreen = false;
    anypsp::display::Settings display;
    bool patches_on = true;
    Menu menu(map, fullscreen, 1);
    menu.set_display(&display);
    menu.set_patches(&patches_on);
    menu.open();
    press(menu, pad::kR);
    press(menu, pad::kR);  // Video
    anyui::Frame f = menu.view();
    CHECK(f.page.preview.shown);
    CHECK(f.hint == "Cross choose   Left/Right change   Select loupe   [ ] or LT/RT divider   L/R tabs   Circle close");
    CHECK(f.page.preview.title == "Graphics preset: Medium");
    anyui::PreviewState &st = menu.preview_state();
    key(menu, SDL_SCANCODE_RIGHTBRACKET);
    CHECK(approx_equal(st.split, 0.6f));
    MenuInput in;
    in.buttons = {anypsp::input::kLeftTrigger};
    menu.update(in);
    menu.update(in);
    CHECK(approx_equal(st.split, 0.4f));
    press(menu, pad::kSelect);
    CHECK(st.loupe);
    MenuInput shifted;
    menu.update({});
    shifted.pad = pad::kDown;
    shifted.shift = true;
    menu.update(shifted);
    CHECK(approx_equal(st.loupe_y, 0.55f));
    CHECK(menu.lines()[1].starts_with("> Graphics preset"));  // the cursor stayed
    in = {};
    in.stick_x = -1;
    menu.update(in);
    CHECK(st.loupe_x < 0.5f);
    press(menu, pad::kSelect);
    CHECK(!st.loupe);

    // Every Video row describes itself, in both languages.
    for (const char *language : {"en", "vi"}) {
        map.player().language = language;
        menu.click({anyui::HitKind::Tab, 2});
        std::vector<std::string> seen;
        const auto rows = menu.view().page.rows;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            menu.click({anyui::HitKind::Row, static_cast<int>(i)});
            const std::string d = menu.view().page.preview.description;
            CHECK(!d.empty());
            CHECK(!d.ends_with(".desc"));
            CHECK(std::find(seen.begin(), seen.end(), d) == seen.end());
            seen.push_back(d);
        }
        CHECK(seen.size() == 12);
    }
    map.player().language = "en";

    // The view: a mouse drag moves the divider, hovering shows the loupe.
    anyui::ViewOptions look;
    look.reduce_motion = true;
    anyui::MenuView view(anypsp::menu::regular_font(), anypsp::menu::bold_font());
    const anyui::Canvas backdrop = scene(960, 540);
    view.set_background(backdrop.data(), 960, 540, look);
    view.open();
    anyui::Canvas out;
    view.render(out, with_preview(menu, map, 960, 540), look, 1.f / 60);
    const anyui::Rect pic = view.rect_of({anyui::HitKind::Preview, -1});
    CHECK(pic.w > 100);
    CHECK(std::abs(pic.w / pic.h - 480.f / 272.f) < 0.03f);
    using Phase = anyui::MenuView::PointerPhase;
    CHECK(view.preview_pointer(st, Phase::Down, pic.x + pic.w * 0.75f, pic.y + 5, false));
    CHECK(approx_equal(st.split, 0.75f));
    CHECK(view.preview_pointer(st, Phase::Up, pic.x + pic.w * 0.75f, pic.y + 5, false));
    CHECK(!view.preview_pointer(st, Phase::Move, pic.x + 10, pic.y + 10, false));
    CHECK(st.pointer);
    view.preview_pointer(st, Phase::Move, pic.x + pic.w * 0.73f, pic.y + pic.h * 0.62f, false);
    view.render(out, with_preview(menu, map, 960, 540), look, 1.f / 60);
    check_golden("video-loupe-1x-dark", out);
    view.preview_pointer(st, Phase::Move, 2, 2, false);
    CHECK(!st.pointer);

    // A preview picture is the game picture at the size it takes in the window.
    anypsp::menu::PreviewPictures pictures;
    const anyui::Canvas game = scene(480, 272);
    pictures.make(game.data(), 480, 272, 1920, 1200, true);
    CHECK(pictures.width == 1920);
    CHECK(pictures.height == 1088);
    CHECK(pictures.before == pictures.after);  // sharp: both nearest
    pictures.make(game.data(), 480, 272, 1920, 1200, false);
    CHECK(pictures.before != pictures.after);
}

TEST_CASE("menu: golden pictures of every tab at 1x and 2x, dark and light") {
    InputMap map;
    bool fullscreen = false;
    Menu menu(map, fullscreen, 2);
    menu.set_game_title("Test Quest Portable");
    std::vector<anypsp::menu::SaveInfo> saves(2);
    const anyui::Canvas icon = scene(144, 80);
    saves[0].title = "Test Quest Portable";
    saves[0].detail = "Chapter 3  \xC2\xB7  ULUS10001DATA00  \xC2\xB7  2026-10-24 21:05";
    saves[0].icon = icon.pixels();
    saves[0].icon_width = 144;
    saves[0].icon_height = 80;
    saves[1].title = "Test Quest Portable";
    saves[1].detail = "System data  \xC2\xB7  ULUS10001SYS  \xC2\xB7  2026-10-20 09:12";
    menu.set_saves(saves);
    anypsp::display::Settings display;
    bool patches_on = true;
    menu.set_display(&display);
    menu.set_patches(&patches_on);
    anypsp::cheats::CheatFile cheats = anypsp::cheats::CheatFile::parse(kCheatText, "ULUS10001");
    menu.set_cheats(&cheats, "/home/player/.local/share/anyport/anypsp/cheats", "ULUS10001.ini");
    const std::array<const char *, 7> names = {"game", "saves", "video", "audio", "controls", "cheats", "settings"};
    for (const int scale : {1, 2}) {
        const int w = 960 * scale, h = 540 * scale;
        const anyui::Canvas backdrop = scene(w, h);
        for (const bool dark : {true, false}) {
            anyui::ViewOptions look;
            look.dark = dark;
            look.reduce_motion = true;
            anyui::MenuView view(anypsp::menu::regular_font(), anypsp::menu::bold_font());
            view.set_background(backdrop.data(), w, h, look);
            menu.open();
            view.open();
            anyui::Canvas out;
            for (std::size_t tab = 0; tab < names.size(); ++tab) {
                menu.click({anyui::HitKind::Tab, static_cast<int>(tab)});
                const anyui::Frame frame = with_preview(menu, map, w, h);
                CHECK(frame.tab == static_cast<int>(tab));
                view.render(out, frame, look, 1.f / 60);
                check_golden(std::string(names[tab]) + "-" + std::to_string(scale) + "x-" + (dark ? "dark" : "light"), out);
            }
            if (dark && scale == 1) {
                menu.click({anyui::HitKind::Tab, 4});
                menu.click({anyui::HitKind::Row, 5});  // Cross
                CHECK(menu.view().page.modal == "Press a key or button for Cross");
                view.render(out, menu.view(), look, 1.f / 60);
                check_golden("controls-capture-1x-dark", out);
                menu.click({anyui::HitKind::Outside, -1});
                // A click through what was drawn: the Audio tab.
                view.render(out, menu.view(), look, 1.f / 60);
                const anyui::Rect r = view.rect_of({anyui::HitKind::Tab, 3});
                menu.click(view.hit(r.x + r.w / 2, r.y + r.h / 2));
                CHECK(menu.lines()[0] == "== Audio ==");
                // The display settings at the end of the Video tab.
                menu.click({anyui::HitKind::Tab, 2});
                const anyui::Frame video = with_preview(menu, map, w, h);
                view.render(out, video, look, 1.f / 60);
                view.wheel(100);
                view.render(out, video, look, 1.f / 60);
                view.render(out, video, look, 1.f / 60);
                check_golden("video-end-1x-dark", out);
                // The Cheats tab without a cheat file.
                menu.set_cheats(nullptr, "/home/player/.local/share/anyport/anypsp/cheats", "ULUS10001.ini");
                menu.click({anyui::HitKind::Tab, 5});
                view.render(out, menu.view(), look, 1.f / 60);
                check_golden("cheats-none-1x-dark", out);
                menu.set_cheats(&cheats, "/home/player/.local/share/anyport/anypsp/cheats", "ULUS10001.ini");
            }
        }
    }
}

TEST_CASE("menu: a session in a hidden GL window opens over the game and closes with Escape") {
    psprecomp::GuestMemory memory;
    anypsp::gpu::GlHost gl(memory);
    try {
        gl.start(1);  // hidden: the window is never shown
    } catch (const std::exception &e) {
        std::cout << "  skipped (no OpenGL 3.3: " << e.what() << ")\n";
        return;
    }
    anypsp::desktop::Controls controls{InputMap{}};
    const fs::path dir = scratch("session");
    write_sfo(dir / "ms" / "PSP" / "SAVEDATA" / "ULUS10001DATA00" / "PARAM.SFO", {{"TITLE", "Test Quest"}});
    anypsp::menu::MenuSession session(gl, controls, dir / "anypsp-input.ini", dir / "ms", "ULUS10001", "Test Quest");
    SDL_Event escape{};
    escape.type = SDL_EVENT_KEY_DOWN;
    escape.key.scancode = SDL_SCANCODE_ESCAPE;
    escape.key.down = true;
    CHECK(SDL_PushEvent(&escape));
    using Result = anypsp::menu::MenuSession::Result;
    // The VRAM framebuffer at 0x04000000, 512 pixels per line, 8888.
    CHECK(session.run(0x04000000u, 512u, 3u) == Result::Resume);
    session.set_rates(59.9, 30.0);
}

namespace {

using pspweb::SystemDialog;

SystemDialog::Item save_item(std::string name, std::string detail, std::int64_t time, bool icon) {
    SystemDialog::Item it;
    it.name = std::move(name);
    it.exists = true;
    it.title = "NARUTO: Ultimate Ninja Heroes";
    it.savedata_title = "Save Data";
    it.detail = std::move(detail);
    it.time = time;
    if (icon) it.icon = "icon0.png";
    return it;
}

// The dialogs the golden pictures show.
SystemDialog sample_dialog(const std::string &kind) {
    SystemDialog d;
    if (kind == "list") {
        d.kind = SystemDialog::Kind::SaveList;
        d.heading = SystemDialog::Heading::Save;
        d.items = {save_item("0000", "Play Time 000:00:47 / Rank: ACADEMY", 1, true),
                   save_item("0001", "Play Time 012:41:05 / Rank: GENIN\nSecond line", 1, false), {}};
        d.items[2].name = "0002";
        d.focus = 1;
    } else if (kind == "overwrite") {
        d.heading = SystemDialog::Heading::Save;
        d.text_id = SystemDialog::Text::ConfirmOverwrite;
        d.buttons = SystemDialog::Buttons::YesNo;
        d.focus = 1;
        d.items = {save_item("0000", "Play Time 000:00:47 / Rank: ACADEMY", 1, true)};
    } else if (kind == "nodata") {
        d.heading = SystemDialog::Heading::Load;
        d.text_id = SystemDialog::Text::NoData;
    } else {
        d.text = "Save data?\nYour progress in Naruto's House is kept on the Memory Stick.";
        d.buttons = SystemDialog::Buttons::YesNo;
    }
    return d;
}

} // namespace

TEST_CASE("dialogs: frames in both languages, error codes and wrapping") {
    anyui::Strings &strings = anypsp::menu::strings();
    CHECK(strings.missing("vi").empty());
    const auto no_icon = [](const fs::path &) { return anyui::Thumb{}; };
    strings.set_language("en");
    SystemDialog d;
    d.text_id = SystemDialog::Text::Error;
    d.error = 0x80110307u;
    anypsp::dialog::Frame f = anypsp::dialog::make_frame(d, strings, no_icon);
    CHECK(f.message == "An error has occurred. (80110307)" && f.buttons.size() == 1u && f.buttons[0] == "OK");
    CHECK(f.heading.empty() && !f.list && f.confirm_hint == "OK" && f.back_hint == "Back");
    d = sample_dialog("nodata");
    f = anypsp::dialog::make_frame(d, strings, no_icon);
    CHECK(f.message == "There is no data." && f.heading == "Load" && f.icon == anyui::Icon::Load);
    d = sample_dialog("overwrite");
    f = anypsp::dialog::make_frame(d, strings, no_icon);
    CHECK(f.message == "Do you want to overwrite?" && f.buttons.size() == 2u && f.buttons[1] == "No" && f.focus == 1);
    CHECK(f.rows.size() == 1u && f.rows[0].title == "NARUTO: Ultimate Ninja Heroes" && f.rows[0].subtitle == "Save Data");
    d.text_id = SystemDialog::Text::SaveFailed;
    d.error = 0x80110385u;
    CHECK(anypsp::dialog::make_frame(d, strings, no_icon).message == "Unable to save data. (80110385)");
    for (const auto &[id, text] : std::vector<std::pair<SystemDialog::Text, std::string>>{
             {SystemDialog::Text::ConfirmLoad, "Do you want to load this data?"},
             {SystemDialog::Text::ConfirmSave, "Do you want to save this data?"},
             {SystemDialog::Text::ConfirmDelete, "Do you want to delete this data?"},
             {SystemDialog::Text::LoadDone, "Load completed."},
             {SystemDialog::Text::SaveDone, "Save completed."},
             {SystemDialog::Text::DeleteDone, "Delete completed."}}) {
        d.text_id = id;
        CHECK(anypsp::dialog::make_frame(d, strings, no_icon).message == text);
    }
    d = sample_dialog("list");
    int asked = 0;
    f = anypsp::dialog::make_frame(d, strings, [&](const fs::path &png) {
        ++asked;
        CHECK(png == "icon0.png");
        return anyui::Thumb{};
    });
    CHECK(asked == 1 && f.list && f.rows.size() == 3u && f.buttons.empty() && f.confirm_hint == "Choose");
    CHECK(f.rows[2].new_data && f.rows[2].title == "New data" && f.rows[2].date.empty() && !f.rows[0].date.empty());
    CHECK(f.heading == "Save" && f.focus == 1);

    strings.set_language("vi");
    f = anypsp::dialog::make_frame(sample_dialog("nodata"), strings, no_icon);
    CHECK(f.message == "Không có dữ liệu." && f.buttons[0] == "OK" && f.heading == "Tải dữ liệu");
    f = anypsp::dialog::make_frame(sample_dialog("list"), strings, no_icon);
    CHECK(f.rows[2].title == "Dữ liệu mới" && f.back_hint == "Quay lại");
    strings.set_language("en");

    const anyui::Font &font = anypsp::menu::regular_font();
    const auto lines = anypsp::dialog::wrap(font, "one two three four five six seven eight\n\nnine", 16, 120);
    CHECK(lines.size() >= 4u && lines.back() == "nine");
    CHECK(std::find(lines.begin(), lines.end(), std::string()) != lines.end());
    for (const std::string &l : lines) CHECK(font.measure(l, 16) <= 120.5f);
    CHECK(anypsp::dialog::wrap(font, "", 16, 100).size() == 1u);
    CHECK(anypsp::dialog::format_time(0).empty());
}

TEST_CASE("dialogs: the PSP buttons answer the dialog and never reach the game twice") {
    using anypsp::dialog::Keys;
    Keys keys;
    SystemDialog d;
    d.buttons = SystemDialog::Buttons::YesNo;
    // Cross held from the game's own prompt into the dialog: no answer until pressed again.
    CHECK(keys.update(nullptr, pad::kCross).game == pad::kCross);
    Keys::Out out = keys.update(&d, pad::kCross);
    CHECK(out.game == 0u && !out.confirm && !out.back && out.move == 0);
    CHECK(!keys.update(&d, 0u).confirm);
    CHECK(keys.update(&d, pad::kRight).move == 1);
    CHECK(keys.update(&d, pad::kRight | pad::kLeft).move == -1);
    out = keys.update(&d, pad::kCross);
    CHECK(out.confirm && out.game == 0u);
    // Closed while Cross is held: the game sees it only after a release.
    CHECK(keys.update(nullptr, pad::kCross | pad::kUp).game == pad::kUp);
    CHECK(keys.update(nullptr, pad::kCross).game == 0u);
    CHECK(keys.update(nullptr, 0u).game == 0u);
    CHECK(keys.update(nullptr, pad::kCross).game == pad::kCross);
    keys.update(nullptr, 0u);
    // Circle goes back; the Japanese layout swaps the two.
    CHECK(keys.update(&d, pad::kCircle).back);
    keys.update(&d, 0u);
    d.cross_confirms = false;
    CHECK(keys.update(&d, pad::kCircle).confirm);
    keys.update(&d, 0u);
    CHECK(keys.update(&d, pad::kCross).back);
    // A list moves up and down only, repeating while held.
    SystemDialog list;
    list.kind = SystemDialog::Kind::SaveList;
    keys.update(&list, 0u);
    CHECK(keys.update(&list, pad::kRight).move == 0);
    keys.update(&list, 0u);
    CHECK(keys.update(&list, pad::kDown).move == 1);
    int moves = 0;
    for (int frame = 0; frame < 60; ++frame) moves += keys.update(&list, pad::kDown).move;
    CHECK(moves >= 6 && moves <= 8);
}

TEST_CASE("dialogs: golden pictures of the list, a question, a message and Yes/No in both languages and themes") {
    const anyui::Canvas icon = scene(144, 80);
    anyui::Strings &strings = anypsp::menu::strings();
    const auto icons = [&](const fs::path &) { return anyui::Thumb{icon.data(), 144, 80}; };
    for (const std::string kind : {"list", "overwrite", "nodata", "yesno"}) {
        for (const char *language : {"en", "vi"}) {
            for (const bool dark : {true, false}) {
                for (const int scale : {1, 2}) {
                    if (scale == 2 && (kind != "list" || !dark || std::string(language) != "en")) continue;
                    strings.set_language(language);
                    SystemDialog d = sample_dialog(kind);
                    anypsp::dialog::Frame f = anypsp::dialog::make_frame(d, strings, icons);
                    // Fixed dates: the pictures must not depend on the time zone.
                    for (anypsp::dialog::Row &r : f.rows)
                        if (!r.new_data) r.date = "2026-10-24 21:05";
                    anyui::ViewOptions look;
                    look.dark = dark;
                    look.reduce_motion = true;
                    anyui::Canvas out = scene(960 * scale, 540 * scale);
                    anypsp::dialog::View view(anypsp::menu::regular_font(), anypsp::menu::bold_font());
                    view.render(out, f, look, 1.f / 60);
                    check_golden("dialog-" + kind + "-" + language + "-" + std::to_string(scale) + "x-" + (dark ? "dark" : "light"), out);
                    if (kind == "list") {
                        const anyui::Rect r = view.rect_of({anypsp::dialog::Hit::Kind::Row, 2});
                        CHECK(r.w > 0 && view.hit(r.x + r.w / 2, r.y + r.h / 2) == (anypsp::dialog::Hit{anypsp::dialog::Hit::Kind::Row, 2}));
                    } else {
                        const anyui::Rect r = view.rect_of({anypsp::dialog::Hit::Kind::Button, 0});
                        CHECK(r.w > 0 && view.hit(r.x + 2, r.y + r.h / 2).kind == anypsp::dialog::Hit::Kind::Button);
                        CHECK(view.hit(1, 1).kind == anypsp::dialog::Hit::Kind::None);
                    }
                }
            }
        }
    }
    strings.set_language("en");
}

TEST_CASE("dialogs: the overlay follows the menu's language and theme and loads ICON0.PNG") {
    const fs::path dir = scratch("dialog-overlay");
    const anyui::Canvas icon = scene(144, 80);
    anypsp::gpu::write_png((dir / "ICON0.PNG").string(), bytes(icon), 144, 80);
    anypsp::dialog::Icons icons;
    const anyui::Thumb thumb = icons.get(dir / "ICON0.PNG");
    CHECK(thumb.width == 144 && thumb.height == 80 && thumb.pixels[0] == icon.data()[0]);
    CHECK(icons.get(dir / "missing.png").empty());

    InputMap map;
    map.player().language = "vi";
    map.player().light_theme = true;
    map.player().reduce_motion = true;
    anypsp::dialog::Overlay overlay(map);
    CHECK(!overlay.look().dark && overlay.look().reduce_motion);
    SystemDialog d = sample_dialog("list");
    d.items[0].icon = dir / "ICON0.PNG";
    anyui::Canvas picture = scene(960, 540);
    const std::vector<std::uint32_t> before = picture.pixels();
    overlay.draw(d, picture.data(), 960, 540, 1.f / 60);
    CHECK(picture.pixels() != before);
    CHECK(overlay.hit(480, 270).kind == anypsp::dialog::Hit::Kind::Row);
    CHECK(overlay.hit(2, 2).kind == anypsp::dialog::Hit::Kind::None);
    CHECK(anypsp::menu::strings().language() == "vi");
    anypsp::menu::strings().set_language("en");
    fs::remove_all(dir);
}
