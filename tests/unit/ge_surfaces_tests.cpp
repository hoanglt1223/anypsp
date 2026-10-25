// SPDX-License-Identifier: GPL-2.0-or-later
// GeSurfaces (the hardware renderers' surface manager) on a CPU device: what reaches guest VRAM
// when the CPU reads GPU-drawn framebuffers.

#include "test_harness.hpp"

#include "ge_surfaces.hpp"

#include "psprecomp/guest_memory.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace {

constexpr std::uint32_t kVram = 0x04000000u;

// Targets are RGBA8 pictures in memory. A draw fills the vertices' bounding box (inside the
// scissor) with the first vertex's colour, honouring the colour mask: enough to place pixels.
class CpuGpu final : public pspweb::GeGpu {
public:
    struct Target {
        int width{}, height{};
        std::vector<std::uint32_t> pixels;
    };
    std::vector<Target> targets;
    int reads{};

    int max_scale() const override { return 1; }
    void set_scale(int) override {}
    pspweb::GpuTarget create_target(int width, int height) override {
        targets.push_back({width, height, std::vector<std::uint32_t>(static_cast<std::size_t>(width * height), 0u)});
        return static_cast<pspweb::GpuTarget>(targets.size());
    }
    void destroy_target(pspweb::GpuTarget) override {}
    void copy_target(pspweb::GpuTarget from, int fx, int fy, pspweb::GpuTarget to, int tx, int ty, int w, int h) override {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) at(to, tx + x, ty + y) = at(from, fx + x, fy + y);
    }
    void read_pixels(pspweb::GpuTarget t, const pspweb::GeRect &r, std::uint8_t *rgba) override {
        ++reads;
        for (int y = r.y0; y <= r.y1; ++y)
            for (int x = r.x0; x <= r.x1; ++x)
                std::memcpy(rgba + (static_cast<std::size_t>(y - r.y0) * static_cast<std::size_t>(r.width()) + static_cast<std::size_t>(x - r.x0)) * 4u,
                            &at(t, x, y), 4u);
    }
    void write_pixels(pspweb::GpuTarget t, const pspweb::GeRect &r, const std::uint8_t *rgba) override {
        for (int y = r.y0; y <= r.y1; ++y)
            for (int x = r.x0; x <= r.x1; ++x)
                std::memcpy(&at(t, x, y),
                            rgba + (static_cast<std::size_t>(y - r.y0) * static_cast<std::size_t>(r.width()) + static_cast<std::size_t>(x - r.x0)) * 4u, 4u);
    }
    void reinterpret(pspweb::GpuTarget, pspweb::GpuTarget, const pspweb::GeRect &, const pspweb::GpuReinterpret &) override {}
    void fill_alpha(pspweb::GpuTarget, const pspweb::GeRect &, std::uint8_t) override {}
    void stencil_to_alpha(pspweb::GpuTarget, const pspweb::GeRect &, std::uint32_t) override {}
    void alpha_to_stencil(pspweb::GpuTarget, const pspweb::GeRect &, std::uint32_t) override {}
    void draw(const pspweb::GpuDraw &d) override {
        const float ox = d.params.v[2], oy = d.params.v[3];
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (const auto &v : d.vertices) {
            x0 = std::min(x0, v.x); y0 = std::min(y0, v.y);
            x1 = std::max(x1, v.x); y1 = std::max(y1, v.y);
        }
        const pspweb::GeRect box = pspweb::GeRect{static_cast<int>(std::floor(x0 + ox)), static_cast<int>(std::floor(y0 + oy)),
                                                  static_cast<int>(std::ceil(x1 + ox)) - 1, static_cast<int>(std::ceil(y1 + oy)) - 1}
                                       .clip(d.scissor);
        const auto &c = d.vertices.front();
        const std::uint32_t color = c.r | (c.g << 8u) | (c.b << 16u) | (static_cast<std::uint32_t>(c.a) << 24u);
        std::uint32_t mask = 0u;
        for (int i = 0; i < 4; ++i)
            if ((d.pipeline.color_mask & (1u << i)) != 0u) mask |= 0xFFu << (i * 8);
        for (int y = box.y0; y <= box.y1; ++y)
            for (int x = box.x0; x <= box.x1; ++x) {
                std::uint32_t &p = at(d.target, x, y);
                p = (p & ~mask) | (color & mask);
            }
    }
    void forget_texture(const void *) override {}

