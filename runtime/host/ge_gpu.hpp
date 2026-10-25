#pragma once

// The GPU half of the GE's hardware renderers. GeSurfaces (ge_surfaces.hpp) decides what
// lives where: which VRAM regions have render targets, when guest VRAM and the GPU copies are
// synchronized, and when stencil and colour alpha are moved into each other. A GeGpu device
// carries those decisions out with one graphics API: OpenGL (ge_gl.cpp) or Vulkan
// (gpu/vulkan/vk_gpu.cpp).
//
// Rectangles are inclusive and in PSP pixels; a device renders at `scale` times that
// internal resolution and scales by itself. Pixel rows are stored with PSP row 0 first.

#include "ge_renderer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace pspweb {

// Inclusive pixel rectangle; empty when x1 < x0.
struct GeRect {
    int x0{0}, y0{0}, x1{-1}, y1{-1};
    [[nodiscard]] bool empty() const { return x1 < x0 || y1 < y0; }
    [[nodiscard]] int width() const { return empty() ? 0 : x1 - x0 + 1; }
    [[nodiscard]] int height() const { return empty() ? 0 : y1 - y0 + 1; }
    void add(int ax0, int ay0, int ax1, int ay1) {
        if (ax1 < ax0 || ay1 < ay0) return;
        if (empty()) {
            x0 = ax0; y0 = ay0; x1 = ax1; y1 = ay1;
            return;
        }
        x0 = std::min(x0, ax0); y0 = std::min(y0, ay0);
        x1 = std::max(x1, ax1); y1 = std::max(y1, ay1);
    }
    void add(const GeRect &r) { add(r.x0, r.y0, r.x1, r.y1); }
    [[nodiscard]] bool intersects(const GeRect &o) const {
        return !empty() && !o.empty() && x0 <= o.x1 && o.x0 <= x1 && y0 <= o.y1 && o.y0 <= y1;
    }
    [[nodiscard]] GeRect clip(const GeRect &o) const {
        if (!intersects(o)) return GeRect{};
        return GeRect{std::max(x0, o.x0), std::max(y0, o.y0), std::min(x1, o.x1), std::min(y1, o.y1)};
    }
    [[nodiscard]] GeRect shifted(int dx, int dy) const {
        return empty() ? GeRect{} : GeRect{x0 + dx, y0 + dy, x1 + dx, y1 + dy};
    }
    bool operator==(const GeRect &) const = default;
};

// Comparisons in the GE's register order (depth, stencil and alpha tests).
enum class GpuCompare : std::uint8_t { Never, Always, Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };
// Stencil operations in the GE's register order.
enum class GpuStencilOp : std::uint8_t { Keep, Zero, Replace, Invert, IncrementClamp, DecrementClamp };
enum class GpuBlendFactor : std::uint8_t {
    Zero, One, SrcColor, OneMinusSrcColor, DstColor, OneMinusDstColor,
    SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha, ConstantColor,
};
enum class GpuBlendOp : std::uint8_t { Add, Subtract, ReverseSubtract, Min, Max };

// Fixed-function state of a draw (Vulkan bakes it into a pipeline). Fields that are off
// keep their defaults, so equal states compare and hash equal.
struct GpuPipeline {
    std::uint8_t color_mask{0xFu}; // bit 0 red .. bit 3 alpha
    bool depth_test{}, depth_write{};
    GpuCompare depth_func{GpuCompare::Always};
    bool blend{};
    GpuBlendFactor blend_src{GpuBlendFactor::One}, blend_dst{GpuBlendFactor::Zero};
    GpuBlendOp blend_op{GpuBlendOp::Add};
    bool stencil_test{};
    GpuCompare stencil_func{GpuCompare::Always};
    GpuStencilOp stencil_fail{GpuStencilOp::Keep}, stencil_zfail{GpuStencilOp::Keep}, stencil_zpass{GpuStencilOp::Keep};
    bool operator==(const GpuPipeline &) const = default;
};

