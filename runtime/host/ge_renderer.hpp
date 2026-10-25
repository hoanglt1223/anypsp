#pragma once

// The seam between the GE front end (ge.cpp: list decoding, transform, lighting,
// clipping) and a GPU backend. Without a backend the front end rasterizes into
// guest VRAM in software, which is what the headless host uses.

#include <cstdint>
#include <functional>

namespace pspweb {

struct GlVertex {
    float x, y, z, w;   // drawing-space pixels, depth 0..65535, clip w
    float u, v;         // texels
    std::uint8_t r, g, b, a;
    float q{1.0f};      // projective texture divisor: the texel is (u, v) / q per pixel
    float fog{255.0f};  // fog factor 0 (all fog colour) .. 255 (none), already in the GE's 8 bits
};

// A rectangle copied by a GE block transfer.
struct TransferRect {
    std::uint32_t base{}, stride{}, x{}, y{}, width{}, height{}, bpp{};
};

struct GlDrawState {
    std::uint32_t fb_offset{}, fb_stride{}, fb_format{};
    int sx1{}, sy1{}, sx2{}, sy2{};
    bool clear{};
    std::uint32_t clear_flags{};
    bool z_test{}, z_write{};
    std::uint32_t z_func{};
    bool blend{};
    std::uint32_t src_factor{}, dst_factor{}, equation{}, fix_a{}, fix_b{};
    bool alpha_test{};
    std::uint32_t alpha_func{}, alpha_ref{}, alpha_mask{};
    bool stencil{};
    std::uint32_t stencil_func{}, stencil_ops{}; // raw STST / SOP registers
    std::uint32_t mask_rgb{}, mask_alpha{};
    // Fog mixes in fog_color by each vertex's GlVertex::fog.
    bool fog{};
    std::uint32_t fog_color{};
    // Dithering adds dither_rows[y & 3] >> ((x & 3) * 4) (signed 4 bits) to each pixel's colour.
    bool dither{};
    std::uint32_t dither_rows[4]{};
    bool texture{};
    std::uint32_t tex_func{};
    bool tex_alpha{}, tex_double{};
    std::uint32_t env{};
    bool clamp_u{}, clamp_v{}, linear{};
    // Texture source: decoded texels, or a surface region at tex_address.
    const std::uint32_t *texels{};
    const void *texture_identity{};
    std::uint64_t texture_hash{};
    std::uint32_t tex_width{}, tex_height{};
    bool tex_from_target{};
    std::uint32_t tex_address{}, tex_format{}, tex_buf_width{};

    bool operator==(const GlDrawState &) const = default;
};

class GeRenderer {
public:
    virtual ~GeRenderer() = default;

    // True when a texture at `address` (format 0..3, buffer width in pixels)
    // reads a render target that lives on the GPU.
    [[nodiscard]] virtual bool is_gpu_target(std::uint32_t address, std::uint32_t format, std::uint32_t buf_width) const = 0;
    // Makes VRAM current for a range the CPU is about to read (texture decode).
    virtual bool prepare_vram_read(std::uint32_t address, std::uint32_t bytes) = 0;
    virtual void set_state(const GlDrawState &state) = 0;
    virtual void triangle(const GlVertex &a, const GlVertex &b, const GlVertex &c) = 0;
    virtual std::uint32_t vertex(const GlVertex &v) = 0;
    virtual void indexed_triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c) = 0;
    virtual void flush() = 0;
    virtual void forget_texture(const void *identity) = 0;
    virtual void before_transfer(const TransferRect &src, const TransferRect &dst) = 0;
    virtual void after_transfer(const TransferRect &dst) = 0;
    virtual void set_vram_write_hook(std::function<void(std::uint32_t, std::uint32_t)> hook) = 0;
    // Called around every display-list run; the guest CPU only touches memory in between.
    virtual void list_started() {}
    virtual void list_finished() {}
};

} // namespace pspweb
