#pragma once

// Desktop OpenGL 3.3 core host for the GE's GL backend. By default the context
// lives on a hidden SDL window that is never shown; frames are presented into
// an offscreen framebuffer and read back. With a visible window, frames are
// also scaled into the window (letterboxed) and swapped with vsync.
// All calls must come from the thread that called start().

#include "ge_gl.hpp"
#include "in_between.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct SDL_Window;
struct SDL_GLContextState;

namespace anypsp::gpu {

class GlHost {
public:
    explicit GlHost(psprecomp::GuestMemory &memory);
    ~GlHost();
    GlHost(const GlHost &) = delete;
    GlHost &operator=(const GlHost &) = delete;

    // Creates the context and the renderer at `scale` times the PSP resolution.
    // Throws psprecomp::Error with the reason on failure.
    // `title` non-empty opens a visible, resizable window with that title.
    void start(int scale, const std::string &title = {});

    // Draws the framebuffer at `address` into the visible window and swaps.
    void present_to_window(std::uint32_t address, std::uint32_t stride, std::uint32_t format);
    // The same in steps, for smoothing: render_output draws the framebuffer into the output
    // picture, keep_previous saves the output picture before the next one replaces it,
    // difference compares the two (0 same .. 1 opposite, from small copies), and show scales
    // the output picture (or a 50/50 blend with the saved one) into the window and swaps.
    void render_output(std::uint32_t address, std::uint32_t stride, std::uint32_t format);
    void keep_previous();
    [[nodiscard]] double difference();
    void show(bool blend_with_previous);
    // Frame interpolation (host/ge_interp.hpp): creates the second renderer that draws the
    // in-between picture and returns what the GE draws through. Call once, after start().
    pspweb::InterpolatingRenderer &enable_in_between();
    [[nodiscard]] bool has_in_between() const noexcept { return tee_ != nullptr; }
    // Draws the in-between picture of the framebuffer at `address` into the output picture.
    void render_in_between(std::uint32_t address, std::uint32_t stride, std::uint32_t format);
    void set_fullscreen(bool on);
    // Refresh rate of the monitor the window is on, or 0 when unknown.
    [[nodiscard]] double refresh_rate() const;
    // Swaps wait for `interval` vblanks (0: no wait). False when the driver refuses.
    bool set_swap_interval(int interval);
    [[nodiscard]] bool fullscreen() const noexcept { return fullscreen_; }
    // How the picture is scaled into the window: linear (smooth, the default) or nearest.
    void set_smooth(bool on) noexcept { smooth_ = on; }
    // Runs after the game picture is drawn into the window and before the swap, with the
    // window's size in pixels (the frame-rate badge).
    void set_overlay(std::function<void(int, int)> overlay) { overlay_ = std::move(overlay); }
    // The window's drawable size in pixels.
    [[nodiscard]] std::pair<int, int> window_pixels() const;
    // Shows a picture of window_pixels() size (R, G, B, A bytes, top row first) and swaps.
    void present_picture(const std::uint32_t *pixels, int width, int height);
    // Reads w x h pixels at (x, y) of the window's back buffer (top row first), lets `edit`
    // change them, and writes them back.
    void edit_window(int x, int y, int w, int h, const std::function<void(std::vector<std::uint32_t> &)> &edit);
    [[nodiscard]] SDL_Window *window() const noexcept { return window_; }

    [[nodiscard]] pspweb::GeGl &renderer() { return *renderer_; }
    [[nodiscard]] const std::string &device() const noexcept { return device_; }
    [[nodiscard]] int scale() const noexcept { return scale_; }

    // Presents the framebuffer at `address` and reads it back as RGBA rows, top
    // row first, (480 x 272) * scale. Alpha is opaque.
    void capture(std::uint32_t address, std::uint32_t stride, std::uint32_t format, std::vector<std::uint8_t> &rgba);
    // The same for the in-between picture.
    void capture_in_between(std::uint32_t address, std::uint32_t stride, std::uint32_t format, std::vector<std::uint8_t> &rgba);

private:
    psprecomp::GuestMemory &memory_;
    SDL_Window *window_{};
    SDL_GLContextState *context_{};
    bool sdl_started_{};
    std::unique_ptr<pspweb::GeGl> renderer_;
    std::unique_ptr<pspweb::GeGl> in_between_;
    std::unique_ptr<InBetweenRenderer> tee_;
    void read_output(std::vector<std::uint8_t> &rgba);
    std::uint32_t out_fbo_{}, out_texture_{};
    std::uint32_t prev_fbo_{}, prev_texture_{};
    std::uint32_t small_fbo_[2]{}, small_texture_[2]{};
    std::uint32_t blend_program_{}, blend_vao_{};
    std::vector<std::uint8_t> small_[2];
    int scale_{1};
    bool fullscreen_{};
    bool smooth_{true};
    std::function<void(int, int)> overlay_;
    std::uint32_t ui_fbo_{}, ui_texture_{};
    int ui_width_{}, ui_height_{};
    std::vector<std::uint32_t> ui_rows_;
    // Uploads top-first pixels and copies them flipped into the window at (x, y) from the top.
    void blit_to_window(const std::uint32_t *pixels, int width, int height, int x, int y);
    std::string device_;
    std::vector<std::uint8_t> readback_;
};

} // namespace anypsp::gpu
