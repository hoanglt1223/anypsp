// SPDX-License-Identifier: GPL-2.0-or-later

#include "gl_host.hpp"

#include "psprecomp/common.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>

namespace anypsp::gpu {
namespace {

constexpr int kWidth = 480, kHeight = 272;

[[noreturn]] void fail(const std::string &what) {
    throw psprecomp::Error(what + (SDL_GetError()[0] != '\0' ? std::string(": ") + SDL_GetError() : std::string()));
}

} // namespace

GlHost::GlHost(psprecomp::GuestMemory &memory) : memory_(memory) {}

GlHost::~GlHost() {
    if (context_ != nullptr && ui_fbo_ != 0u) {
        glDeleteFramebuffers(1, &ui_fbo_);
        glDeleteTextures(1, &ui_texture_);
    }
    renderer_.reset();
    if (context_ != nullptr) SDL_GL_DestroyContext(context_);
    if (window_ != nullptr) SDL_DestroyWindow(window_);
    if (sdl_started_) SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void GlHost::start(int scale, const std::string &title) {
    const bool visible = !title.empty();
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) fail("SDL video initialisation failed");
    sdl_started_ = true;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, visible ? 1 : 0);
    // Headless, the window only carries the context: it is created hidden and never shown.
    const int window_scale = std::max(scale, 2);
    window_ = visible ? SDL_CreateWindow(title.c_str(), kWidth * window_scale, kHeight * window_scale,
                                         SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY)
                      : SDL_CreateWindow("anypsp-gpu", 16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (window_ == nullptr) fail("cannot create the GL window");
    if (visible) SDL_SetWindowMinimumSize(window_, kWidth, kHeight);
    context_ = SDL_GL_CreateContext(window_);
    if (context_ == nullptr) fail("cannot create an OpenGL 3.3 core context");
    if (!SDL_GL_MakeCurrent(window_, context_)) fail("cannot make the GL context current");
    // No vsync: the caller paces vblanks at 59.94 Hz with a timer, and waiting for
    // the monitor as well let the two waits add up (about 32 vblanks per second
    // on a 60 Hz display).  The desktop compositor keeps windowed output tear-free.
    SDL_GL_SetSwapInterval(0);
    if (const char *missing = load_gl([](const char *name) { return reinterpret_cast<GlProc>(SDL_GL_GetProcAddress(name)); }))
        throw psprecomp::Error(std::string("OpenGL function missing: ") + missing);
    const auto *vendor = reinterpret_cast<const char *>(glGetString(GL_VENDOR));
    const auto *name = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
    const auto *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
    device_ = std::string(vendor != nullptr ? vendor : "?") + " " + (name != nullptr ? name : "?") + ", GL " +
              (version != nullptr ? version : "?");

    renderer_ = std::make_unique<pspweb::GeGl>(memory_);
    if (!renderer_->init()) throw psprecomp::Error("GE GL backend failed to initialise on " + device_);
    scale_ = renderer_->set_render_scale(scale);

    glGenTextures(1, &out_texture_);
    glBindTexture(GL_TEXTURE_2D, out_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kWidth * scale_, kHeight * scale_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &out_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, out_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out_texture_, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw psprecomp::Error("offscreen output framebuffer incomplete on " + device_);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (const GLenum error = glGetError(); error != GL_NO_ERROR)
        throw psprecomp::Error("GL error " + std::to_string(error) + " while starting on " + device_);
}

void GlHost::capture(std::uint32_t address, std::uint32_t stride, std::uint32_t format, std::vector<std::uint8_t> &rgba) {
    const int width = kWidth * scale_, height = kHeight * scale_;
    renderer_->present(address, stride, format, out_fbo_, width, height);
    const auto row = static_cast<std::size_t>(width) * 4u;
    readback_.resize(row * static_cast<std::size_t>(height));
    glBindFramebuffer(GL_FRAMEBUFFER, out_fbo_);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, readback_.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (const GLenum error = glGetError(); error != GL_NO_ERROR)
        throw psprecomp::Error("GL error " + std::to_string(error) + " while capturing a frame");
    rgba.resize(readback_.size());
    for (int y = 0; y < height; ++y)
        std::memcpy(&rgba[static_cast<std::size_t>(y) * row], &readback_[static_cast<std::size_t>(height - 1 - y) * row], row);
}

void GlHost::present_to_window(std::uint32_t address, std::uint32_t stride, std::uint32_t format) {
    const int width = kWidth * scale_, height = kHeight * scale_;
    renderer_->present(address, stride, format, out_fbo_, width, height);
    int window_w = 0, window_h = 0;
    SDL_GetWindowSizeInPixels(window_, &window_w, &window_h);
    // Largest 480:272 rectangle that fits, centred.
    int w = window_w, h = window_w * kHeight / kWidth;
    if (h > window_h) {
        h = window_h;
        w = window_h * kWidth / kHeight;
    }
    const int x = (window_w - w) / 2, y = (window_h - h) / 2;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, out_fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, window_w, window_h);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlitFramebuffer(0, 0, width, height, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, smooth_ ? GL_LINEAR : GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (const GLenum error = glGetError(); error != GL_NO_ERROR)
        throw psprecomp::Error("GL error " + std::to_string(error) + " while presenting a frame");
    if (overlay_) overlay_(window_w, window_h);
    SDL_GL_SwapWindow(window_);
}

std::pair<int, int> GlHost::window_pixels() const {
    int w = 0, h = 0;
    if (window_ != nullptr) SDL_GetWindowSizeInPixels(window_, &w, &h);
    return {w, h};
}

void GlHost::blit_to_window(const std::uint32_t *pixels, int width, int height, int x, int y) {
    if (ui_fbo_ == 0u) {
        glGenTextures(1, &ui_texture_);
        glGenFramebuffers(1, &ui_fbo_);
    }
    // The GE renderer's texture binding is put back afterwards.
    GLint previous_texture = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glBindTexture(GL_TEXTURE_2D, ui_texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (width > ui_width_ || height > ui_height_) {
        ui_width_ = std::max(width, ui_width_);
        ui_height_ = std::max(height, ui_height_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ui_width_, ui_height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, ui_fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ui_texture_, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw psprecomp::Error("menu framebuffer incomplete on " + device_);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));
    const auto [window_w, window_h] = window_pixels();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, ui_fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    // Texture row 0 is the picture's top: flip it into GL's bottom-up window.
    glBlitFramebuffer(0, 0, width, height, x, window_h - y, x + width, window_h - y - height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    (void)window_w;
}

void GlHost::present_picture(const std::uint32_t *pixels, int width, int height) {
    blit_to_window(pixels, width, height, 0, 0);
    if (const GLenum error = glGetError(); error != GL_NO_ERROR)
        throw psprecomp::Error("GL error " + std::to_string(error) + " while presenting the menu");
    SDL_GL_SwapWindow(window_);
}

void GlHost::edit_window(int x, int y, int w, int h, const std::function<void(std::vector<std::uint32_t> &)> &edit) {
    const auto [window_w, window_h] = window_pixels();
    x = std::clamp(x, 0, window_w);
    y = std::clamp(y, 0, window_h);
    w = std::min(w, window_w - x);
    h = std::min(h, window_h - y);
    if (w <= 0 || h <= 0) return;
    std::vector<std::uint32_t> bottom_up(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(x, window_h - y - h, w, h, GL_RGBA, GL_UNSIGNED_BYTE, bottom_up.data());
    ui_rows_.resize(bottom_up.size());
    for (int row = 0; row < h; ++row)
        std::memcpy(&ui_rows_[static_cast<std::size_t>(row) * static_cast<std::size_t>(w)],
                    &bottom_up[static_cast<std::size_t>(h - 1 - row) * static_cast<std::size_t>(w)], static_cast<std::size_t>(w) * 4u);
    edit(ui_rows_);
    blit_to_window(ui_rows_.data(), w, h, x, y);
}

void GlHost::set_fullscreen(bool on) {
    if (SDL_SetWindowFullscreen(window_, on)) fullscreen_ = on;
}

} // namespace anypsp::gpu
