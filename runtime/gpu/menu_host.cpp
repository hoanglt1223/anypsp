// SPDX-License-Identifier: GPL-2.0-or-later

#include "menu_host.hpp"

#include "desktop.hpp"
#include "gl_host.hpp"
#include "speed.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <tuple>
#include <cstring>

namespace anypsp::menu {
namespace fs = std::filesystem;
namespace {

constexpr int kWidth = 480, kHeight = 272;

std::uint32_t le32(const std::vector<unsigned char> &d, std::size_t at) {
    return at + 4 <= d.size() ? static_cast<std::uint32_t>(d[at] | d[at + 1] << 8 | d[at + 2] << 16 | static_cast<std::uint32_t>(d[at + 3]) << 24)
                              : 0u;
}

std::uint16_t le16(const std::vector<unsigned char> &d, std::size_t at) {
    return at + 2 <= d.size() ? static_cast<std::uint16_t>(d[at] | d[at + 1] << 8) : std::uint16_t{0};
}

std::string when(fs::file_time_type t) {
    const auto at = std::chrono::time_point_cast<std::chrono::system_clock::duration>(t - fs::file_time_type::clock::now() +
                                                                                      std::chrono::system_clock::now());
    const std::time_t tt = std::chrono::system_clock::to_time_t(at);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%d %H:%M", &tm);
    return text;
}

bool load_icon(const fs::path &png, SaveInfo &save) { return load_icon_pixels(png, save.icon, save.icon_width, save.icon_height); }

} // namespace

bool load_icon_pixels(const fs::path &png, std::vector<std::uint32_t> &pixels, int &width, int &height) {
    const std::u8string name = png.u8string();
    SDL_Surface *loaded = SDL_LoadPNG(reinterpret_cast<const char *>(name.c_str()));
    if (loaded == nullptr) return false;
    SDL_Surface *rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_ABGR8888);
    SDL_DestroySurface(loaded);
    if (rgba == nullptr) return false;
    width = rgba->w;
    height = rgba->h;
    pixels.resize(static_cast<std::size_t>(rgba->w) * static_cast<std::size_t>(rgba->h));
    for (int y = 0; y < rgba->h; ++y) {
        const auto *row = reinterpret_cast<const std::uint32_t *>(static_cast<const std::uint8_t *>(rgba->pixels) + y * rgba->pitch);
        std::copy(row, row + rgba->w, pixels.begin() + static_cast<std::ptrdiff_t>(y) * rgba->w);
    }
    SDL_DestroySurface(rgba);
    // Icons have transparent corners; the menu draws them opaque, so put them on black.
    for (std::uint32_t &p : pixels) {
        const std::uint32_t a = p >> 24;
        const auto ch = [&](int shift) { return (((p >> shift) & 0xFFu) * a + 127u) / 255u; };
        p = ch(0) | ch(8) << 8 | ch(16) << 16 | 0xFF000000u;
    }
    return true;
}

std::string sfo_string(const fs::path &sfo, const std::string &key) {
    std::ifstream in(sfo, std::ios::binary);
    if (!in) return {};
    const std::vector<unsigned char> d{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (d.size() < 20 || d[0] != 0 || d[1] != 'P' || d[2] != 'S' || d[3] != 'F') return {};
    const std::uint32_t keys = le32(d, 8), data = le32(d, 12), count = le32(d, 16);
    for (std::uint32_t i = 0; i < count && i < 256; ++i) {
        const std::size_t entry = 20 + static_cast<std::size_t>(i) * 16;
        if (entry + 16 > d.size()) break;
        const std::size_t name_at = keys + le16(d, entry);
        std::string name;
        for (std::size_t k = name_at; k < d.size() && d[k] != 0; ++k) name += static_cast<char>(d[k]);
        if (name != key || le16(d, entry + 2) != 0x0204u) continue;
        const std::size_t value_at = static_cast<std::size_t>(data) + le32(d, entry + 12);
        const std::size_t length = le32(d, entry + 4);
        if (value_at + length > d.size()) return {};
        std::string value(reinterpret_cast<const char *>(d.data() + value_at), length);
        value.erase(std::find(value.begin(), value.end(), '\0'), value.end());
        return value;
    }
    return {};
}

std::vector<SaveInfo> scan_saves(const fs::path &memstick, const std::string &game_id) {
    std::vector<std::pair<fs::file_time_type, SaveInfo>> found;
    std::error_code ec;
    const fs::path dir = memstick / "PSP" / "SAVEDATA";
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::string folder = it->path().filename().string();
        if (!game_id.empty() && folder.rfind(game_id, 0) != 0) continue;
        SaveInfo s;
        const fs::path sfo = it->path() / "PARAM.SFO";
        s.title = sfo_string(sfo, "TITLE");
        const std::string save_title = sfo_string(sfo, "SAVEDATA_TITLE");
        if (s.title.empty()) s.title = folder;
        const auto time = fs::last_write_time(fs::exists(sfo, ec) ? sfo : it->path(), ec);
        s.detail = (save_title.empty() ? folder : save_title + "  \xC2\xB7  " + folder) + (ec ? "" : "  \xC2\xB7  " + when(time));
        load_icon(it->path() / "ICON0.PNG", s);
        found.emplace_back(ec ? fs::file_time_type{} : time, std::move(s));
    }
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    std::vector<SaveInfo> out;
    for (auto &f : found) out.push_back(std::move(f.second));
    return out;
}

