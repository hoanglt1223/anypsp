#include "ge_gl.hpp"

#include <algorithm>
#include <iostream>
#include <string>

namespace pspweb {
namespace {

constexpr std::size_t kMaxTextures = 4096u;
constexpr std::uint32_t kMaxSurfaceStride = 1024u;
constexpr GLsizei kStagingWidth = 2048, kStagingHeight = 512;
constexpr std::size_t kIndexBufferPool = 512u;

// The shaders in shaders/ are shared with the Vulkan backend; this prelude maps their
// macros to GLSL ES 3.00 (desktop builds swap in GLSL 3.30, which accepts the same code).
const char *kPrelude =
#ifdef ANYPSP_GL_DESKTOP
    "#version 330 core\n"
    "#define VARYING_IN_LINEAR(n) noperspective in\n"
    "#define VARYING_OUT_LINEAR(n) noperspective out\n"
#else
    // GLSL ES has no noperspective: colour is interpolated with perspective there.
    "#version 300 es\n"
    "#define VARYING_IN_LINEAR(n) in\n"
    "#define VARYING_OUT_LINEAR(n) out\n"
#endif
    "precision highp float;\n"
    "precision highp int;\n"
    "#define VERTEX_ID gl_VertexID\n"
    "#define VARYING_IN(n) in\n"
    "#define VARYING_OUT(n) out\n"
    "#define SAMPLER(n) uniform\n"
    "#define PARAMS(type, name, count) uniform type name[count]\n"
    "#define DEPTH_NDC(z) ((z) * 2.0 - 1.0)\n"
    "#define PRESENT_ROW(y) (1.0 - (y))\n";

const char *kGeVertex =
#include "shaders/ge.vert.inc"
    ;
const char *kGeFragment =
#include "shaders/ge.frag.inc"
    ;
const char *kFullscreenVertex =
#include "shaders/fullscreen.vert.inc"
    ;
const char *kReinterpretFragment =
#include "shaders/reinterpret.frag.inc"
    ;
const char *kAlphaToStencilFragment =
#include "shaders/alpha_to_stencil.frag.inc"
    ;
const char *kFillFragment =
#include "shaders/fill.frag.inc"
    ;
const char *kPresentVertex =
#include "shaders/present.vert.inc"
    ;
const char *kPresentFragment =
#include "shaders/present.frag.inc"
    ;

GLuint compile(GLenum type, const char *body) {
    const GLuint shader = glCreateShader(type);
    const std::string source = std::string(kPrelude) + body;
    const char *text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof log, nullptr, log);
        std::cerr << "[gl] shader error: " << log << "\n";
    }
    return shader;
}

GLuint link(const char *vs, const char *fs) {
    const GLuint program = glCreateProgram();
    const GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(program, v);
    glAttachShader(program, f);
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(program, sizeof log, nullptr, log);
        std::cerr << "[gl] link error: " << log << "\n";
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return program;
}

GLenum compare_func(GpuCompare f) {
    static constexpr GLenum table[8] = {GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL};
    return table[static_cast<int>(f) & 7];
}

GLenum stencil_op(GpuStencilOp op) {
    static constexpr GLenum table[6] = {GL_KEEP, GL_ZERO, GL_REPLACE, GL_INVERT, GL_INCR, GL_DECR};
    return table[static_cast<int>(op)];
}

GLenum blend_factor(GpuBlendFactor f) {
    static constexpr GLenum table[11] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_DST_COLOR,
                                         GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA,
                                         GL_ONE_MINUS_DST_ALPHA, GL_CONSTANT_COLOR};
    return table[static_cast<int>(f)];
}

GLenum blend_equation(GpuBlendOp op) {
    static constexpr GLenum table[5] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX};
    return table[static_cast<int>(op)];
}

} // namespace

GeGl::GeGl(psprecomp::GuestMemory &memory)
    : GeSurfaces(memory, std::make_unique<GlGpu>()), gl_(static_cast<GlGpu &>(gpu())) {}

