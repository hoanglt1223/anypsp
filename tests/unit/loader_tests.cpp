// SPDX-License-Identifier: GPL-2.0-or-later
#include "elf_builder.hpp"
#include "test_harness.hpp"

#include "elf_prx.hpp"
#include "nid_db.hpp"
#include "relocs.hpp"

#include <filesystem>
#include <fstream>

using namespace anypsp_test;
using namespace anypsp::loader;

namespace {

constexpr std::uint32_t kBase = 0x08900000u;
constexpr std::uint16_t kPrx = 0xFFA0u;
constexpr std::uint16_t kExec = 2u;

// lui a0, 0 / addiu a0, a0, 0x8010 / jal 0x40 / .word 0x20, before relocation.
std::vector<std::uint8_t> reloc_code() { return words({0x3C040000u, 0x24848010u, 0x0C000010u, 0x00000020u}); }

std::vector<std::uint8_t> classic_table() {
    std::vector<std::uint8_t> t;
    for (const auto [offset, type] : {std::pair{0u, 5u}, {4u, 6u}, {8u, 4u}, {12u, 2u}}) {
        append32(t, offset);
        append32(t, type); // patch segment 0, target segment 0
    }
    return t;
}

void check_relocated(const psprecomp::GuestMemory &memory) {
    CHECK_EQ(memory.load32(kBase + 0u), 0x3C040890u); // carry: 0x0890 << 16 + (s16)0x8010 = 0x088F8010
    CHECK_EQ(memory.load32(kBase + 4u), 0x24848010u);
    CHECK_EQ(memory.load32(kBase + 8u), 0x0E240010u);
    CHECK_EQ(memory.load32(kBase + 12u), kBase + 0x20u);
}

psprecomp::Elf32Image image(std::vector<std::uint8_t> bytes) {
    return psprecomp::Elf32Image::from_bytes(std::move(bytes), "test");
}

std::filesystem::path temp_file(const std::string &name, const std::string &text) {
    const auto path = std::filesystem::temp_directory_path() / ("anypsp_test_" + name);
    std::ofstream(path, std::ios::binary) << text;
    return path;
}

} // namespace

TEST_CASE("relocs: classic SHT_PSPREL section") {
    const auto elf = image(build_elf(kPrx, 0, {{1, 0, reloc_code()}}, {{".text", 1, reloc_code()}, {".rel.text", 0x700000A0u, classic_table()}}));
    psprecomp::GuestMemory memory;
    elf.load_into(memory, kBase);
    const auto result = apply_relocations(elf, memory, kBase);
    CHECK(result.format == RelocFormat::Sections);
    CHECK_EQ(result.applied, 4u);
    CHECK_EQ(result.invalid, 0u);
    check_relocated(memory);
    CHECK_EQ(result.sites.size(), std::size_t{4});
    CHECK_EQ(static_cast<unsigned>(result.sites[0].type), static_cast<unsigned>(kRelHi16));
}

TEST_CASE("relocs: 0x700000A0 program header without sections") {
    const auto elf = image(build_elf(kPrx, 0, {{1, 0, reloc_code()}, {0x700000A0u, 0, classic_table(), 0, 0}}));
    CHECK(elf.sections().empty());
    psprecomp::GuestMemory memory;
    elf.load_into(memory, kBase);
    const auto result = apply_relocations(elf, memory, kBase);
    CHECK(result.format == RelocFormat::Segments);
    CHECK_EQ(result.applied, 4u);
    check_relocated(memory);
}

TEST_CASE("relocs: packed 0x700000A1 program header") {
    // flag_bits 2, seg_bits 1 (relocation segment index 1), type_bits 3: shift 6.
    std::vector<std::uint8_t> packed = {0, 0, 2, 3};
    const std::vector<std::uint8_t> flags = {4, 0x00, 0x01, 0x11};   // 1: set base, 2: relocate, 3: relocate with lo16
    const std::vector<std::uint8_t> types = {5, 2, 3, 4, 5};         // 1: R32, 2: R26, 3: HI16, 4: LO16
    packed.insert(packed.end(), flags.begin(), flags.end());
    packed.insert(packed.end(), types.begin(), types.end());
    const auto command = [&](std::uint32_t value) { packed.push_back(static_cast<std::uint8_t>(value)); packed.push_back(static_cast<std::uint8_t>(value >> 8)); };
    command(1u | (0u << 2) | (0u << 3));             // base 0 in segment 0
    command(3u | (0u << 2) | (3u << 3) | (0u << 6)); // +0 HI16, lo16 follows
    command(0x8010u);
    command(2u | (0u << 2) | (4u << 3) | (4u << 6)); // +4 LO16
    command(2u | (0u << 2) | (2u << 3) | (4u << 6)); // +4 R26
    command(2u | (0u << 2) | (1u << 3) | (4u << 6)); // +4 R32
    const auto elf = image(build_elf(kPrx, 0, {{1, 0, reloc_code()}, {0x700000A1u, 0, packed, 0, 0}}));
    psprecomp::GuestMemory memory;
    elf.load_into(memory, kBase);
    const auto result = apply_relocations(elf, memory, kBase);
    CHECK(result.format == RelocFormat::PackedSegments);
    CHECK_EQ(result.invalid, 0u);
    CHECK_EQ(result.applied, 4u);
    check_relocated(memory);
}