// The GE shader's parameters: eight vec4s (128 bytes, all the push constant space Vulkan
// guarantees), the same block for every backend (shaders/ge.frag.inc documents each field).
struct GpuShaderParams {
    enum Flag : std::uint32_t {
        kTexEnable = 1u, kTexAlpha = 2u, kTexDouble = 4u, kAlphaTest = 8u, kClear = 16u, kTexOpaque = 32u, kFog = 64u,
        kLinear = 128u, kExactSample = 256u, kDither = 512u, kQuantize = 1024u,
    };
    std::array<float, 32> v{};
    void set(int slot, float a, float b, float c, float d) {
        v[static_cast<std::size_t>(slot) * 4u] = a;
        v[static_cast<std::size_t>(slot) * 4u + 1u] = b;
        v[static_cast<std::size_t>(slot) * 4u + 2u] = c;
        v[static_cast<std::size_t>(slot) * 4u + 3u] = d;
    }
    bool operator==(const GpuShaderParams &) const = default;
};

using GpuTarget = std::uint32_t; // 0: none

struct GpuDraw {
    GpuTarget target{};
    GeRect scissor;                // empty: draws nothing
    GpuPipeline pipeline;
    std::uint8_t stencil_ref{}, stencil_compare_mask{0xFFu}, stencil_write_mask{0xFFu};
    std::array<float, 4> blend_constant{};
    GpuShaderParams params;
    // What the draw samples. Snapshot: the target being drawn, read from a copy of `snapshot`
    // taken just before the draw.
    enum class Source : std::uint8_t { None, Texels, Target, Snapshot } source{Source::None};
    GpuTarget texture_target{};
    GeRect snapshot;
    const void *texture_identity{};
    std::uint64_t texture_hash{};
    const std::uint32_t *texels{};
    std::uint32_t tex_width{}, tex_height{};
    bool linear{}, clamp_u{}, clamp_v{};
    std::span<const GlVertex> vertices;
    std::span<const std::uint32_t> indices;
};

// Rebuilds a target's pixels from another target holding the same VRAM bytes in the other
// pixel size (shaders/reinterpret.glsl.inc).
struct GpuReinterpret {
    int mode{};      // 0: 32-bit source into a 16-bit target, 1: the other way
    int format{};    // the 16-bit side's pixel format (0..2)
    int delta{};     // into.offset - from.offset in bytes
    int row_bytes{}; // shared by both
    int src_width{}, src_height{};
};

class GeGpu {
public:
    virtual ~GeGpu() = default;

    // Highest internal resolution multiple the device's size limits allow for a 1024-pixel
    // wide target, at most 4 (8 for Vulkan, with an explicit request).
    [[nodiscard]] virtual int max_scale() const = 0;
    // Only called while no target exists.
    virtual void set_scale(int scale) = 0;
    // A render target: colour (contents undefined until written), depth and stencil cleared to 0.
    virtual GpuTarget create_target(int width, int height) = 0;
    virtual void destroy_target(GpuTarget target) = 0;
    // Copies colour and depth (not stencil) of a w x h block from one target to another.
    virtual void copy_target(GpuTarget from, int fx, int fy, GpuTarget to, int tx, int ty, int w, int h) = 0;
    // RGBA8 pixels of `r` at one sample per PSP pixel, rows of r.width() pixels from r.y0 on.
    virtual void read_pixels(GpuTarget target, const GeRect &r, std::uint8_t *rgba) = 0;
    virtual void write_pixels(GpuTarget target, const GeRect &r, const std::uint8_t *rgba) = 0;
    // Draws into `hit` of `into` from `from` (a different target).
    virtual void reinterpret(GpuTarget into, GpuTarget from, const GeRect &hit, const GpuReinterpret &params) = 0;
    // Sets colour alpha in `r` to `value`.
    virtual void fill_alpha(GpuTarget target, const GeRect &r, std::uint8_t value) = 0;
    // Colour alpha in `r` := the stencil bits `bits` (other alpha bits become 0).
    virtual void stencil_to_alpha(GpuTarget target, const GeRect &r, std::uint32_t bits) = 0;
    // Stencil in `r` := the colour alpha bits `bits` (other stencil bits become 0).
    virtual void alpha_to_stencil(GpuTarget target, const GeRect &r, std::uint32_t bits) = 0;
    virtual void draw(const GpuDraw &draw) = 0;
    virtual void forget_texture(const void *identity) = 0;
    // Another user of the same API context may have changed its state.
    virtual void invalidate_cache() {}
};

} // namespace pspweb