GlGpu::~GlGpu() = default;

void GlGpu::use_program(GLuint program) {
    if (cache_.program == program) return;
    glUseProgram(program);
    cache_.program = program;
    cache_.params_valid = false;
}

void GlGpu::bind_framebuffer(GLuint fbo) {
    if (cache_.fbo == fbo) return;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    cache_.fbo = fbo;
}

void GlGpu::bind_texture0(GLuint texture) {
    if (cache_.texture == texture) return;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    cache_.texture = texture;
}

void GlGpu::set_enabled(GLenum cap, int &cached, bool on) {
    if (cached == static_cast<int>(on)) return;
    if (on) glEnable(cap);
    else glDisable(cap);
    cached = static_cast<int>(on);
}

bool GlGpu::init() {
    program_ = link(kGeVertex, kGeFragment);
    u_params_ = glGetUniformLocation(program_, "u_p");
    present_program_ = link(kPresentVertex, kPresentFragment);
    u_present_ = glGetUniformLocation(present_program_, "u_present");
    reinterpret_program_ = link(kFullscreenVertex, kReinterpretFragment);
    u_ri_ = glGetUniformLocation(reinterpret_program_, "u_ri");
    stencil_program_ = link(kFullscreenVertex, kAlphaToStencilFragment);
    u_st_ = glGetUniformLocation(stencil_program_, "u_st");
    fill_program_ = link(kFullscreenVertex, kFillFragment);
    u_fill_ = glGetUniformLocation(fill_program_, "u_fill");
    // Every sampler reads texture unit 0.
    for (GLuint program : {program_, present_program_, reinterpret_program_, stencil_program_}) {
        glUseProgram(program);
        for (const char *name : {"u_tex", "u_src"}) {
            const GLint location = glGetUniformLocation(program, name);
            if (location >= 0) glUniform1i(location, 0);
        }
    }
    glUseProgram(0);

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    index_buffers_.resize(kIndexBufferPool);
    glGenBuffers(static_cast<GLsizei>(index_buffers_.size()), index_buffers_.data());
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(GlVertex), reinterpret_cast<void *>(offsetof(GlVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GlVertex), reinterpret_cast<void *>(offsetof(GlVertex, u)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GlVertex), reinterpret_cast<void *>(offsetof(GlVertex, r)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(GlVertex), reinterpret_cast<void *>(offsetof(GlVertex, q)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(GlVertex), reinterpret_cast<void *>(offsetof(GlVertex, fog)));
    glGenVertexArrays(1, &present_vao_);
    glGenTextures(1, &scratch_texture_);
    glGenTextures(1, &present_texture_);
    // Samplers for every filter/wrap combination: index = linear | clamp_u << 1 | clamp_v << 2.
    glGenSamplers(8, samplers_);
    for (int i = 0; i < 8; ++i) {
        const GLint filter = (i & 1) != 0 ? GL_LINEAR : GL_NEAREST;
        glSamplerParameteri(samplers_[i], GL_TEXTURE_MIN_FILTER, filter);
        glSamplerParameteri(samplers_[i], GL_TEXTURE_MAG_FILTER, filter);
        glSamplerParameteri(samplers_[i], GL_TEXTURE_WRAP_S, (i & 2) != 0 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glSamplerParameteri(samplers_[i], GL_TEXTURE_WRAP_T, (i & 4) != 0 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    }
    // Bound whenever a draw samples nothing, so no framebuffer texture stays
    // bound to the sampler (WebGL rejects that as a feedback loop).
    glGenTextures(1, &blank_texture_);
    glBindTexture(GL_TEXTURE_2D, blank_texture_);
    const std::uint32_t white = 0xFFFFFFFFu;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    glGenTextures(1, &staging_texture_);
    glBindTexture(GL_TEXTURE_2D, staging_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kStagingWidth, kStagingHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &staging_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, staging_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, staging_texture_, 0);
    glGenFramebuffers(1, &stencil_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, stencil_fbo_);
    const GLenum none = GL_NONE;
    glDrawBuffers(1, &none);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindVertexArray(0);
    return glGetError() == GL_NO_ERROR;
}

int GlGpu::max_scale() const {
    GLint max_texture = 0, max_renderbuffer = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture);
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_renderbuffer);
    const GLint limit = std::min(max_texture, max_renderbuffer);
    int scale = 4;
    while (scale > 1 && static_cast<GLint>(kMaxSurfaceStride) * scale > limit) --scale;
    return scale;
}

void GlGpu::set_scale(int scale) {
    scale_ = scale;
    reset_cache();
}

void GlGpu::blit(GLuint from, const GLint src[4], GLuint to, const GLint dst[4], GLbitfield mask) {
    set_enabled(GL_SCISSOR_TEST, cache_.scissor_test, false);
    if (cache_.color_mask != 15) {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        cache_.color_mask = 15;
    }
    if ((mask & GL_DEPTH_BUFFER_BIT) != 0u && cache_.depth_mask != 1) {
        glDepthMask(GL_TRUE);
        cache_.depth_mask = 1;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, from);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, to);
    glBlitFramebuffer(src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], mask, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    cache_.fbo = 0u;
}

GpuTarget GlGpu::create_target(int width, int height) {
    GpuTarget handle;
    if (!free_targets_.empty()) {
        handle = free_targets_.back();
        free_targets_.pop_back();
    } else {
        targets_.emplace_back();
        handle = static_cast<GpuTarget>(targets_.size());
    }
    Target &t = at(handle);
    t.width = width;
    t.height = height;
    glGenTextures(1, &t.color);
    glBindTexture(GL_TEXTURE_2D, t.color);
    const GLsizei width_px = width * scale_, height_px = height * scale_;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_px, height_px, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &t.depth);
    glBindRenderbuffer(GL_RENDERBUFFER, t.depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width_px, height_px);
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        std::cerr << "[gl] incomplete framebuffer " << width << "x" << height << "\n";
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFFu);
    glClearDepthf(0.0f);
    glClearStencil(0);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    reset_cache();
    return handle;
}