TEST_CASE("relocs: truncated packed stream is reported, not read past the end") {
    const std::vector<std::uint8_t> packed = {0, 0, 2, 3, 4, 0, 1};
    const auto elf = image(build_elf(kPrx, 0, {{1, 0, reloc_code()}, {0x700000A1u, 0, packed, 0, 0}}));
    psprecomp::GuestMemory memory;
    elf.load_into(memory, kBase);
    const auto result = apply_relocations(elf, memory, kBase);
    CHECK(result.invalid > 0u);
    CHECK(!result.problems.empty());
}

TEST_CASE("relocs: tables cut off the end of the file load unrelocated") {
    Section rel{".rel.text", 0x700000A0u, classic_table()};
    rel.past_end = true;
    const auto elf = image(build_elf(kPrx, 0, {{1, 0, reloc_code()}}, {{".text", 1, reloc_code()}, rel}));
    psprecomp::GuestMemory memory;
    elf.load_into(memory, kBase);
    const auto result = apply_relocations(elf, memory, kBase);
    CHECK_EQ(result.truncated_tables, 1u);
    CHECK_EQ(result.applied, 0u);
    CHECK_EQ(memory.load32(kBase), 0x3C040000u);
}

TEST_CASE("relocs: R_MIPS_16 pairs with HI16, NONE and GPREL16 are ignored") {
    std::vector<std::uint8_t> table;
    for (const auto [offset, type] : {std::pair{0u, 0u}, {0u, 5u}, {4u, 1u}, {4u, 7u}}) {
        append32(table, offset);
        append32(table, type);
    }
    const auto elf = image(build_elf(kPrx, 0, {{1, 0, reloc_code()}, {0x700000A0u, 0, table, 0, 0}}));
    psprecomp::GuestMemory memory;
    elf.load_into(memory, kBase);
    const auto result = apply_relocations(elf, memory, kBase);
    CHECK_EQ(result.ignored, 2u);
    CHECK_EQ(result.applied, 2u);
    CHECK_EQ(memory.load32(kBase), 0x3C040890u);
}

namespace {

// ET_EXEC module at 0x08900000 with module info, one export library and imports,
// found through p_paddr (no section headers).
std::vector<std::uint8_t> module_image(const char *module_name, const char *export_library, std::uint32_t export_nid,
                                       const char *import_library, std::vector<std::uint32_t> import_nids) {
    constexpr std::uint32_t va = kBase;
    std::vector<std::uint8_t> d(0x200, 0);
    put32(d, 0x00, 0x03E00008u); // module_start: jr ra
    put32(d, 0x08, 0x03E00008u); // exported function
    // module info at 0x40
    put16(d, 0x40, 0);
    for (std::size_t i = 0; module_name[i] != '\0'; ++i) d[0x44 + i] = static_cast<std::uint8_t>(module_name[i]);
    put32(d, 0x40 + 32, va + 0x1F0); // gp
    put32(d, 0x40 + 36, va + 0x80);  // ent_top
    put32(d, 0x40 + 40, va + 0xA0);  // ent_end
    put32(d, 0x40 + 44, va + 0xA0);  // stub_top
    put32(d, 0x40 + 48, va + 0xB4);  // stub_end
    // exports: system entry (module_start) then the library
    put32(d, 0x80, 0); put16(d, 0x84, 0); put16(d, 0x86, 0x8000); d[0x88] = 4; d[0x89] = 0; put16(d, 0x8A, 1); put32(d, 0x8C, va + 0x100);
    put32(d, 0x90, va + 0x180); put16(d, 0x94, 1); put16(d, 0x96, 1); d[0x98] = 4; d[0x99] = 0; put16(d, 0x9A, 1); put32(d, 0x9C, va + 0x110);
    put32(d, 0x100, kNidModuleStart); put32(d, 0x104, va + 0x00);
    put32(d, 0x110, export_nid); put32(d, 0x114, va + 0x08);
    // imports
    put32(d, 0xA0, va + 0x1A0); put16(d, 0xA4, 0x11); put16(d, 0xA6, 0x4001); d[0xA8] = 5; d[0xA9] = 0;
    put16(d, 0xAA, static_cast<std::uint32_t>(import_nids.size())); put32(d, 0xAC, va + 0x120); put32(d, 0xB0, va + 0x140);
    for (std::size_t i = 0; i < import_nids.size(); ++i) put32(d, 0x120 + 4 * i, import_nids[i]);
    for (std::size_t i = 0; export_library[i] != '\0'; ++i) d[0x180 + i] = static_cast<std::uint8_t>(export_library[i]);
    for (std::size_t i = 0; import_library[i] != '\0'; ++i) d[0x1A0 + i] = static_cast<std::uint8_t>(import_library[i]);
    Segment segment{1, va, d};
    segment.paddr_data_offset = 0x40;
    return build_elf(kExec, va, {segment});
}

} // namespace

