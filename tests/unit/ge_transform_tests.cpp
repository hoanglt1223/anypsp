// SPDX-License-Identifier: GPL-2.0-or-later
// GE vertex processing as the hardware renderers receive it: skinning, morphing, attribute
// carry between draws, per-vertex fog and position-less vertices.

#include "test_harness.hpp"

#include "ge.hpp"

#include "psprecomp/guest_memory.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

constexpr std::uint32_t kList = 0x08800000u;
constexpr std::uint32_t kVertices = 0x08812000u;

std::uint32_t op(std::uint32_t cmd, std::uint32_t data) { return (cmd << 24u) | (data & 0xFFFFFFu); }
std::uint32_t f24(float f) { return std::bit_cast<std::uint32_t>(f) >> 8u; }

// Records what the front end hands a GPU backend.
class Recorder final : public pspweb::GeRenderer {
public:
    std::vector<pspweb::GlVertex> vertices; // three per triangle
    std::vector<pspweb::GlDrawState> states;

    bool is_gpu_target(std::uint32_t, std::uint32_t, std::uint32_t) const override { return false; }
    bool prepare_vram_read(std::uint32_t, std::uint32_t) override { return false; }
    void set_state(const pspweb::GlDrawState &state) override { states.push_back(state); }
    void triangle(const pspweb::GlVertex &a, const pspweb::GlVertex &b, const pspweb::GlVertex &c) override {
        vertices.insert(vertices.end(), {a, b, c});
    }
    std::uint32_t vertex(const pspweb::GlVertex &v) override {
        pool_.push_back(v);
        return static_cast<std::uint32_t>(pool_.size() - 1u);
    }
    void indexed_triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c) override {
        vertices.insert(vertices.end(), {pool_[a], pool_[b], pool_[c]});
    }
    void flush() override { pool_.clear(); }
    void forget_texture(const void *) override {}
    void before_transfer(const pspweb::TransferRect &, const pspweb::TransferRect &) override {}
    void after_transfer(const pspweb::TransferRect &) override {}
    void set_vram_write_hook(std::function<void(std::uint32_t, std::uint32_t)>) override {}

private:
    std::vector<pspweb::GlVertex> pool_;
};

