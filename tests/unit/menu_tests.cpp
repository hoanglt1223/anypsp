// SPDX-License-Identifier: GPL-2.0-or-later
// The in-game menu without a window: scripted navigation, rebinding, settings in the input
// file, the Saves tab's PARAM.SFO reader, and golden pictures of every tab at 1x (960x540)
// and 2x (1920x1080) in the dark and light themes, drawn offscreen.
// ANYPSP_UPDATE_GOLDENS=1 rewrites tests/golden/menu; ANYPSP_MENU_SHOTS=<dir> also writes
// every rendered picture there.

#include "test_harness.hpp"

#include "desktop.hpp"
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

bool near(float a, float b) { return std::abs(a - b) < 0.011f; }

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
    CHECK(near(st.split, 0.6f));
    MenuInput in;
    in.buttons = {anypsp::input::kLeftTrigger};
    menu.update(in);
    menu.update(in);
    CHECK(near(st.split, 0.4f));
    press(menu, pad::kSelect);
    CHECK(st.loupe);
    MenuInput shifted;
    menu.update({});
    shifted.pad = pad::kDown;
    shifted.shift = true;
    menu.update(shifted);
    CHECK(near(st.loupe_y, 0.55f));
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
    CHECK(near(st.split, 0.75f));
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
    const std::array<const char *, 6> names = {"game", "saves", "video", "audio", "controls", "settings"};
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
