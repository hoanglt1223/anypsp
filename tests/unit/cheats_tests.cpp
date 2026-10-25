// SPDX-License-Identifier: GPL-2.0-or-later
// CWCheat files: parsing, refusals, every code type against guest memory, saving the switches.

#include "test_harness.hpp"

#include "cheats.hpp"

#include "psprecomp/guest_memory.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace fs = std::filesystem;
using anypsp::cheats::Cheat;
using anypsp::cheats::CheatFile;
using anypsp::cheats::Line;

namespace {

// 0x00100000 in a code is 0x08900000.
constexpr std::uint32_t kBase = 0x08900000u;

std::size_t run(psprecomp::GuestMemory &m, std::initializer_list<Line> lines, std::uint32_t buttons = 0) {
    Cheat c;
    c.lines = lines;
    return anypsp::cheats::run(c, m, buttons);
}

std::string error_of(const std::string &code) {
    const CheatFile f = CheatFile::parse("_S ULUS-10299\n_C1 Test\n" + code, "ULUS10299");
    return f.cheats().empty() ? "(no cheat)" : f.cheats()[0].error;
}

std::string slurp(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("Cheat files: games, names, switches and code lines as PPSSPP reads them") {
    const std::string text = "\xEF\xBB\xBF_S ULUS-10299\n"
                             "_G Naruto: Ultimate Ninja Heroes\n"
                             "// a comment\n"
                             "_C0 Max money\n"
                             "_L 0x20123456 0x0001869F\n"
                             "_C1 Timer\n"
                             "_L 0xE1010000 0x00200000   # trailing comment\n"
                             "_L 0x00200001 0x00000063\n"
                             "_C1 Section title\n"
                             "stray text\n"
                             "_S ULES-00999\n"
                             "_C1 Other game\n"
                             "_L 0x00000000 0x00000001\n";
    const CheatFile f = CheatFile::parse(text, "ULUS10299", "t.ini");
    CHECK_EQ(f.cheats().size(), 3u);
    CHECK(f.cheats()[0].name == "Max money");
    CHECK(!f.cheats()[0].enabled);
    CHECK_EQ(f.cheats()[0].file_line, 4);
    CHECK_EQ(f.cheats()[0].lines.size(), 1u);
    CHECK_EQ(f.cheats()[0].lines[0].a, 0x20123456u);
    CHECK_EQ(f.cheats()[0].lines[0].b, 0x0001869Fu);
    CHECK(f.cheats()[1].enabled);
    CHECK_EQ(f.cheats()[1].lines.size(), 2u);
    CHECK(f.cheats()[1].error.empty());
    CHECK(f.cheats()[2].lines.empty());
    CHECK_EQ(f.warnings().size(), 1u);
    CHECK(f.warnings()[0].starts_with("line 10:"));

    // The other game's cheats, by either form of its ID.
    const CheatFile other = CheatFile::parse(text, "ULES-00999");
    CHECK_EQ(other.cheats().size(), 1u);
    CHECK(other.cheats()[0].name == "Other game");
    // A file for one game is used whatever its ID (PPSSPP does the same); with several, only matches.
    CHECK_EQ(CheatFile::parse("_S ULUS-10299\n_C1 A\n_L 0x00000000 0x00000001\n", "NPJH50000").cheats().size(), 1u);
    CHECK(CheatFile::parse(text, "NPJH50000").cheats().empty());
}

TEST_CASE("Cheats with codes that change no memory or do not parse are refused with the reason") {
    CHECK(error_of("_L 0x20000000 0x00000001\n").empty());
    CHECK(error_of("_L 0xA0000000 0x00000000\n").find("vibration") != std::string::npos);
    CHECK(error_of("_M 0x00000000 0x00000001\n").find("Action Replay") != std::string::npos);
    CHECK(error_of("_N 0x00000000 0x00000001\n").find("TempAR") != std::string::npos);
    CHECK(error_of("_L 0x90000000 0x00000001\n").find("unknown code type") != std::string::npos);
    CHECK(error_of("_L 0xF0000000 0x00000001\n").find("unknown code type") != std::string::npos);
    CHECK(error_of("_L 0x20000000\n").find("two hex numbers") != std::string::npos);
    CHECK(error_of("_L 0x20000000 0x1 junk\n").find("junk") != std::string::npos);
    CHECK(error_of("_L 0x40000000 0x00010001\n").find("second line") != std::string::npos);
    CHECK(error_of("_L 0x30700000 0x00000000\n").find("increment") != std::string::npos);
    CHECK(error_of("_L 0x70000000 0x00060000\n").find("boolean") != std::string::npos);
    CHECK(error_of("_L 0xD0000000 0x00400000\n").find("comparison") != std::string::npos);
    CHECK(error_of("_L 0x60000000 0x00000001\n_L 0x00000003 0x00000000\n_L 0x90000000 0x00000000\n").find("fewer lines") !=
          std::string::npos);
    CHECK(error_of("_L 0x60000000 0x00000001\n_L 0x00060001 0x00000000\n").find("pointer write type") != std::string::npos);
    CHECK(error_of("_L 0x60000000 0x00000001\n_L 0x00000002 0x00000000\n_L 0x50000000 0x00000000\n").find("pointer line") !=
          std::string::npos);
    // The message ends with the file line and the code.
    const std::string e = error_of("_L 0x20000000 0x00000001\n_L 0xA1000000 0x08800000\n");
    CHECK(e.starts_with("vibration"));
    CHECK(e.ends_with(" (line 4: _L 0xA1000000 0x08800000)"));
    CHECK(error_of("_L 0x20000000\n").ends_with("(line 3)"));
    // A refused cheat cannot be turned on, and never runs.
    CheatFile f = CheatFile::parse("_C1 Bad\n_L 0x20100000 0x00000001\n_L 0xA0000000 0x00000000\n", "");
    CHECK(!f.set_enabled(0, true));
    auto memory = std::make_unique<psprecomp::GuestMemory>();
    CHECK_EQ(f.apply(*memory, 0), 0u);
    CHECK_EQ(memory->load32(kBase), 0u);
}

TEST_CASE("Cheat writes, increments, boolean operations, serial writes and copies") {
    auto memory = std::make_unique<psprecomp::GuestMemory>();
    auto &m = *memory;
    // 0x0: 8 bits, or 16/32 when the value needs them; 0x1: 16 bits; 0x2: 32 bits.
    CHECK_EQ(run(m, {{0x00100000u, 0x000000ABu}}), 1u);
    CHECK_EQ(m.load32(kBase), 0xABu);
    run(m, {{0x00100000u, 0x00001234u}});
    CHECK_EQ(m.load32(kBase), 0x1234u);
    run(m, {{0x10100002u, 0x0000BEEFu}, {0x20100004u, 0xCAFEF00Du}});
    CHECK_EQ(m.load16(kBase + 2), 0xBEEFu);
    CHECK_EQ(m.load32(kBase + 4), 0xCAFEF00Du);
    CHECK_EQ(run(m, {{0x20100004u, 0xCAFEF00Du}}), 0u);  // already that value
    // Outside memory: nothing happens.
    CHECK_EQ(run(m, {{0x2F000000u, 1u}}), 0u);

    // 0x3: increment and decrement, 8, 16 and 32 bits (the 32-bit amount on the next line).
    m.store32(kBase, 0x00000010u);
    run(m, {{0x30100005u, 0x00100000u}});
    CHECK_EQ(m.load8(kBase), 0x15u);
    m.store16(kBase + 2, 0x0005u);
    run(m, {{0x30400010u, 0x00100002u}});
    CHECK_EQ(m.load16(kBase + 2), 0xFFF5u);
    m.store32(kBase + 4, 1u);
    run(m, {{0x30500000u, 0x00100004u}, {0x00010000u, 0u}});
    CHECK_EQ(m.load32(kBase + 4), 0x00010001u);
    run(m, {{0x30600000u, 0x00100004u}, {0x00000002u, 0u}});
    CHECK_EQ(m.load32(kBase + 4), 0x0000FFFFu);

    // 0x7: OR, AND, XOR.
    m.store32(kBase, 0x0000000Fu);
    run(m, {{0x70100000u, 0x000000F0u}});
    CHECK_EQ(m.load8(kBase), 0xFFu);
    run(m, {{0x70100000u, 0x00020F0Fu}});
    CHECK_EQ(m.load8(kBase), 0x0Fu);
    m.store16(kBase + 2, 0x1010u);
    run(m, {{0x70100002u, 0x00051111u}});
    CHECK_EQ(m.load16(kBase + 2), 0x0101u);
    run(m, {{0x70100002u, 0x00011000u}});
    CHECK_EQ(m.load16(kBase + 2), 0x1101u);

    // 0x4: 32-bit serial write: 3 values 8 bytes apart, each 1 more.
    CHECK_EQ(run(m, {{0x40100010u, 0x00030002u}, {0x00000100u, 0x00000001u}}), 3u);
    CHECK_EQ(m.load32(kBase + 0x10), 0x100u);
    CHECK_EQ(m.load32(kBase + 0x18), 0x101u);
    CHECK_EQ(m.load32(kBase + 0x20), 0x102u);
    CHECK_EQ(m.load32(kBase + 0x28), 0u);
    // 0x8: 8-bit (value line 0x000000VV) and 16-bit (0x0001VVVV) serial writes.
    run(m, {{0x80100040u, 0x00040001u}, {0x000000AAu, 0x00000001u}});
    CHECK_EQ(m.load32(kBase + 0x40), 0xADACABAAu);
    run(m, {{0x80100050u, 0x00020002u}, {0x00011234u, 0x00000010u}});
    CHECK_EQ(m.load16(kBase + 0x50), 0x1234u);
    CHECK_EQ(m.load16(kBase + 0x54), 0x1244u);

    // 0x5: copy 4 bytes from 0x08900040 to 0x08900100.
    run(m, {{0x50100040u, 0x00000004u}, {0x00100100u, 0u}});
    CHECK_EQ(m.load32(kBase + 0x100), 0xADACABAAu);

    // 0xB (delay) does nothing.
    CHECK_EQ(run(m, {{0xB0000000u, 0x00000010u}}), 0u);
}

TEST_CASE("Cheat pointer codes write through pointers, walk them and copy between them") {
    auto memory = std::make_unique<psprecomp::GuestMemory>();
    auto &m = *memory;
    const Line ptr16{0x60100100u, 0x0000ABCDu}, type16_at_16{0x00010001u, 0x00000010u};
    // Null pointer: nothing set up yet, nothing written.
    CHECK_EQ(run(m, {ptr16, type16_at_16}), 0u);
    m.store32(kBase + 0x100, 0x08A00000u);
    CHECK_EQ(run(m, {ptr16, type16_at_16}), 1u);
    CHECK_EQ(m.load16(0x08A00010u), 0xABCDu);
    // Inverse 32-bit write: pointer minus the offset.
    run(m, {{0x60100100u, 0x11223344u}, {0x00050001u, 0x00000004u}});
    CHECK_EQ(m.load32(0x089FFFFCu), 0x11223344u);
    // A 0x9 line moves the address and adds to the value.
    run(m, {{0x60100100u, 0x00000005u}, {0x00000002u, 0x00000000u}, {0x90000020u, 0x00000001u}});
    CHECK_EQ(m.load8(0x08A00020u), 6u);
    // Walk: the pointer at 0x08A00000 + 8 leads to 0x08B00000.
    m.store32(0x08A00008u, 0x08B00000u);
    run(m, {{0x60100100u, 0x000000EEu}, {0x00000002u, 0x00000004u}, {0x20000008u, 0x00000000u}});
    CHECK_EQ(m.load8(0x08B00004u), 0xEEu);
    // Walk backwards twice: 0x08B00000 - 0x10 holds 0x08C00000, 0x08C00000 + 4 holds 0x08D00000.
    m.store32(0x08AFFFF0u, 0x08C00000u);
    m.store32(0x08C00004u, 0x08D00000u);
    m.store32(kBase + 0x100, 0x08B00000u);
    m.store32(0x08B00000u - 0x10u, 0x08C00000u);
    run(m, {{0x60100100u, 0x00000077u}, {0x00000002u, 0x00000000u}, {0x30000010u, 0x20000004u}});
    CHECK_EQ(m.load8(0x08D00000u), 0x77u);
    // Copy 4 bytes from the first pointer to the second (at base offset 4) plus 0x10.
    m.store32(kBase + 0x100, 0x08A00000u);
    m.store32(kBase + 0x104, 0x08C00000u);
    m.store32(0x08A00000u, 0x04030201u);
    run(m, {{0x60100100u, 0x00000004u}, {0x00100002u, 0x00000000u}, {0x10000010u, 0x00000000u}});
    CHECK_EQ(m.load32(0x08C00010u), 0x04030201u);
}

TEST_CASE("Cheat conditions: values, addresses, buttons and the code stopper") {
    auto memory = std::make_unique<psprecomp::GuestMemory>();
    auto &m = *memory;
    const Line set_flag{0x00100100u, 0x00000001u}, set_flag2{0x00100101u, 0x00000001u};
    const auto flags = [&] {
        const std::uint32_t f = m.load16(kBase + 0x100);
        m.store16(kBase + 0x100, 0);
        return f;
    };
    // 0xD 16-bit equal, skipping the next line when false.
    m.store16(kBase, 5);
    run(m, {{0xD0100000u, 0x00000005u}, set_flag, set_flag2});
    CHECK_EQ(flags(), 0x0101u);
    run(m, {{0xD0100000u, 0x00000006u}, set_flag, set_flag2});
    CHECK_EQ(flags(), 0x0100u);
    // 0xD 8-bit: not equal, less, greater.
    run(m, {{0xD0100000u, 0x20100005u}, set_flag});
    CHECK_EQ(flags(), 0u);
    run(m, {{0xD0100000u, 0x20200006u}, set_flag});
    CHECK_EQ(flags(), 1u);
    run(m, {{0xD0100000u, 0x20300004u}, set_flag});
    CHECK_EQ(flags(), 1u);
    run(m, {{0xD0100000u, 0x20300005u}, set_flag});
    CHECK_EQ(flags(), 0u);
    // 0xE: 8-bit equal skipping 1 line; 16-bit not equal skipping 2.
    run(m, {{0xE1010005u, 0x00100000u}, set_flag, set_flag2});
    CHECK_EQ(flags(), 0x0101u);
    run(m, {{0xE1010007u, 0x00100000u}, set_flag, set_flag2});
    CHECK_EQ(flags(), 0x0100u);
    run(m, {{0xE0020005u, 0x10100000u}, set_flag, set_flag2});
    CHECK_EQ(flags(), 0u);
    run(m, {{0xE0020009u, 0x10100000u}, set_flag, set_flag2});
    CHECK_EQ(flags(), 0x0101u);
    // 0xD 0x4-0x7: two addresses, 32 bits.
    m.store32(kBase + 4, 5);
    m.store32(kBase, 5);
    run(m, {{0xD0100000u, 0x40100004u}, {0x00000001u, 0x00000002u}, set_flag});
    CHECK_EQ(flags(), 1u);
    run(m, {{0xD0100000u, 0x50100004u}, {0x00000001u, 0x00000002u}, set_flag});
    CHECK_EQ(flags(), 0u);
    m.store32(kBase + 4, 9);
    run(m, {{0xD0100000u, 0x60100004u}, {0x00000001u, 0x00000002u}, set_flag});
    CHECK_EQ(flags(), 1u);
    // Joker: Cross held runs the next line; not held (0x3) skips (low byte + 1) lines while held.
    constexpr std::uint32_t kCross = 0x4000u;
    run(m, {{0xD0000000u, 0x10004000u}, set_flag, set_flag2}, kCross);
    CHECK_EQ(flags(), 0x0101u);
    run(m, {{0xD0000000u, 0x10004000u}, set_flag, set_flag2}, 0);
    CHECK_EQ(flags(), 0x0100u);
    run(m, {{0xD0000001u, 0x30004000u}, set_flag, set_flag2}, kCross);
    CHECK_EQ(flags(), 0u);
    run(m, {{0xD0000001u, 0x30004000u}, set_flag, set_flag2}, 0);
    CHECK_EQ(flags(), 0x0101u);
    // 0xC: the rest of the cheat runs only while the word is equal.
    m.store32(kBase, 0x12345678u);
    run(m, {{0xC0100000u, 0x12345678u}, set_flag});
    CHECK_EQ(flags(), 1u);
    run(m, {{0xC0100000u, 0x12345679u}, set_flag});
    CHECK_EQ(flags(), 0u);
}

TEST_CASE("Cheat switches are saved back into the file, keeping every other byte") {
    const fs::path dir = fs::temp_directory_path() / "anypsp-cheats-test";
    fs::remove_all(dir);
    fs::create_directories(dir / "user" / "cheats");
    fs::create_directories(dir / "program" / "cheats");
    const fs::path path = dir / "user" / "cheats" / "ULUS10299.ini";
    const std::string text = "\xEF\xBB\xBF_S ULUS-10299\r\n_G UNH\r\n_C0 Money\r\n_L 0x20100000 0x00000063\r\n"
                             "  _C1 Timer\r\n_L 0x10100004 0x00000063\r\n_C0 Bad\r\n_L 0xA0000000 0x0\r\n";
    std::ofstream(path, std::ios::binary) << text;
    CHECK(anypsp::cheats::find_file("ULUS-10299", dir / "program", dir / "user") == path);
    CHECK(anypsp::cheats::find_file("ULES-00000", dir / "program", dir / "user").empty());
    CHECK(anypsp::cheats::file_id("ULUS-10299") == "ULUS10299");
    CheatFile f = CheatFile::load(path, "ULUS-10299");
    CHECK_EQ(f.cheats().size(), 3u);
    auto memory = std::make_unique<psprecomp::GuestMemory>();
    CHECK_EQ(f.apply(*memory, 0), 1u);  // Timer only
    CHECK_EQ(memory->load16(kBase + 4), 0x63u);
    CHECK_EQ(memory->load32(kBase), 0u);
    CHECK(f.set_enabled(0, true));
    CHECK(f.set_enabled(1, false));
    CHECK(!f.set_enabled(2, true));
    f.save();
    std::string expected = text;
    expected.replace(expected.find("_C0 Money"), 3, "_C1");
    expected.replace(expected.find("_C1 Timer"), 3, "_C0");
    CHECK(slurp(path) == expected);
    const CheatFile again = CheatFile::load(path, "ULUS10299");
    CHECK(again.cheats()[0].enabled);
    CHECK(!again.cheats()[1].enabled);
    CHECK_EQ(again.apply(*memory, 0), 1u);
    CHECK_EQ(memory->load32(kBase), 0x63u);

    // The program's folder comes first; the file changing underneath is not overwritten.
    std::ofstream(dir / "program" / "cheats" / "ULUS-10299.ini", std::ios::binary) << "_C1 Other\n";
    CHECK(anypsp::cheats::find_file("ULUS-10299", dir / "program", dir / "user") == dir / "program" / "cheats" / "ULUS-10299.ini");
    std::ofstream(path, std::ios::binary) << "_C0 Something else\n";
    CHECK_THROWS(f.save());
    CHECK(slurp(path) == "_C0 Something else\n");
    CHECK_THROWS(CheatFile::load(dir / "missing.ini", ""));
    fs::remove_all(dir);
}