void PreviewPictures::make(const std::uint32_t *game, int game_w, int game_h, int w, int h, bool sharp) {
    float fw = static_cast<float>(w), fh = fw * kHeight / kWidth;
    if (fh > static_cast<float>(h)) {
        fh = static_cast<float>(h);
        fw = fh * kWidth / kHeight;
    }
    width = std::max(1, static_cast<int>(std::lround(fw)));
    height = std::max(1, static_cast<int>(std::lround(fh)));
    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    before.resize(n);
    after.resize(n);
    anyui::resample(game, game_w, game_h, before.data(), width, height, anyui::Resample::Nearest);
    anyui::resample(game, game_w, game_h, after.data(), width, height, sharp ? anyui::Resample::Nearest : anyui::Resample::Linear);
}

struct MenuSession::State {
    gpu::GlHost &gl;
    desktop::Controls &controls;
    fs::path ini, memstick;
    std::string game_id;
    bool fullscreen{};
    Menu menu;
    anyui::MenuView view{regular_font(), bold_font()};
    anyui::Canvas canvas;
    std::vector<std::uint32_t> game;  // the paused game picture, internal resolution
    int game_w{}, game_h{};
    PreviewPictures preview;
    int preview_w{}, preview_h{};
    bool preview_sharp{};
    std::uint64_t preview_version{};
    double hz{}, fps{};
    int speed_percent{100};
    const display::Settings *display{};
    std::function<void()> display_changed;
    cheats::CheatFile *cheats{};
    std::optional<std::int64_t> drag_finger;
    float drag_y{}, press_y{};
    bool dragged{};

    State(gpu::GlHost &g, desktop::Controls &c, fs::path i, fs::path m, std::string id)
        : gl(g), controls(c), ini(std::move(i)), memstick(std::move(m)), game_id(std::move(id)), fullscreen(g.fullscreen()),
          menu(c.map(), fullscreen, g.scale()) {}

    // The menu's frame time: Menu FPS from the display settings, else 60 per second.
    [[nodiscard]] std::uint64_t frame_ns() const {
        return 1'000'000'000u / static_cast<unsigned>(display != nullptr && display->menu_fps > 0 ? display->menu_fps : 60);
    }

    [[nodiscard]] anyui::ViewOptions look() const {
        const input::PlayerSettings &p = controls.map().player();
        anyui::ViewOptions o;
        o.dark = !p.light_theme;
        o.reduce_motion = p.reduce_motion;
        o.ui_scale = static_cast<float>(p.ui_scale) / 100.f;
        return o;
    }

    // The game picture letterboxed into the window, the backdrop the menu frosts.
    void backdrop(int w, int h) {
        anyui::Canvas c(w, h);
        c.clear({0, 0, 0, 1});
        float pw = static_cast<float>(w), ph = pw * kHeight / kWidth;
        if (ph > static_cast<float>(h)) {
            ph = static_cast<float>(h);
            pw = ph * kWidth / kHeight;
        }
        c.image(game.data(), game_w, game_h, {(static_cast<float>(w) - pw) / 2, (static_cast<float>(h) - ph) / 2, pw, ph});
        view.set_background(c.data(), w, h, look());
    }

