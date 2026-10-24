// SPDX-License-Identifier: GPL-2.0-or-later
// Per-game memory patch files.

#include "test_harness.hpp"

#include "patches.hpp"

#include "psprecomp/guest_memory.hpp"

#include <filesystem>
#include <fstream>
#include <memory>

namespace fs = std::filesystem;
using anypsp::patches::When;

TEST_CASE("Patch files parse plain and pointer writes with conditions") {
    const auto p = anypsp::patches::parse("# comment\n"
                                          "load u32 0x08900000 = 0x24040001\n"
                                          "vblank u8 [0x08C20C70]+0x1 = 1 if 2   # 60 fps\n"
                                          "\n"
                                          "vblank u16 0x08900010 = 300\n",
                                          "t.ini");
    CHECK_EQ(p.size(), 3u);
    CHECK(p[0].when == When::Load);
    CHECK_EQ(p[0].size, 4u);
    CHECK_EQ(p[0].value, 0x24040001u);
    CHECK(p[1].when == When::Vblank);
    CHECK(p[1].pointer);
    CHECK_EQ(p[1].address, 0x08C20C70u);
    CHECK_EQ(p[1].offset, 1u);
    CHECK_EQ(p[1].size, 1u);
    CHECK(p[1].only_if.has_value() && *p[1].only_if == 2u);
    CHECK_EQ(p[2].value, 300u);
}

TEST_CASE("Mistakes in a patch file are refused") {
    CHECK_THROWS(anypsp::patches::parse("sometimes u8 0x08900000 = 1\n", "x"));
    CHECK_THROWS(anypsp::patches::parse("load u64 0x08900000 = 1\n", "x"));
    CHECK_THROWS(anypsp::patches::parse("load u8 0x08900000 1\n", "x"));
    CHECK_THROWS(anypsp::patches::parse("load u8 0x00001000 = 1\n", "x"));   // outside user memory
    CHECK_THROWS(anypsp::patches::parse("load u32 0x08900002 = 1\n", "x"));  // misaligned
    CHECK_THROWS(anypsp::patches::parse("load u8 0x08900000 = 0x100\n", "x"));
    CHECK_THROWS(anypsp::patches::parse("load u8 [0x08900000 = 1\n", "x"));
    CHECK_THROWS(anypsp::patches::parse("load u8 0x08900000 = 1 unless 2\n", "x"));
}

TEST_CASE("Patches write only when due, through valid pointers and matching old values") {
    auto memory = std::make_unique<psprecomp::GuestMemory>();
    auto &m = *memory;
    const auto p = anypsp::patches::parse("load u32 0x08900000 = 7\n"
                                          "vblank u8 [0x08900100]+1 = 1 if 2\n",
                                          "t");
    CHECK_EQ(anypsp::patches::apply(p, When::Load, m), 1u);
    CHECK_EQ(m.load32(0x08900000u), 7u);
    // Null pointer: nothing set up yet, nothing written.
    CHECK_EQ(anypsp::patches::apply(p, When::Vblank, m), 0u);
    m.store32(0x08900100u, 0x08A00000u);
    m.store8(0x08A00001u, 2u);
    CHECK_EQ(anypsp::patches::apply(p, When::Vblank, m), 1u);
    CHECK_EQ(m.load8(0x08A00001u), 1u);
    CHECK_EQ(anypsp::patches::apply(p, When::Vblank, m), 0u); // already 1
    m.store8(0x08A00001u, 3u);
    CHECK_EQ(anypsp::patches::apply(p, When::Vblank, m), 0u); // not the expected old value
    CHECK_EQ(m.load8(0x08A00001u), 3u);
}

TEST_CASE("Patch files are found by disc ID") {
    const fs::path dir = fs::temp_directory_path() / "anypsp-patches-test";
    fs::remove_all(dir);
    fs::create_directories(dir / "disc");
    fs::create_directories(dir / "user" / "patches");
    std::ofstream(dir / "disc" / "UMD_DATA.BIN", std::ios::binary) << "ULUS-10299|B3D5A81C6A3E8A2E|0001|G";
    std::ofstream(dir / "user" / "patches" / "ULUS-10299.ini") << "\n";
    const std::string id = anypsp::patches::disc_id(dir / "disc");
    CHECK(id == "ULUS-10299");
    CHECK(anypsp::patches::find_file(id, dir / "program", dir / "user") == dir / "user" / "patches" / "ULUS-10299.ini");
    CHECK(anypsp::patches::find_file("ULES-00000", dir / "program", dir / "user").empty());
    CHECK(anypsp::patches::disc_id(dir / "nothing").empty());
    fs::remove_all(dir);
}
