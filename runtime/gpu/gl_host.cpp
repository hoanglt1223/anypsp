// SPDX-License-Identifier: GPL-2.0-or-later

#include "gl_host.hpp"

#include "psprecomp/common.hpp"

#include <SDL3/SDL.h>

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
    renderer_.reset();
    if (context_ != nullptr) SDL_GL_DestroyContext(context_);
    if (window_ != nullptr) SDL_DestroyWindow(window_);
    if (sdl_started_) SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void GlHost::start(int scale) {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) fail("SDL video initialisation failed");
    sdl_started_ = true;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 0);
    // The window only carries the context: it is created hidden and never shown.
    window_ = SDL_CreateWindow("anypsp-gpu", 16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (window_ == nullptr) fail("cannot create the hidden GL window");
    context_ = SDL_GL_CreateContext(window_);
    if (context_ == nullptr) fail("cannot create an OpenGL 3.3 core context");
    if (!SDL_GL_MakeCurrent(window_, context_)) fail("cannot make the GL context current");
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

} // namespace anypsp::gpu