void GlGpu::destroy_target(GpuTarget target) {
    Target &t = at(target);
    glDeleteFramebuffers(1, &t.fbo);
    glDeleteTextures(1, &t.color);
    glDeleteRenderbuffers(1, &t.depth);
    t = Target{};
    free_targets_.push_back(target);
    reset_cache();
}

void GlGpu::copy_target(GpuTarget from, int fx, int fy, GpuTarget to, int tx, int ty, int w, int h) {
    const GLint src[4] = {fx * scale_, fy * scale_, (fx + w) * scale_, (fy + h) * scale_};
    const GLint dst[4] = {tx * scale_, ty * scale_, (tx + w) * scale_, (ty + h) * scale_};
    blit(at(from).fbo, src, at(to).fbo, dst, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    reset_cache();
}

void GlGpu::read_pixels(GpuTarget target, const GeRect &r, std::uint8_t *rgba) {
    const GLint w = r.width(), h = r.height();
    if (scale_ == 1) {
        glBindFramebuffer(GL_FRAMEBUFFER, at(target).fbo);
        glReadPixels(r.x0, r.y0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    } else {
        // Shrink to native size first; VRAM holds PSP pixels.
        const GLint sw = std::min(w, static_cast<GLint>(kStagingWidth)), sh = std::min(h, static_cast<GLint>(kStagingHeight));
        const GLint src[4] = {r.x0 * scale_, r.y0 * scale_, (r.x0 + sw) * scale_, (r.y0 + sh) * scale_};
        const GLint dst[4] = {0, 0, sw, sh};
        blit(at(target).fbo, src, staging_fbo_, dst, GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, staging_fbo_);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    cache_.fbo = ~0u;
}

void GlGpu::write_pixels(GpuTarget target, const GeRect &r, const std::uint8_t *rgba) {
    const GLint x0 = r.x0, y0 = r.y0, w = r.width(), h = r.height();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (scale_ == 1) {
        glBindTexture(GL_TEXTURE_2D, at(target).color);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    } else {
        // Upload at native size, then stretch into the surface.
        const GLint sw = std::min(w, static_cast<GLint>(kStagingWidth)), sh = std::min(h, static_cast<GLint>(kStagingHeight));
        glBindTexture(GL_TEXTURE_2D, staging_texture_);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, w);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        const GLint src[4] = {0, 0, sw, sh};
        const GLint dst[4] = {x0 * scale_, y0 * scale_, (x0 + sw) * scale_, (y0 + sh) * scale_};
        blit(staging_fbo_, src, at(target).fbo, dst, GL_COLOR_BUFFER_BIT);
    }
    cache_.texture = ~0u;
}

void GlGpu::begin_pass(GLuint fbo, const Target &t, const GeRect &r) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, t.width * scale_, t.height * scale_);
    glEnable(GL_SCISSOR_TEST);
    glScissor(r.x0 * scale_, r.y0 * scale_, r.width() * scale_, r.height() * scale_);
}

