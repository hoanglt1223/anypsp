// SPDX-License-Identifier: GPL-2.0-or-later
// Software-rendered GE lists checking how textures are read from guest memory.

#include "test_harness.hpp"

#include "ge.hpp"

#include "psprecomp/guest_memory.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace {

constexpr std::uint32_t kList = 0x08800000u;
constexpr std::uint32_t kTexture = 0x08810000u;
constexpr std::uint32_t kClut = 0x08811000u;
constexpr std::uint32_t kVertices = 0x08812000u;
constexpr std::uint32_t kFrame = 0x04000000u;

std::uint32_t op(std::uint32_t cmd, std::uint32_t data) { return (cmd << 24u) | (data & 0xFFFFFFu); }

} // namespace

TEST_CASE("ge texture: a CLUT4 buffer width under 16 bytes uses 16-byte rows") {
    auto memory = std::make_unique<psprecomp::GuestMemory>();
    // 16x2 CLUT4 texture drawn with bufw 16 (8 bytes).  The GE rounds rows up to
    // 16 bytes, so row 1 starts at byte 16; bytes 8..15 are never sampled.
    for (std::uint32_t i = 0; i < 8u; ++i) {
        memory->store8(kTexture + i, 0x11u);
        memory->store8(kTexture + 8u + i, 0x33u);
        memory->store8(kTexture + 16u + i, 0x22u);
    }
    memory->store32(kClut + 4u, 0xFF0000FFu);  // 1: red
    memory->store32(kClut + 8u, 0xFF00FF00u);  // 2: green
    memory->store32(kClut + 12u, 0xFFFF0000u); // 3: blue
    const std::int16_t vertices[] = {0, 0, 0, 0, 0, 16, 2, 16, 2, 0}; // u, v, x, y, z per corner
    for (std::uint32_t i = 0; i < 10u; ++i) memory->store16(kVertices + i * 2u, static_cast<std::uint16_t>(vertices[i]));

    const std::vector<std::uint32_t> list = {
        op(0x9C, 0u), op(0x9D, 512u), op(0xD2, 3u), op(0xD4, 0u), op(0xD5, (271u << 10u) | 479u),
        op(0xB0, kClut & 0xFFFFFFu), op(0xB1, (kClut >> 8u) & 0x0F0000u), op(0xC5, 0xFF03u), op(0xC4, 2u),
        op(0xA0, kTexture & 0xFFFFFFu), op(0xA8, ((kTexture >> 8u) & 0x0F0000u) | 16u), op(0xB8, (1u << 8u) | 4u),
        op(0xC2, 0u), op(0xC3, 4u), op(0xC9, 0x103u), op(0x1E, 1u),
        op(0x10, (kVertices >> 8u) & 0x0F0000u), op(0x12, 0x800102u), op(0x01, kVertices & 0xFFFFFFu),
        op(0x04, (6u << 16u) | 2u), op(0x0F, 0u), op(0x0C, 0u),
    };
    for (std::size_t i = 0; i < list.size(); ++i) memory->store32(kList + static_cast<std::uint32_t>(i * 4u), list[i]);

    pspweb::Ge ge(*memory);
    ge.run_list(kList, 0u);
    CHECK_EQ(memory->load32(kFrame) & 0xFFFFFFu, 0x0000FFu);
    CHECK_EQ(memory->load32(kFrame + 15u * 4u) & 0xFFFFFFu, 0x0000FFu);
    CHECK_EQ(memory->load32(kFrame + 512u * 4u) & 0xFFFFFFu, 0x00FF00u);
    CHECK_EQ(memory->load32(kFrame + 512u * 4u + 15u * 4u) & 0xFFFFFFu, 0x00FF00u);
}