private:
    std::uint32_t &at(pspweb::GpuTarget t, int x, int y) {
        Target &target = targets[t - 1u];
        return target.pixels[static_cast<std::size_t>(y * target.width + x)];
    }
};

pspweb::GlDrawState framebuffer(std::uint32_t format) {
    pspweb::GlDrawState s;
    s.fb_offset = 0u;
    s.fb_stride = 512u;
    s.fb_format = format;
    s.sx1 = 0;
    s.sy1 = 0;
    s.sx2 = 479;
    s.sy2 = 271;
    s.mask_rgb = 0u;
    return s;
}

void rectangle(pspweb::GeSurfaces &gpu, float x0, float y0, float x1, float y1, std::uint32_t rgb) {
    pspweb::GlVertex v{};
    v.r = static_cast<std::uint8_t>(rgb);
    v.g = static_cast<std::uint8_t>(rgb >> 8u);
    v.b = static_cast<std::uint8_t>(rgb >> 16u);
    v.a = 255u;
    pspweb::GlVertex a = v, b = v, c = v, d = v;
    a.x = x0; a.y = y0; b.x = x1; b.y = y0; c.x = x0; c.y = y1; d.x = x1; d.y = y1;
    gpu.triangle(a, b, c);
    gpu.triangle(b, d, c);
}

} // namespace

TEST_CASE("ge surfaces: a 16-bit framebuffer is packed on readback, and only drawn pixels are written") {
    auto memory = std::make_unique<psprecomp::GuestMemory>();
    // Something the CPU put there first, next to where the GPU draws.
    memory->store16(kVram + 2u * 20u, 0x1234u);
    auto device = std::make_unique<CpuGpu>();
    CpuGpu &cpu = *device;
    pspweb::GeSurfaces gpu(*memory, std::move(device));
    gpu.set_state(framebuffer(0u)); // RGB565
    rectangle(gpu, 0.0f, 0.0f, 4.0f, 2.0f, 0x00FF8040u);
    gpu.flush();
    CHECK(gpu.prepare_vram_read(kVram, 512u * 2u * 2u));
    CHECK_EQ(cpu.reads, 1);
    // r 0x40 >> 3, g 0x80 >> 2, b 0xFF >> 3.
    const std::uint16_t packed = static_cast<std::uint16_t>((0x40u >> 3u) | ((0x80u >> 2u) << 5u) | ((0xFFu >> 3u) << 11u));
    CHECK_EQ(memory->load16(kVram), packed);
    CHECK_EQ(memory->load16(kVram + 2u * 3u), packed);
    CHECK_EQ(memory->load16(kVram + 512u * 2u + 2u * 3u), packed);
    CHECK_EQ(memory->load16(kVram + 2u * 4u), 0u);       // right of the rectangle: not drawn
    CHECK_EQ(memory->load16(kVram + 2u * 20u), 0x1234u); // the CPU's value survives
    // Nothing new drawn: a second read downloads nothing.
    CHECK(!gpu.prepare_vram_read(kVram, 512u * 2u * 2u));
    CHECK_EQ(cpu.reads, 1);
}

TEST_CASE("ge surfaces: a block transfer reading a GPU-drawn framebuffer gets its pixels first") {
    auto memory = std::make_unique<psprecomp::GuestMemory>();
    auto device = std::make_unique<CpuGpu>();
    pspweb::GeSurfaces gpu(*memory, std::move(device));
    gpu.set_state(framebuffer(3u)); // 8888
    rectangle(gpu, 8.0f, 8.0f, 16.0f, 16.0f, 0x00336699u);
    // Not flushed yet: the transfer must still see the batch.
    const pspweb::TransferRect src{kVram, 512u, 8u, 8u, 8u, 8u, 4u};
    const pspweb::TransferRect dst{kVram + 0x100000u, 64u, 0u, 0u, 8u, 8u, 4u};
    gpu.before_transfer(src, dst);
    CHECK_EQ(memory->load32(kVram + (8u * 512u + 8u) * 4u) & 0xFFFFFFu, 0x336699u);
    CHECK_EQ(memory->load32(kVram + (15u * 512u + 15u) * 4u) & 0xFFFFFFu, 0x336699u);
}