// A 480x272 8888 framebuffer, identity world/view/projection, and a viewport that maps
// clip x/y -1..1 to the screen (y down), depth 0..1 to 0..65535.
std::vector<std::uint32_t> setup_3d() {
    std::vector<std::uint32_t> l = {op(0x9C, 0u), op(0x9D, 512u), op(0xD2, 3u), op(0xD4, 0u), op(0xD5, (271u << 10u) | 479u),
                                    op(0x42, f24(240.0f)), op(0x43, f24(-136.0f)), op(0x44, f24(32767.5f)),
                                    op(0x45, f24(240.0f)), op(0x46, f24(136.0f)), op(0x47, f24(32767.5f)),
                                    op(0x4C, 0u), op(0x4D, 0u)};
    const float identity43[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    const float identity44[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (std::uint32_t m : {0x3Au, 0x3Cu}) {
        l.push_back(op(m, 0u));
        for (float v : identity43) l.push_back(op(m + 1u, f24(v)));
    }
    l.push_back(op(0x3E, 0u));
    for (float v : identity44) l.push_back(op(0x3F, f24(v)));
    return l;
}

void run(psprecomp::GuestMemory &memory, pspweb::Ge &ge, std::vector<std::uint32_t> list) {
    list.push_back(op(0x0F, 0u));
    list.push_back(op(0x0C, 0u));
    for (std::size_t i = 0; i < list.size(); ++i) memory.store32(kList + static_cast<std::uint32_t>(i * 4u), list[i]);
    ge.run_list(kList, 0u);
}

void store_floats(psprecomp::GuestMemory &memory, std::uint32_t at, const std::vector<float> &values) {
    for (std::size_t i = 0; i < values.size(); ++i)
        memory.store32(at + static_cast<std::uint32_t>(i * 4u), std::bit_cast<std::uint32_t>(values[i]));
}

std::vector<std::uint32_t> draw(std::uint32_t vertex_type, std::uint32_t count) {
    return {op(0x10, (kVertices >> 8u) & 0x0F0000u), op(0x12, vertex_type), op(0x01, kVertices & 0xFFFFFFu),
            op(0x04, (3u << 16u) | count)};
}

bool near(float a, float b) { return std::fabs(a - b) < 0.07f; }

} // namespace

TEST_CASE("ge transform: skinning blends the bone matrices by the vertex weights") {
    auto memory = std::make_unique<psprecomp::GuestMemory>();
    // Float weights (2), float positions: w0, w1, x, y, z per vertex.
    store_floats(*memory, kVertices, {0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 0.5f, 0.5f, 0.0f, 0.5f, 0.0f, 0.25f, 0.75f, 0.5f, 0.0f, 0.0f});
    auto list = setup_3d();
    // Bone 0 moves x by 0.1, bone 1 by 0.3.
    for (std::uint32_t bone = 0; bone < 2u; ++bone) {
        list.push_back(op(0x2A, bone * 12u));
        const float m[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, bone == 0u ? 0.1f : 0.3f, 0, 0};
        for (float v : m) list.push_back(op(0x2B, f24(v)));
    }
    const auto d = draw((3u << 9u) | (1u << 14u) | (3u << 7u), 3u);
    list.insert(list.end(), d.begin(), d.end());
    pspweb::Ge ge(*memory);
    Recorder recorder;
    ge.set_gl(&recorder);
    run(*memory, ge, list);
    CHECK_EQ(recorder.vertices.size(), 3u);
    // 0.5 * 0.1 + 0.5 * 0.3 = 0.2 -> x = 240 + 0.2 * 240; the third vertex: 0.5 + 0.25 * 0.1 + 0.75 * 0.3.
    CHECK(near(recorder.vertices[0].x, 288.0f));
    CHECK(near(recorder.vertices[0].y, 136.0f));
    CHECK(near(recorder.vertices[1].y, 136.0f - 0.5f * 136.0f));
    CHECK(near(recorder.vertices[2].x, 240.0f + 0.75f * 240.0f));
}

TEST_CASE("ge transform: morphing mixes the vertex blocks by the morph weights") {
    auto memory = std::make_unique<psprecomp::GuestMemory>();
    // Two morph targets of float positions per vertex.
    store_floats(*memory, kVertices, {0.0f, 0.0f, 0.0f, 0.4f, 0.0f, 0.0f,  // vertex 0: (0,0) and (0.4,0)
                                      0.0f, 0.5f, 0.0f, 0.0f, -0.5f, 0.0f, // vertex 1
                                      0.5f, 0.0f, 0.0f, 0.5f, 0.0f, 0.0f});
    auto list = setup_3d();
    list.push_back(op(0x2C, f24(0.25f)));
    list.push_back(op(0x2D, f24(0.75f)));
    const auto d = draw((1u << 18u) | (3u << 7u), 3u);
    list.insert(list.end(), d.begin(), d.end());
    pspweb::Ge ge(*memory);
    Recorder recorder;
    ge.set_gl(&recorder);
    run(*memory, ge, list);
    CHECK_EQ(recorder.vertices.size(), 3u);
    CHECK(near(recorder.vertices[0].x, 240.0f + 0.3f * 240.0f));
    CHECK(near(recorder.vertices[1].y, 136.0f + 0.25f * 136.0f));
    CHECK(near(recorder.vertices[2].x, 240.0f + 0.5f * 240.0f));
}

TEST_CASE("ge transform: a format without texture coordinates reuses the last ones read") {
    auto memory = std::make_unique<psprecomp::GuestMemory>();
    // Through mode, float UVs and positions: u, v, x, y, z.
    store_floats(*memory, kVertices, {1, 2, 0, 0, 0, 3, 4, 10, 0, 0, 5, 6, 0, 10, 0});
    std::vector<std::uint32_t> list = {op(0x9C, 0u), op(0x9D, 512u), op(0xD2, 3u), op(0xD4, 0u), op(0xD5, (271u << 10u) | 479u)};
    auto first = draw((1u << 23u) | 3u | (3u << 7u), 3u);
    list.insert(list.end(), first.begin(), first.end());
    // Same positions again without UVs: positions only, 12 bytes from vertex 0's x.
    store_floats(*memory, kVertices + 0x100u, {0, 0, 0, 10, 0, 0, 0, 10, 0});
    list.push_back(op(0x01, (kVertices + 0x100u) & 0xFFFFFFu));
    list.push_back(op(0x12, (1u << 23u) | (3u << 7u)));
    list.push_back(op(0x04, (3u << 16u) | 3u));
    pspweb::Ge ge(*memory);
    Recorder recorder;
    ge.set_gl(&recorder);
    run(*memory, ge, list);
    CHECK_EQ(recorder.vertices.size(), 6u);
    for (std::size_t i = 3; i < 6u; ++i) {
        CHECK_EQ(recorder.vertices[i].u, 5.0f);
        CHECK_EQ(recorder.vertices[i].v, 6.0f);
    }
}

TEST_CASE("ge transform: fog factors come per vertex from view-space depth") {
    auto memory = std::make_unique<psprecomp::GuestMemory>();
    store_floats(*memory, kVertices, {0, 0, 0.5f, 0.5f, 0, 0.25f, 0, 0.5f, 2.0f});
    auto list = setup_3d();
    // factor = (view z + FOG1) * FOG2, as 8 bits: min(floor(256 f), 255).
    list.push_back(op(0x1F, 1u));
    list.push_back(op(0xCD, f24(0.0f)));
    list.push_back(op(0xCE, f24(1.0f)));
    const auto d = draw(3u << 7u, 3u);
    list.insert(list.end(), d.begin(), d.end());
    pspweb::Ge ge(*memory);
    Recorder recorder;
    ge.set_gl(&recorder);
    run(*memory, ge, list);
    CHECK_EQ(recorder.vertices.size(), 3u);
    CHECK(!recorder.states.empty());
    CHECK(recorder.states.back().fog);
    CHECK_EQ(recorder.vertices[0].fog, 128.0f);
    CHECK_EQ(recorder.vertices[1].fog, 64.0f);
    CHECK_EQ(recorder.vertices[2].fog, 255.0f);
}

TEST_CASE("ge transform: vertices without a position are culled") {
    auto memory = std::make_unique<psprecomp::GuestMemory>();
    for (std::uint32_t i = 0; i < 3u; ++i) memory->store32(kVertices + i * 4u, 0xFF0000FFu);
    auto list = setup_3d();
    const auto d = draw(7u << 2u, 3u); // 8888 colour only
    list.insert(list.end(), d.begin(), d.end());
    pspweb::Ge ge(*memory);
    Recorder recorder;
    ge.set_gl(&recorder);
    run(*memory, ge, list);
    CHECK(recorder.vertices.empty());
}
