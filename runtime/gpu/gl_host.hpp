#pragma once

// Offscreen desktop OpenGL 3.3 core host for the GE's GL backend. The context
// lives on a hidden SDL window that is never shown; frames are presented into
// an offscreen framebuffer and read back. No audio subsystem is initialised.
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
    void start(int scale);

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
    int scale_{1};
    std::string device_;
    std::vector<std::uint8_t> readback_;
};

} // namespace anypsp::gpu
