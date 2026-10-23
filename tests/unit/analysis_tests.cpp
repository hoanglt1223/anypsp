// SPDX-License-Identifier: GPL-2.0-or-later
#include "elf_builder.hpp"
#include "test_harness.hpp"

#include "cfg.hpp"
#include "functions.hpp"
#include "jumptables.hpp"
#include "nid_db.hpp"

#include <filesystem>
#include <fstream>

using namespace anypsp_test;
using namespace anypsp::analysis;
namespace loader = anypsp::loader;

namespace {

constexpr std::uint32_t kBase = 0x08900000u;
constexpr std::uint32_t kJrRa = 0x03E00008u;

std::vector<std::uint8_t> code_at(std::size_t size, std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> words) {
    std::vector<std::uint8_t> bytes(size, 0);
    for (const auto &[offset, word] : words) put32(bytes, offset, word);
    return bytes;
}

std::uint32_t j_to(std::uint32_t opcode, std::uint32_t target) { return opcode | ((target >> 2) & 0x03FFFFFFu); }

struct Loaded {
    std::unique_ptr<psprecomp::GuestMemory> memory = std::make_unique<psprecomp::GuestMemory>();
    loader::LoadedModule module;
    std::vector<ExecutableRange> ranges;
};

Loaded load(std::vector<std::uint8_t> code) {
    Loaded l;
    l.module = loader::load_module(*l.memory, psprecomp::Elf32Image::from_bytes(build_elf(2, kBase, {{1, kBase, std::move(code)}}), "t"), "t", 0);
    l.ranges = {{kBase, l.module.end}};
    return l;
}

// sltiu/beq bound check, table at kBase + 0x100 with three cases and a default.
std::vector<std::uint8_t> switch_code() {
    return code_at(0x110, {
        {0x00, 0x2C830003u}, // sltiu v1, a0, 3
        {0x04, 0x10600007u}, // beqz v1, default (0x24)
        {0x08, 0x00041080u}, // sll v0, a0, 2 (delay slot)
        {0x0C, 0x3C030890u}, // lui v1, 0x0890
        {0x10, 0x00431021u}, // addu v0, v0, v1
        {0x14, 0x8C420100u}, // lw v0, 0x100(v0)
        {0x18, 0x00400008u}, // jr v0
        {0x24, kJrRa},
        {0x30, kJrRa}, {0x38, kJrRa}, {0x40, kJrRa},
        {0x100, kBase + 0x30u}, {0x104, kBase + 0x38u}, {0x108, kBase + 0x40u},
    });
}

} // namespace

TEST_CASE("cfg: a branch block ends after its delay slot") {
    auto l = load(code_at(0x20, {{0x00, 0x24080000u}, {0x04, 0x11000002u}, {0x08, 0x25080001u}, {0x0C, 0x24020001u},
                                 {0x10, 0x24020002u}, {0x14, kJrRa}}));
    const auto cfg = build_cfg(*l.memory, l.ranges, kBase);
    CHECK_EQ(cfg.blocks.size(), std::size_t{3});
    const auto &first = cfg.blocks.at(kBase);
    CHECK(first.exit == Exit::Branch);
    CHECK_EQ(first.end, kBase + 0x0Cu);
    CHECK_EQ(first.branch_pc, kBase + 4u);
    CHECK(!first.likely);
    CHECK((first.successors == std::vector<std::uint32_t>{kBase + 0x0Cu, kBase + 0x10u}));
    CHECK(cfg.blocks.at(kBase + 0x0Cu).exit == Exit::Fallthrough);
    CHECK(cfg.blocks.at(kBase + 0x10u).exit == Exit::Return);
    CHECK_EQ(cfg.blocks.at(kBase + 0x10u).end, kBase + 0x1Cu);
}

TEST_CASE("cfg: branch-likely is marked, its delay slot belongs to the taken edge") {
    auto l = load(code_at(0x20, {{0x00, 0x51000002u}, {0x04, 0x25080001u}, {0x08, 0x24020001u}, {0x0C, 0x24020002u}, {0x10, kJrRa}}));
    const auto cfg = build_cfg(*l.memory, l.ranges, kBase);
    CHECK(cfg.blocks.at(kBase).likely);
    CHECK_EQ(cfg.blocks.at(kBase).end, kBase + 8u);
}

