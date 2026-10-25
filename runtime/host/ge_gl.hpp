#pragma once

// OpenGL ES 3 / WebGL2 / desktop OpenGL 3.3 core backend for the GE: GeSurfaces
// (ge_surfaces.hpp) with a GL device. A GL render target is a framebuffer object with an
// RGBA8 colour texture and a depth24/stencil8 renderbuffer; PSP row 0 is GL row 0, so
// pictures are upside down in GL window terms and present() flips them.

#include "ge_gpu.hpp"
#include "ge_surfaces.hpp"
#include "psprecomp/guest_memory.hpp"

#ifdef ANYPSP_GL_DESKTOP
#include "gl_api.hpp"
#else
#include <GLES3/gl3.h>
#endif

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace pspweb {

class GlGpu final : public GeGpu {
public:
    GlGpu() = default;
    ~GlGpu() override;
    GlGpu(const GlGpu &) = delete;
    GlGpu &operator=(const GlGpu &) = delete;

    // Compiles shaders; a GL ES 3 (or desktop GL 3.3 core) context must be current.
    bool init();
    // Draws `source` stretched into `out_fbo` (0 = the canvas), bottom row first.
    void present(const GeSurfaces::PresentSource &source, GLuint out_fbo, int out_width, int out_height);

    [[nodiscard]] int max_scale() const override;
    void set_scale(int scale) override;
    GpuTarget create_target(int width, int height) override;
    void destroy_target(GpuTarget target) override;
    void copy_target(GpuTarget from, int fx, int fy, GpuTarget to, int tx, int ty, int w, int h) override;
    void read_pixels(GpuTarget target, const GeRect &r, std::uint8_t *rgba) override;
    void write_pixels(GpuTarget target, const GeRect &r, const std::uint8_t *rgba) override;
    void reinterpret(GpuTarget into, GpuTarget from, const GeRect &hit, const GpuReinterpret &params) override;
    void fill_alpha(GpuTarget target, const GeRect &r, std::uint8_t value) override;
    void stencil_to_alpha(GpuTarget target, const GeRect &r, std::uint32_t bits) override;
    void alpha_to_stencil(GpuTarget target, const GeRect &r, std::uint32_t bits) override;
    void draw(const GpuDraw &draw) override;
    void forget_texture(const void *identity) override;
    void invalidate_cache() override { reset_cache(); }

private:
    struct Target {
        GLuint fbo{}, color{}, depth{};
        int width{}, height{}; // PSP pixels
    };
    struct GlTexture {
        GLuint id{};
        std::uint64_t hash{};
        std::uint32_t width{}, height{};
    };
    [[nodiscard]] Target &at(GpuTarget target) { return targets_[target - 1u]; }
    // glBlitFramebuffer with the scissor test off and all colour channels on.
    void blit(GLuint from, const GLint src[4], GLuint to, const GLint dst[4], GLbitfield mask);
    // Binds `t`, sets its full viewport and scissors to `r`.
    void begin_pass(GLuint fbo, const Target &t, const GeRect &r);
    GLuint texture_for(const GpuDraw &d);

    // Cached GL state: skips redundant calls, which are costly through WebGL.
    struct Cache {
        GLuint program{~0u}, vao{~0u}, fbo{~0u}, texture{~0u}, sampler{~0u};
        GLint viewport_w{-1}, viewport_h{-1};
        GLint scissor[4]{-1, -1, -1, -1};
        int scissor_test{-1}, depth_test{-1}, blend{-1}, stencil_test{-1};
        std::uint64_t stencil_key{~0ull};
        GLenum depth_func{0}, blend_src{0}, blend_dst{0}, blend_eq{0};
        int depth_mask{-1};
        int color_mask{-1};
        float blend_color[4]{-1, -1, -1, -1};
        bool params_valid{};
        GpuShaderParams params;
    };
    void reset_cache() { cache_ = Cache{}; }
    void use_program(GLuint program);
    void bind_framebuffer(GLuint fbo);
    void bind_texture0(GLuint texture);
    void set_enabled(GLenum cap, int &cached, bool on);
    Cache cache_{};
    GLuint samplers_[8]{};

    std::vector<Target> targets_; // handle - 1; fbo 0 marks a free slot
    std::vector<GpuTarget> free_targets_;
    int scale_{1};
    GLuint program_{}, present_program_{};
    GLuint vao_{}, vbo_{}, present_vao_{}, scratch_texture_{}, present_texture_{}, blank_texture_{};
    GLsizei scratch_width_{}, scratch_height_{};
    // Each draw gets its own small index buffer from a rotating pool: some
    // browsers re-validate a whole index buffer after any change to it.
    std::vector<GLuint> index_buffers_;
    std::size_t next_index_buffer_{};
    GLint u_params_{}, u_present_{};
    GLuint reinterpret_program_{};
    GLint u_ri_{};
    // Uploads and downloads at higher internal resolutions pass through a native-size staging framebuffer.
    GLuint staging_fbo_{}, staging_texture_{};
    GLuint stencil_program_{}, fill_program_{}, stencil_fbo_{};
    GLint u_st_{}, u_fill_{};
    std::unordered_map<const void *, GlTexture> textures_;
};

class GeGl final : public GeSurfaces {
public:
    explicit GeGl(psprecomp::GuestMemory &memory);

    // Compiles shaders; a GL ES 3 (or desktop GL 3.3 core) context must be current.
    bool init() { return gl_.init(); }
    // Draws the displayed framebuffer into `out_fbo` (0 = the canvas).
    void present(std::uint32_t fb_address, std::uint32_t stride, std::uint32_t format,
                 GLuint out_fbo, int out_width, int out_height) {
        gl_.present(present_source(fb_address, stride, format), out_fbo, out_width, out_height);
    }
    // Forgets the cached GL state, for when another renderer used the same context.
    void reset_gl_cache() { gl_.invalidate_cache(); }

private:
    GlGpu &gl_;
};

} // namespace pspweb