    void save_settings() {
        try {
            controls.map().save(ini);
        } catch (const std::exception &e) {
            menu.say(e.what(), true);
        }
        gl.set_smooth(!controls.map().player().sharp);
    }

    void update_preview(int w, int h) {
        const bool sharp = controls.map().player().sharp;
        if (w == preview_w && h == preview_h && sharp == preview_sharp && preview_version != 0) return;
        preview_w = w;
        preview_h = h;
        preview_sharp = sharp;
        ++preview_version;
        preview.make(game.data(), game_w, game_h, w, h, sharp);
    }

    void draw(float dt) {
        const auto [w, h] = gl.window_pixels();
        if (w <= 0 || h <= 0) return;
        if (!view.background_fits(w, h, look())) backdrop(w, h);
        anyui::Frame frame = menu.view();
        if (frame.page.preview.shown && !game.empty()) {
            update_preview(w, h);
            anyui::Preview &pv = frame.page.preview;
            preview.attach(pv);
            pv.version = preview_version;
        }
        view.render(canvas, frame, look(), dt);
        gl.present_picture(canvas.data(), canvas.width(), canvas.height());
    }

    std::vector<Action> pointer(const std::vector<desktop::Pointer> &events) {
        std::vector<Action> out;
        const auto [w, h] = gl.window_pixels();
        for (const desktop::Pointer &e : events) {
            using Kind = desktop::Pointer::Kind;
            using Phase = anyui::MenuView::PointerPhase;
            if (e.kind != Kind::Wheel && (!e.touch || drag_finger != e.finger)) {
                const Phase phase = e.kind == Kind::Down ? Phase::Down : e.kind == Kind::Up ? Phase::Up : Phase::Move;
                if (view.preview_pointer(menu.preview_state(), phase, e.x, e.y, e.touch)) continue;
            }
            const auto click = [&] {
                const auto a = menu.click(view.hit(e.x, e.y));
                out.insert(out.end(), a.begin(), a.end());
            };
            if (e.kind == Kind::Wheel) {
                view.wheel(e.wheel);
            } else if (e.kind == Kind::Move) {
                if (e.touch && drag_finger == e.finger) {
                    if (std::abs(e.y - press_y) > 12) dragged = true;
                    const float unit = 40.f * std::min(static_cast<float>(w) / 960.f, static_cast<float>(h) / 540.f) * 1.5f;
                    if (dragged) view.wheel((drag_y - e.y) / std::max(1.f, unit));
                    drag_y = e.y;
                } else if (!e.touch) {
                    view.hover(e.x, e.y);
                }
            } else if (e.kind == Kind::Down) {
                if (e.touch) {
                    drag_finger = e.finger;
                    drag_y = press_y = e.y;
                    dragged = false;
                } else {
                    click();
                }
            } else if (e.kind == Kind::Up && e.touch && drag_finger == e.finger) {
                drag_finger.reset();
                if (!dragged && e.x >= 0) click();
            }
        }
        return out;
    }

    // The frame-rate badge in the top-left corner of the window.
    void badge(int w, int h) {
        const bool rates = controls.map().player().show_fps && hz > 0;
        if (!rates && speed_percent == 100) return;
        const float u = std::max(0.75f, std::min(static_cast<float>(w) / 960.f, static_cast<float>(h) / 540.f));
        std::string text;
        if (rates) {
            char line[64];
            std::snprintf(line, sizeof line, "%.0f Hz  \xC2\xB7  %.0f fps", hz, fps);
            text = line;
        }
        if (speed_percent != 100) text += (text.empty() ? "" : "  \xC2\xB7  ") + speed::format(speed_percent);
        const float px = 14 * u;
        const int bw = static_cast<int>(bold_font().measure(text, px) + 24 * u), bh = static_cast<int>(28 * u);
        const int x = static_cast<int>(10 * u), y = static_cast<int>(10 * u);
        gl.edit_window(x, y, bw, bh, [&](std::vector<std::uint32_t> &pixels) {
            anyui::Canvas c(bw, bh);
            std::copy(pixels.begin(), pixels.end(), c.data());
            c.fill_round_rect({0, 0, static_cast<float>(bw), static_cast<float>(bh)}, static_cast<float>(bh) / 2,
                              anyui::Color::rgb(0x0A0C11, 0.72f));
            bold_font().draw(c, 12 * u, static_cast<float>(bh) / 2 + (bold_font().ascent(px) - bold_font().descent(px)) / 2, text,
                             px, anyui::Color::rgb(0xF2F4F8));
            std::copy(c.data(), c.data() + pixels.size(), pixels.begin());
        });
    }
};