TEST_CASE("cfg: a branch into another branch's delay slot gets its own block") {
    auto l = load(code_at(0x28, {
        {0x00, 0x14800003u},               // bnez a0, 0x10
        {0x08, 0x24020001u},               // addiu v0, zero, 1
        {0x0C, j_to(0x08000000u, kBase + 0x20u)}, // j 0x20
        {0x10, 0x24420005u},               // addiu v0, v0, 5 (delay slot and branch target)
        {0x14, kJrRa},
        {0x20, kJrRa},
    }));
    const auto cfg = build_cfg(*l.memory, l.ranges, kBase);
    CHECK(cfg.delay_slot_targets.contains(kBase + 0x10u));
    const auto &jump = cfg.blocks.at(kBase + 0x08u);
    CHECK(jump.exit == Exit::Jump);
    CHECK_EQ(jump.end, kBase + 0x14u);
    const auto &slot = cfg.blocks.at(kBase + 0x10u);
    CHECK(slot.exit == Exit::Return);
    CHECK_EQ(slot.end, kBase + 0x1Cu);
    CHECK(cfg.blocks.contains(kBase + 0x20u));
}

TEST_CASE("cfg: j to another function is a tail call, jal continues after its delay slot") {
    auto l = load(code_at(0x30, {{0x00, j_to(0x0C000000u, kBase + 0x20u)}, {0x08, j_to(0x08000000u, kBase + 0x28u)},
                                 {0x20, kJrRa}, {0x28, kJrRa}}));
    const std::set<std::uint32_t> entries = {kBase, kBase + 0x20u, kBase + 0x28u};
    CfgOptions options;
    options.function_entries = &entries;
    const auto cfg = build_cfg(*l.memory, l.ranges, kBase, options);
    CHECK(cfg.blocks.at(kBase).exit == Exit::Call);
    CHECK(cfg.calls.contains(kBase + 0x20u));
    CHECK(cfg.blocks.at(kBase + 8u).exit == Exit::TailCall);
    CHECK(!cfg.blocks.contains(kBase + 0x28u));
}

TEST_CASE("jumptables: bounded table behind an sltiu check") {
    auto l = load(switch_code());
    const auto table = resolve_jump_table(*l.memory, l.ranges, kBase + 0x18u, 0);
    CHECK(table.has_value());
    CHECK_EQ(table->table, kBase + 0x100u);
    CHECK(table->bounded);
    CHECK((table->targets == std::vector<std::uint32_t>{kBase + 0x30u, kBase + 0x38u, kBase + 0x40u}));
    const auto cfg = build_cfg(*l.memory, l.ranges, kBase);
    CHECK_EQ(cfg.jump_tables.size(), std::size_t{1});
    CHECK(cfg.blocks.contains(kBase + 0x38u));
    CHECK_EQ(cfg.blocks.at(kBase + 0x0Cu).successors.size(), std::size_t{3});
}

TEST_CASE("jumptables: an unbounded table stops at the first non-code word, jr through a plain register is unresolved") {
    auto code = switch_code();
    put32(code, 0x00, 0); // drop the sltiu bound
    put32(code, 0x04, 0);
    put32(code, 0x10C, 0x12345678u);
    auto l = load(code);
    const auto table = resolve_jump_table(*l.memory, l.ranges, kBase + 0x18u, 0);
    CHECK(table.has_value());
    CHECK(!table->bounded);
    CHECK_EQ(table->targets.size(), std::size_t{3});

    auto plain = load(code_at(0x10, {{0x00, 0x00800008u}})); // jr a0
    CHECK(!resolve_jump_table(*plain.memory, plain.ranges, kBase, 0).has_value());
    CHECK_EQ(build_cfg(*plain.memory, plain.ranges, kBase).unresolved_indirect_jumps.size(), std::size_t{1});
}

