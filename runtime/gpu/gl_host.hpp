#pragma once

// Desktop OpenGL 3.3 core host for the GE's GL backend. By default the context
// lives on a hidden SDL window that is never shown; frames are presented into
// an offscreen framebuffer and read back. With a visible window, frames are
// also scaled into the window (letterboxed) and swapped with vsync.
// All calls must come from the thread that called start().

#include "ge_gl.hpp"

#include <cstdint>
#include <memory>
#include <string>
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
    void set_fullscreen(bool on);
    // Refresh rate of the monitor the window is on, or 0 when unknown.
    [[nodiscard]] double refresh_rate() const;
    // Swaps wait for `interval` vblanks (0: no wait). False when the driver refuses.
    bool set_swap_interval(int interval);
    [[nodiscard]] bool fullscreen() const noexcept { return fullscreen_; }
    [[nodiscard]] SDL_Window *window() const noexcept { return window_; }

    [[nodiscard]] pspweb::GeGl &renderer() { return *renderer_; }
    [[nodiscard]] const std::string &device() const noexcept { return device_; }
    [[nodiscard]] int scale() const noexcept { return scale_; }

    // Presents the framebuffer at `address` and reads it back as RGBA rows, top
    // row first, (480 x 272) * scale. Alpha is opaque.
    void capture(std::uint32_t address, std::uint32_t stride, std::uint32_t format, std::vector<std::uint8_t> &rgba);

private:
    psprecomp::GuestMemory &memory_;
    SDL_Window *window_{};
    SDL_GLContextState *context_{};
    bool sdl_started_{};
    std::unique_ptr<pspweb::GeGl> renderer_;
    std::uint32_t out_fbo_{}, out_texture_{};
    std::uint32_t prev_fbo_{}, prev_texture_{};
    std::uint32_t small_fbo_[2]{}, small_texture_[2]{};
    std::uint32_t blend_program_{}, blend_vao_{};
    std::vector<std::uint8_t> small_[2];
    int scale_{1};
    bool fullscreen_{};
    std::string device_;
    std::vector<std::uint8_t> readback_;
};

} // namespace anypsp::gpu