void GlGpu::reinterpret(GpuTarget into, GpuTarget from, const GeRect &hit, const GpuReinterpret &params) {
    begin_pass(at(into).fbo, at(into), hit);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(reinterpret_program_);
    glBindVertexArray(present_vao_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, at(from).color);
    glBindSampler(0, samplers_[0]);
    const GLint values[8] = {params.mode, params.format, params.delta, params.row_bytes,
                             params.src_width, params.src_height, scale_, 0};
    glUniform4iv(u_ri_, 2, values);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    reset_cache();
}

void GlGpu::fill_alpha(GpuTarget target, const GeRect &r, std::uint8_t value) {
    begin_pass(at(target).fbo, at(target), r);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, static_cast<float>(value) / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    reset_cache();
}

void GlGpu::stencil_to_alpha(GpuTarget target, const GeRect &r, std::uint32_t bits) {
    begin_pass(at(target).fbo, at(target), r);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0u);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glUseProgram(fill_program_);
    glBindVertexArray(present_vao_);
    for (GLuint bit = 0; bit < 8u; ++bit) {
        if ((bits & (1u << bit)) == 0u) continue;
        glStencilFunc(GL_EQUAL, static_cast<GLint>(1u << bit), 1u << bit);
        glUniform4f(u_fill_, 0.0f, 0.0f, 0.0f, static_cast<float>(1u << bit) / 255.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    reset_cache();
}

void GlGpu::alpha_to_stencil(GpuTarget target, const GeRect &r, std::uint32_t bits) {
    const Target &t = at(target);
    begin_pass(stencil_fbo_, t, r);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.depth);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDepthMask(GL_FALSE);
    glStencilMask(0xFFu);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 0xFF, 0xFFu);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glUseProgram(stencil_program_);
    glBindVertexArray(present_vao_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t.color);
    glBindSampler(0, samplers_[0]);
    for (GLuint bit = 0; bit < 8u; ++bit) {
        if ((bits & (1u << bit)) == 0u) continue;
        glStencilMask(1u << bit);
        glUniform4i(u_st_, static_cast<GLint>(bit), 0, 0, 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
    reset_cache();
}

GLuint GlGpu::texture_for(const GpuDraw &d) {
    switch (d.source) {
    case GpuDraw::Source::None: return 0u;
    case GpuDraw::Source::Target: return at(d.texture_target).color;
    case GpuDraw::Source::Snapshot: {
        // Sampling the surface being drawn: read from a copy of the sampled part.
        const Target &source = at(d.texture_target);
        bind_texture0(scratch_texture_);
        const GLsizei width = source.width * scale_, height = source.height * scale_;
        if (scratch_width_ != width || scratch_height_ != height) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            scratch_width_ = width;
            scratch_height_ = height;
        }
        const GeRect &w = d.snapshot;
        if (!w.empty()) {
            bind_framebuffer(source.fbo);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, w.x0 * scale_, w.y0 * scale_, w.x0 * scale_, w.y0 * scale_,
                                w.width() * scale_, w.height() * scale_);
        }
        return scratch_texture_;
    }
    case GpuDraw::Source::Texels: break;
    }
    if (textures_.size() > kMaxTextures) {
        for (auto &[key, texture] : textures_) glDeleteTextures(1, &texture.id);
        textures_.clear();
        cache_.texture = ~0u;
    }
    GlTexture &texture = textures_[d.texture_identity];
    if (texture.id == 0u) glGenTextures(1, &texture.id);
    if (texture.hash != d.texture_hash || texture.width != d.tex_width || texture.height != d.tex_height) {
        bind_texture0(texture.id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(d.tex_width), static_cast<GLsizei>(d.tex_height), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, d.texels);
        texture.hash = d.texture_hash;
        texture.width = d.tex_width;
        texture.height = d.tex_height;
    }
    return texture.id;
}