TEST_CASE("elf_prx: module info from p_paddr, imports and exports") {
    psprecomp::GuestMemory memory;
    const auto module = load_module(memory, image(module_image("child", "ChildLib", 0x11111111u, "ThreadManForUser", {0x446D8DE6u})), "child.prx", 0);
    CHECK(module.info.has_value());
    CHECK_EQ(module.info->name, std::string("child"));
    CHECK_EQ(module.base, kBase);
    CHECK_EQ(module.exports.size(), std::size_t{2});
    CHECK(module.exports[0].library.empty());
    CHECK_EQ(module.exports[0].nid, kNidModuleStart);
    CHECK_EQ(module.exports[1].library, std::string("ChildLib"));
    CHECK_EQ(module.exports[1].address, kBase + 8u);
    CHECK_EQ(module.imports.size(), std::size_t{1});
    CHECK_EQ(module.imports[0].stub_address, kBase + 0x140u);
}

TEST_CASE("elf_prx: imports link to game exports, HLE names, or are reported unknown") {
    psprecomp::GuestMemory memory_a;
    psprecomp::GuestMemory memory_b;
    std::vector<LoadedModule> modules;
    modules.push_back(load_module(memory_a, image(module_image("main", "MainLib", 0x22222222u, "ChildLib", {0x11111111u, 0x33333333u})), "main", 0));
    modules.push_back(load_module(memory_b, image(module_image("child", "ChildLib", 0x11111111u, "ThreadManForUser", {0x446D8DE6u, 0xDEADBEEFu})), "child", 0));
    psprecomp::NidRegistry nids;
    const auto links = link_imports(modules, nids);
    CHECK_EQ(links.size(), std::size_t{4});
    CHECK(links[0].target == ImportTarget::GameModule);
    CHECK_EQ(links[0].export_module, std::size_t{1});
    CHECK_EQ(links[0].export_address, kBase + 8u);
    CHECK(links[1].target == ImportTarget::Unknown);
    CHECK(links[2].target == ImportTarget::Hle);
    CHECK_EQ(links[2].name, std::string("sceKernelCreateThread"));
    CHECK(links[3].target == ImportTarget::Unknown);
}

TEST_CASE("elf_prx: unwrap PBP and refuse encrypted or compressed input") {
    const auto elf = build_elf(kExec, kBase, {{1, kBase, reloc_code()}});
    std::vector<std::uint8_t> pbp(0x28, 0);
    pbp[1] = 'P'; pbp[2] = 'B'; pbp[3] = 'P';
    for (int i = 0; i < 6; ++i) put32(pbp, 8 + 4 * static_cast<std::size_t>(i), 0x28);
    put32(pbp, 0x20, 0x28);
    put32(pbp, 0x24, 0x28 + static_cast<std::uint32_t>(elf.size()));
    pbp.insert(pbp.end(), elf.begin(), elf.end());
    CHECK(unwrap_executable(pbp, "EBOOT.PBP") == elf);
    CHECK_THROWS(unwrap_executable({'~', 'P', 'S', 'P', 0, 0}, "EBOOT.BIN"));
    CHECK_THROWS(unwrap_executable({0x1F, 0x8B, 8, 0}, "x.prx"));
    CHECK_THROWS(unwrap_executable({'N', 'O', 'P', 'E'}, "x.prx"));
}