MenuSession::MenuSession(gpu::GlHost &gl, desktop::Controls &controls, fs::path ini, fs::path memstick, std::string game_id,
                         std::string title)
    : s_(std::make_unique<State>(gl, controls, std::move(ini), std::move(memstick), std::move(game_id))) {
    s_->menu.set_game_title(std::move(title));
    gl.set_smooth(!controls.map().player().sharp);
    State *state = s_.get();
    gl.set_overlay([state](int w, int h) { state->badge(w, h); });
}

MenuSession::~MenuSession() { s_->gl.set_overlay({}); }

void MenuSession::set_speed(int percent) { s_->speed_percent = percent; }

void MenuSession::set_rates(double hz, double fps) {
    s_->hz = hz;
    s_->fps = fps;
}

void MenuSession::set_display(display::Settings *settings, std::function<void()> changed) {
    s_->display = settings;
    s_->display_changed = std::move(changed);
    s_->menu.set_display(settings);
}

void MenuSession::set_patches(bool *enabled) { s_->menu.set_patches(enabled); }

void MenuSession::set_cheats(cheats::CheatFile *file, std::string folder, std::string file_name) {
    s_->cheats = file;
    s_->menu.set_cheats(file, std::move(folder), std::move(file_name));
}

MenuSession::Result MenuSession::run(std::uint32_t address, std::uint32_t stride, std::uint32_t format) {
    State &s = *s_;
    std::vector<std::uint8_t> rgba;
    s.gl.capture(address, stride, format, rgba);
    s.game_w = kWidth * s.gl.scale();
    s.game_h = kHeight * s.gl.scale();
    s.game.resize(static_cast<std::size_t>(s.game_w) * static_cast<std::size_t>(s.game_h));
    s.preview_version = 0;
    std::memcpy(s.game.data(), rgba.data(), std::min(rgba.size(), s.game.size() * 4u));
    for (std::uint32_t &p : s.game) p |= 0xFF000000u;
    const auto [w, h] = s.gl.window_pixels();
    s.backdrop(std::max(w, 1), std::max(h, 1));
    s.menu.set_saves(scan_saves(s.memstick, s.game_id));
    s.fullscreen = s.gl.fullscreen();
    s.menu.open();
    s.view.open();

    Result result = Result::Resume;
    std::uint64_t last = SDL_GetTicksNS();
    while (s.menu.is_open()) {
        if (!s.controls.poll(s.gl)) return Result::WindowClosed;
        desktop::UiEvents events = s.controls.take_ui_events();
        MenuInput in;
        in.pad = s.controls.pad().buttons;
        in.keys = std::move(events.keys);
        in.buttons = std::move(events.buttons);
        in.shift = s.controls.shift_held();
        in.menu = events.menu;
        std::tie(in.stick_x, in.stick_y) = s.controls.right_stick();
        std::vector<Action> actions = s.menu.update(in);
        const auto clicks = s.pointer(events.pointers);
        actions.insert(actions.end(), clicks.begin(), clicks.end());
        for (const Action a : actions) {
            switch (a) {
            case Action::SettingsChanged: s.save_settings(); break;
            case Action::Fullscreen: s.gl.set_fullscreen(s.fullscreen); break;
            case Action::DisplayChanged:
                if (s.display_changed) s.display_changed();
                break;
            case Action::CheatsChanged:
                try {
                    if (s.cheats != nullptr) s.cheats->save();
                } catch (const std::exception &e) {
                    s.menu.say(e.what(), true);
                }
                break;
            case Action::Exit: result = Result::Exit; break;
            case Action::Reset: result = Result::Reset; break;
            case Action::Close: break;
            }
        }
        const std::uint64_t now = SDL_GetTicksNS();
        s.draw(std::min(0.1f, static_cast<float>(now - last) / 1e9f));
        last = now;
        SDL_DelayPrecise(s.frame_ns());
    }
    if (result == Result::Resume) {
        // The menu fades out over the game picture.
        s.view.close();
        for (int i = 0; i < 30 && !s.view.closed(); ++i) {
            s.draw(1.f / 60);
            SDL_DelayPrecise(s.frame_ns());
        }
    }
    return result;
}

} // namespace anypsp::menu
