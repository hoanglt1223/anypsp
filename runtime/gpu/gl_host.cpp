// SPDX-License-Identifier: GPL-2.0-or-later

#include "gl_host.hpp"

#include "display.hpp"

#include "psprecomp/common.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>

namespace anypsp::gpu {
namespace {

constexpr int kWidth = 480, kHeight = 272;
constexpr int kSmallWidth = 60, kSmallHeight = 34;  // copies compared for scene cuts

constexpr const char *kBlendVertex = R"(#version 330 core
out vec2 uv;
void main() {
    uv = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
})";
constexpr const char *kBlendFragment = R"(#version 330 core
in vec2 uv;
out vec4 color;
uniform sampler2D cur;
uniform sampler2D prev;
void main() { color = vec4(mix(texture(cur, uv).rgb, texture(prev, uv).rgb, 0.5), 1.0); }
)";

GLuint compile(GLenum type, const char *source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) throw psprecomp::Error("smoothing shader does not compile");
    return shader;
}

void make_target(int width, int height, GLuint &fbo, GLuint &texture) {
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw psprecomp::Error("output framebuffer incomplete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

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

    make_target(kWidth * scale_, kHeight * scale_, out_fbo_, out_texture_);
    make_target(kWidth * scale_, kHeight * scale_, prev_fbo_, prev_texture_);
    for (int i = 0; i < 2; ++i) make_target(kSmallWidth, kSmallHeight, small_fbo_[i], small_texture_[i]);
    {
        const GLuint vs = compile(GL_VERTEX_SHADER, kBlendVertex), fs = compile(GL_FRAGMENT_SHADER, kBlendFragment);
        blend_program_ = glCreateProgram();
        glAttachShader(blend_program_, vs);
        glAttachShader(blend_program_, fs);
        glLinkProgram(blend_program_);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(blend_program_, GL_LINK_STATUS, &ok);
        if (!ok) throw psprecomp::Error("smoothing shader does not link on " + device_);
        glUseProgram(blend_program_);
        glUniform1i(glGetUniformLocation(blend_program_, "cur"), 0);
        glUniform1i(glGetUniformLocation(blend_program_, "prev"), 1);
        glUseProgram(0);
        glGenVertexArrays(1, &blend_vao_);
    }
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
    render_output(address, stride, format);
    show(false);
}

void GlHost::render_output(std::uint32_t address, std::uint32_t stride, std::uint32_t format) {
    renderer_->present(address, stride, format, out_fbo_, kWidth * scale_, kHeight * scale_);
}

void GlHost::keep_previous() {
    const int width = kWidth * scale_, height = kHeight * scale_;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, out_fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_fbo_);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

double GlHost::difference() {
    const int width = kWidth * scale_, height = kHeight * scale_;
    const GLuint sources[2] = {out_fbo_, prev_fbo_};
    glDisable(GL_SCISSOR_TEST);
    for (int i = 0; i < 2; ++i) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, sources[i]);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, small_fbo_[i]);
        glBlitFramebuffer(0, 0, width, height, 0, 0, kSmallWidth, kSmallHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, small_fbo_[i]);
        small_[i].resize(static_cast<std::size_t>(kSmallWidth) * kSmallHeight * 4u);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, kSmallWidth, kSmallHeight, GL_RGBA, GL_UNSIGNED_BYTE, small_[i].data());
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return display::mean_difference(small_[0], small_[1]);
}

void GlHost::show(bool blend_with_previous) {
    const int width = kWidth * scale_, height = kHeight * scale_;
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
    if (blend_with_previous) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(x, y, w, h);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glUseProgram(blend_program_);
        glBindVertexArray(blend_vao_);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, prev_texture_);
        glBindSampler(1, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, out_texture_);
        glBindSampler(0, 0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        glUseProgram(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
    } else {
        glBlitFramebuffer(0, 0, width, height, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (const GLenum error = glGetError(); error != GL_NO_ERROR)
        throw psprecomp::Error("GL error " + std::to_string(error) + " while presenting a frame");
    SDL_GL_SwapWindow(window_);
}

void GlHost::set_fullscreen(bool on) {
    if (SDL_SetWindowFullscreen(window_, on)) fullscreen_ = on;
}

} // namespace anypsp::gpu