TEST_CASE("elf_prx: layout stacks extra modules from the top and rejects overlaps") {
    const auto bases = plan_layout({{"EBOOT.BIN", 0x10000, true}, {"a.prx", 0x1234, true}, {"b.prx", 0x800, true}});
    CHECK_EQ(bases[0], 0x08804000u);
    CHECK_EQ(bases[1], 0x09FFED00u);
    CHECK_EQ(bases[2], 0x09FFE500u);
    CHECK_EQ(plan_layout({{"EBOOT.BIN", 0x100, true}, {"a.prx", 0x100, true}}, {{"a.prx", 0x09000000u}})[1], 0x09000000u);
    CHECK_THROWS(plan_layout({{"EBOOT.BIN", 0x10000, true}, {"a.prx", 0x100, true}}, {{"a.prx", 0x08804100u}}));
    const auto path = temp_file("layout.txt", "# comment\n0x09000000 disc0:/PSP_GAME/USRDIR/a b.prx\n");
    const auto layout = read_layout_file(path);
    CHECK_EQ(layout.at("disc0:/PSP_GAME/USRDIR/a b.prx"), 0x09000000u);
    CHECK_THROWS(read_layout_file(temp_file("layout_bad.txt", "0x09000001 a.prx\n")));
    CHECK_THROWS(read_layout_file(temp_file("layout_bad2.txt", "zz a.prx\n")));
}

TEST_CASE("elf_prx: function symbols from .symtab") {
    std::vector<std::uint8_t> strings = {0};
    const std::string name = "main";
    strings.insert(strings.end(), name.begin(), name.end());
    strings.push_back(0);
    std::vector<std::uint8_t> symtab(16, 0); // null symbol
    append32(symtab, 1); append32(symtab, 0x10); append32(symtab, 8);
    symtab.push_back(0x12); symtab.push_back(0); symtab.push_back(1); symtab.push_back(0); // FUNC, shndx 1
    append32(symtab, 1); append32(symtab, 0x20); append32(symtab, 8);
    symtab.push_back(0x11); symtab.push_back(0); symtab.push_back(1); symtab.push_back(0); // OBJECT: skipped
    const auto elf = image(build_elf(kPrx, 0, {{1, 0, reloc_code()}},
                                     {{".text", 1, reloc_code()}, {".symtab", 2, symtab, 0, 3, 0, 16}, {".strtab", 3, strings}}));
    const auto symbols = read_function_symbols(elf, kBase);
    CHECK_EQ(symbols.size(), std::size_t{1});
    CHECK_EQ(symbols.at(kBase + 0x10u), name);
}

TEST_CASE("nid_db: SHA-1 and NIDs") {
    const auto digest = sha1("abc");
    CHECK_EQ(static_cast<unsigned>(digest[0]), 0xA9u);
    CHECK_EQ(static_cast<unsigned>(digest[19]), 0x9Du);
    CHECK_EQ(nid_from_name("sceKernelCreateThread"), 0x446D8DE6u);
    CHECK_EQ(nid_from_name("sceIoOpen"), 0x109F50BCu);
}

TEST_CASE("nid_db: TOML subset is parsed strictly") {
    psprecomp::NidRegistry registry;
    const auto good = temp_file("nids.toml", "# x\n[\"MyLib\"]\n0x00000001 = \"one\"\n0x00000001 = \"one\"\n\n[\"Other\"]\n0xABCDEF01 = \"two\"\n");
    CHECK_EQ(load_nid_toml(good, registry), std::size_t{2});
    CHECK_EQ(registry.resolve("Other", 0xABCDEF01u).value_or(""), std::string("two"));
    for (const char *bad : {"0x00000001 = \"x\"\n", "[\"L\"]\n0x1 = \"x\"\n", "[\"L\"]\n0x00000001 = x\n",
                            "[\"L\"]\n0x00000001 = \"a\"\n0x00000001 = \"b\"\n", "[L]\n", "[\"L\"]\n0x0000000G = \"x\"\n"}) {
        psprecomp::NidRegistry scratch;
        CHECK_THROWS(load_nid_toml(temp_file("nids_bad.toml", bad), scratch));
    }
}

TEST_CASE("nid_db: the shipped database loads and names common NIDs") {
    psprecomp::NidRegistry registry;
    CHECK(load_nid_toml(default_nid_database(), registry) > 3000u);
    CHECK_EQ(registry.resolve("ThreadManForUser", 0x446D8DE6u).value_or(""), std::string("sceKernelCreateThread"));
    CHECK_EQ(registry.resolve("sceDisplay", 0x0E20F177u).value_or(""), std::string("sceDisplaySetMode"));
}