void GlGpu::forget_texture(const void *identity) {
    const auto it = textures_.find(identity);
    if (it == textures_.end()) return;
    if (cache_.texture == it->second.id) cache_.texture = ~0u;
    glDeleteTextures(1, &it->second.id);
    textures_.erase(it);
}

void GlGpu::draw(const GpuDraw &d) {
    const Target &target = at(d.target);
    use_program(program_);
    if (cache_.vao != vao_) {
        glBindVertexArray(vao_);
        cache_.vao = vao_;
    }
    if (!cache_.params_valid || !(cache_.params == d.params)) {
        glUniform4fv(u_params_, 8, d.params.v.data());
        cache_.params = d.params;
        cache_.params_valid = true;
    }
    const GLuint texture = texture_for(d);

    bind_framebuffer(target.fbo);
    const GLint viewport_w = target.width * scale_, viewport_h = target.height * scale_;
    if (cache_.viewport_w != viewport_w || cache_.viewport_h != viewport_h) {
        glViewport(0, 0, viewport_w, viewport_h);
        cache_.viewport_w = viewport_w;
        cache_.viewport_h = viewport_h;
    }
    set_enabled(GL_SCISSOR_TEST, cache_.scissor_test, true);
    const GLint scissor[4] = {d.scissor.x0 * scale_, d.scissor.y0 * scale_, d.scissor.width() * scale_, d.scissor.height() * scale_};
    if (!std::equal(scissor, scissor + 4, cache_.scissor)) {
        glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
        std::copy(scissor, scissor + 4, cache_.scissor);
    }
    const GpuPipeline &p = d.pipeline;
    set_enabled(GL_STENCIL_TEST, cache_.stencil_test, p.stencil_test);
    if (p.stencil_test) {
        const std::uint64_t key = static_cast<std::uint64_t>(p.stencil_func) | (static_cast<std::uint64_t>(p.stencil_fail) << 4u) |
                                  (static_cast<std::uint64_t>(p.stencil_zfail) << 8u) | (static_cast<std::uint64_t>(p.stencil_zpass) << 12u) |
                                  (static_cast<std::uint64_t>(d.stencil_ref) << 16u) | (static_cast<std::uint64_t>(d.stencil_compare_mask) << 24u) |
                                  (static_cast<std::uint64_t>(d.stencil_write_mask) << 32u);
        if (cache_.stencil_key != key) {
            glStencilFunc(compare_func(p.stencil_func), d.stencil_ref, d.stencil_compare_mask);
            glStencilOp(stencil_op(p.stencil_fail), stencil_op(p.stencil_zfail), stencil_op(p.stencil_zpass));
            glStencilMask(d.stencil_write_mask);
            cache_.stencil_key = key;
        }
    }
    if (cache_.color_mask != p.color_mask) {
        glColorMask((p.color_mask & 1u) != 0u, (p.color_mask & 2u) != 0u, (p.color_mask & 4u) != 0u, (p.color_mask & 8u) != 0u);
        cache_.color_mask = p.color_mask;
    }
    set_enabled(GL_DEPTH_TEST, cache_.depth_test, p.depth_test);
    if (p.depth_test) {
        const GLenum func = compare_func(p.depth_func);
        if (cache_.depth_func != func) {
            glDepthFunc(func);
            cache_.depth_func = func;
        }
        if (cache_.depth_mask != static_cast<int>(p.depth_write)) {
            glDepthMask(p.depth_write ? GL_TRUE : GL_FALSE);
            cache_.depth_mask = static_cast<int>(p.depth_write);
        }
    }
    set_enabled(GL_BLEND, cache_.blend, p.blend);
    if (p.blend) {
        const GLenum src = blend_factor(p.blend_src), dst = blend_factor(p.blend_dst);
        if (src == GL_CONSTANT_COLOR || dst == GL_CONSTANT_COLOR) {
            if (!std::equal(d.blend_constant.begin(), d.blend_constant.end(), cache_.blend_color)) {
                glBlendColor(d.blend_constant[0], d.blend_constant[1], d.blend_constant[2], d.blend_constant[3]);
                std::copy(d.blend_constant.begin(), d.blend_constant.end(), cache_.blend_color);
            }
        }
        if (cache_.blend_src != src || cache_.blend_dst != dst) {
            glBlendFunc(src, dst);
            cache_.blend_src = src;
            cache_.blend_dst = dst;
        }
        const GLenum equation = blend_equation(p.blend_op);
        if (cache_.blend_eq != equation) {
            glBlendEquation(equation);
            cache_.blend_eq = equation;
        }
    }
    if (texture == 0u) {
        bind_texture0(blank_texture_);
    } else {
        bind_texture0(texture);
        const GLuint sampler = samplers_[(d.linear ? 1 : 0) | (d.clamp_u ? 2 : 0) | (d.clamp_v ? 4 : 0)];
        if (cache_.sampler != sampler) {
            glBindSampler(0, sampler);
            cache_.sampler = sampler;
        }
    }
    // Each draw re-specifies the vertex buffer (orphaning) and gets an index
    // buffer of its own from a rotating pool.  Writing into a buffer the GPU may
    // still be reading makes mobile drivers stall or copy it, and some browsers
    // re-validate a whole index buffer after any change to it.
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(d.vertices.size_bytes()), d.vertices.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_buffers_[next_index_buffer_]);
    next_index_buffer_ = (next_index_buffer_ + 1u) % index_buffers_.size();
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(d.indices.size_bytes()), d.indices.data(), GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(d.indices.size()), GL_UNSIGNED_INT, nullptr);
}

void GlGpu::present(const GeSurfaces::PresentSource &source, GLuint out_fbo, int out_width, int out_height) {
    reset_cache(); // the code below drives GL directly
    GLuint texture = 0u;
    float rect[8] = {source.rect_w, source.rect_h, source.origin_x, source.origin_y,
                     static_cast<float>(source.target != 0u ? source.format : 3u), 0.0f, 0.0f, 0.0f};
    if (source.target != 0u) {
        texture = at(source.target).color;
    } else if (source.cpu_pixels != nullptr) {
        glBindTexture(GL_TEXTURE_2D, present_texture_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GeSurfaces::kDisplayWidth, GeSurfaces::kDisplayHeight, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, source.cpu_pixels);
        texture = present_texture_;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, out_fbo);
    glViewport(0, 0, out_width, out_height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (texture == 0u) return;
    glUseProgram(present_program_);
    glBindVertexArray(present_vao_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindSampler(0, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUniform4fv(u_present_, 2, rect);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    reset_cache();
}

} // namespace pspweb