TEST_CASE("functions: jump-table targets and thread entries become function seeds") {
    auto code = switch_code();
    // 0x50: lui a1, 0x0890 / jal stub / addiu a1, a1, 0x70 (delay slot) / jr ra
    put32(code, 0x50, 0x3C050890u);
    put32(code, 0x54, j_to(0x0C000000u, kBase + 0x90u));
    put32(code, 0x58, 0x24A50070u);
    put32(code, 0x5C, kJrRa);
    put32(code, 0x70, kJrRa); // thread entry
    put32(code, 0x90, kJrRa); // import stub
    auto l = load(code);
    l.module.imports.push_back({"ThreadManForUser", loader::nid_from_name("sceKernelCreateThread"), kBase + 0x90u, false});
    DiscoveryOptions options;
    options.feedback = {kBase + 0x50u, 0x08000000u};
    const auto discovery = discover_functions(l.module, *l.memory, options);
    const auto &seeds = discovery.program.seeds;
    for (const auto target : {kBase + 0x30u, kBase + 0x38u, kBase + 0x40u}) {
        CHECK(seeds.contains(target));
        CHECK_EQ(seeds.at(target), std::string("jump_table_target"));
    }
    CHECK_EQ(seeds.at(kBase + 0x50u), std::string("feedback"));
    CHECK_EQ(discovery.feedback_outside.size(), std::size_t{1});
    CHECK_EQ(discovery.code_pointers.size(), std::size_t{1});
    CHECK_EQ(discovery.code_pointers[0].function, std::string("sceKernelCreateThread"));
    CHECK_EQ(discovery.code_pointers[0].address, kBase + 0x70u);
    CHECK(seeds.contains(kBase + 0x70u));
    CHECK(discovery.iterations >= 2u);
}

TEST_CASE("functions: code-pointer API names are in the NID database") {
    psprecomp::NidRegistry registry;
    (void)loader::load_nid_toml(loader::default_nid_database(), registry);
    for (const auto &api : code_pointer_apis())
        CHECK_EQ(registry.resolve(api.library, loader::nid_from_name(api.function)).value_or(""), std::string(api.function));
}

TEST_CASE("functions: feedback files are parsed strictly") {
    const auto path = std::filesystem::temp_directory_path() / "anypsp_feedback.txt";
    std::ofstream(path) << "# runtime feedback\n0x08804ABC missing_code\n0x08804ABC\n\n0x0880A000,extra\n";
    CHECK((read_feedback(path) == std::vector<std::uint32_t>{0x08804ABCu, 0x0880A000u}));
    for (const char *bad : {"08804ABC\n", "0x08804ABD\n", "0xZZ\n", "0x123456789\n"}) {
        std::ofstream(path) << bad;
        CHECK_THROWS(read_feedback(path));
    }
}

TEST_CASE("functions: unreferenced code after a function's final jump is seeded") {
    auto l = load(code_at(0x40, {
        {0x00, j_to(0x0C000000u, kBase + 0x18u)}, // jal stub 0
        {0x08, j_to(0x0C000000u, kBase + 0x28u)}, // jal stub 2
        {0x10, kJrRa},
        {0x18, kJrRa}, {0x20, kJrRa}, {0x28, kJrRa}, // stubs: the middle one is never called
        {0x34, 0x3C020890u}, {0x38, kJrRa},        // after a nop of padding: dead leaf function
    }));
    const auto discovery = discover_functions(l.module, *l.memory);
    const auto &seeds = discovery.program.seeds;
    CHECK(seeds.contains(kBase + 0x20u));
    CHECK_EQ(seeds.at(kBase + 0x20u), std::string("gap_after_return"));
    CHECK(seeds.contains(kBase + 0x34u));
}

TEST_CASE("functions: a j into the next function is seeded as a tail call") {
    auto l = load(code_at(0x30, {
        {0x00, j_to(0x08000000u, kBase + 0x10u)}, // j next (tail call)
        {0x08, kJrRa},
        {0x10, 0x3C020890u}, {0x14, kJrRa},      // only reached through the j
    }));
    const auto discovery = discover_functions(l.module, *l.memory);
    CHECK_EQ(discovery.program.seeds.at(kBase + 0x10u), std::string("tail_call_target"));
}

TEST_CASE("functions: a prologue reached by j after a call that does not return is a function") {
    auto l = load(code_at(0x40, {
        {0x00, 0x27BDFFF0u},                      // addiu sp, sp, -16
        {0x04, j_to(0x0C000000u, kBase + 0x30u)}, // jal abort (never returns)
        {0x10, 0x27BDFFF0u},                      // next function: prologue, only reached by the j below
        {0x14, kJrRa},
        {0x20, j_to(0x08000000u, kBase + 0x10u)}, // another function tail-calls it
        {0x30, kJrRa},
    }));
    const auto discovery = discover_functions(l.module, *l.memory);
    CHECK_EQ(discovery.program.seeds.at(kBase + 0x10u), std::string("tail_call_target"));
}
