// SPDX-License-Identifier: GPL-2.0-or-later

#include "functions.hpp"

#include "jumptables.hpp"
#include "nid_db.hpp"

#include "psprecomp/decoder.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <fstream>

namespace anypsp::analysis {
namespace {

using psprecomp::Error;

constexpr std::uint32_t kA1 = 5u, kA2 = 6u, kA3 = 7u, kT0 = 8u;

std::string seed_source_for_export(const loader::Export &e) {
    if (!e.library.empty()) return "export";
    switch (e.nid) {
    case loader::kNidModuleStart: return "module_start";
    case loader::kNidModuleStop: return "module_stop";
    case loader::kNidModuleBootStart: return "module_bootstart";
    case loader::kNidModuleRebootBefore: return "module_reboot_before";
    case loader::kNidModuleRebootPhase: return "module_reboot_phase";
    default: return "system_export";
    }
}


constexpr std::uint32_t kShfAlloc = 2u, kShfExecInstr = 4u, kShtProgbits = 1u;

// Address ranges of the image's sections with the given flags (code or data).
std::vector<ExecutableRange> section_ranges(const loader::LoadedModule &module, bool code) {
    std::vector<ExecutableRange> ranges;
    for (const auto &section : module.elf.sections()) {
        if (section.type != kShtProgbits || (section.flags & kShfAlloc) == 0u || section.size < 4u) continue;
        if (((section.flags & kShfExecInstr) != 0u) != code) continue;
        const std::uint32_t start = module.elf.section_runtime_address(section, module.base);
        ranges.push_back({start, start + (section.size & ~3u)});
    }
    std::sort(ranges.begin(), ranges.end(), [](const auto &a, const auto &b) { return a.start < b.start; });
    return ranges;
}

bool is_prologue(std::uint32_t word) { return (word & 0xFFFF8000u) == 0x27BD8000u; } // addiu sp, sp, -n

bool is_unconditional_transfer(const psprecomp::DecodedInstruction &d) {
    return d.kind == psprecomp::OpcodeKind::J || d.kind == psprecomp::OpcodeKind::Jr ||
           (d.kind == psprecomp::OpcodeKind::Beq && d.rs == 0u && d.rt == 0u);
}

// A function starts right after the previous function's final jump and delay
// slot (alignment nops aside). Seed what starts there when nobody references it
// (dead library code, pointers built at run time), or when it is only reached by
// a `j` from elsewhere, which the base analysis follows as if it were a branch
// inside the caller (a tail call).
void add_gap_seeds(const psprecomp::GuestMemory &memory, const std::vector<ExecutableRange> &code,
                   const std::set<std::uint32_t> &covered, const std::set<std::uint32_t> &entries,
                   const std::set<std::uint32_t> &jump_targets, std::map<std::uint32_t, std::string> &found) {
    constexpr std::uint32_t kEret = 0x42000018u;
    for (const auto &range : code) {
        bool after_end = true;
        bool after_eret = false;
        std::uint32_t end_at = 0u;
        for (std::uint32_t pc = range.start; pc + 4u <= range.end; pc += 4u) {
            if (pc == end_at) after_end = true;
            const auto d = psprecomp::decode_allegrex(memory.load32(pc));
            if (covered.contains(pc)) {
                if (after_end && d.word == 0u) continue; // alignment nops
                // Prologue + reached by a j also marks a function that follows a call
                // to a function that does not return.
                const bool tail_called = jump_targets.contains(pc) && (after_end || is_prologue(d.word));
                if (!entries.contains(pc) && (tail_called || (after_end && after_eret)))
                    found.try_emplace(pc, tail_called ? "tail_call_target" : "after_exception_return");
                after_end = false;
                after_eret = false;
                if (is_unconditional_transfer(d)) end_at = pc + 8u;
                if (d.word == kEret) {
                    end_at = pc + 4u; // eret has no delay slot
                    after_eret = true;
                }
                continue;
            }
            if (!after_end || d.word == 0u) continue;
            after_end = false;
            after_eret = false;
            if (d.kind == psprecomp::OpcodeKind::Unsupported) continue;
            if (!entries.contains(pc)) found.try_emplace(pc, "gap_after_return");
        }
    }
}

// ET_EXEC images carry no relocations, so code pointers stored in data (callback
// tables, vtables) are found by scanning the data sections for code addresses.
void add_data_pointer_seeds(const psprecomp::GuestMemory &memory, const std::vector<ExecutableRange> &data,
                            const std::vector<ExecutableRange> &code, std::map<std::uint32_t, std::string> &seeds) {
    for (const auto &range : data)
        for (std::uint32_t at = range.start; at + 4u <= range.end; at += 4u) {
            const std::uint32_t value = memory.load32(at);
            if (psprecomp::is_executable_address(code, value) &&
                psprecomp::decode_allegrex(memory.load32(value)).kind != psprecomp::OpcodeKind::Unsupported)
                seeds.try_emplace(value, "data_code_pointer");
        }
}

} // namespace

const std::vector<CodePointerApi> &code_pointer_apis() {
    static const std::vector<CodePointerApi> apis = {
        {"ThreadManForUser", "sceKernelCreateThread", kA1, "thread_entry"},
        {"ThreadManForUser", "sceKernelCreateCallback", kA1, "callback"},
        {"ThreadManForUser", "sceKernelSetAlarm", kA1, "alarm_handler"},
        {"ThreadManForUser", "sceKernelSetSysClockAlarm", kA1, "alarm_handler"},
        {"ThreadManForUser", "sceKernelSetVTimerHandler", kA2, "vtimer_handler"},
        {"ThreadManForUser", "sceKernelSetVTimerHandlerWide", kT0, "vtimer_handler"},
        {"ThreadManForUser", "sceKernelRegisterThreadEventHandler", kA3, "thread_event_handler"},
        {"InterruptManager", "sceKernelRegisterSubIntrHandler", kA2, "interrupt_handler"},
    };
    return apis;
}

Discovery discover_functions(const loader::LoadedModule &module, const psprecomp::GuestMemory &memory,
                             const DiscoveryOptions &options) {
    Discovery result;
    const auto sites = loader::analysis_sites(module.relocations);
    const std::uint32_t gp = module.info ? module.info->gp : 0u;

    std::map<std::uint32_t, const CodePointerApi *> api_stubs;
    for (const auto &api : code_pointer_apis()) {
        const std::uint32_t nid = loader::nid_from_name(api.function);
        for (const auto &import : module.imports)
            if (!import.variable && import.library == api.library && import.nid == nid) api_stubs[import.stub_address] = &api;
    }

    psprecomp::AnalysisOptions analysis;
    analysis.relocation_sites = &sites;
    for (const auto &e : module.exports)
        if (!e.variable) analysis.extra_seeds.try_emplace(e.address, seed_source_for_export(e));

    // Executable ranges are a property of the image, so a first run gives them for
    // checking feedback addresses.
    result.program = psprecomp::analyze_program(module.elf, memory, module.base, analysis);
    auto code = section_ranges(module, true);
    if (code.empty()) code = result.program.executable_ranges;
    if (!module.elf.is_psp_prx()) add_data_pointer_seeds(memory, section_ranges(module, false), code, analysis.extra_seeds);
    for (const auto address : options.feedback) {
        if (psprecomp::is_executable_address(result.program.executable_ranges, address)) {
            analysis.extra_seeds.try_emplace(address, "feedback");
            result.feedback_used.push_back(address);
        } else {
            result.feedback_outside.push_back(address);
        }
    }

    for (result.iterations = 1; result.iterations <= options.max_iterations; ++result.iterations) {
        result.program = psprecomp::analyze_program(module.elf, memory, module.base, analysis);
        const auto &ranges = result.program.executable_ranges;
        std::set<std::uint32_t> entries;
        for (const auto &[address, source] : result.program.seeds) entries.insert(address);

        CfgOptions cfg_options;
        cfg_options.function_entries = &entries;
        cfg_options.gp = gp;
        result.cfgs.clear();
        result.jump_tables.clear();
        result.code_pointers.clear();
        std::map<std::uint32_t, std::string> found;
        for (const auto &function : result.program.functions) {
            auto cfg = build_cfg(memory, ranges, function.entry, cfg_options);
            for (const auto &table : cfg.jump_tables) {
                for (const auto target : table.targets)
                    if (!entries.contains(target)) found.try_emplace(target, "jump_table_target");
                result.jump_tables.push_back(table);
            }
            for (const auto call_pc : cfg.call_sites) {
                const auto d = psprecomp::decode_allegrex(memory.load32(call_pc));
                if (d.kind != psprecomp::OpcodeKind::Jal) continue;
                const std::uint32_t target = ((call_pc + 4u) & 0xF0000000u) | (d.target << 2u);
                const auto api = api_stubs.find(target);
                if (api == api_stubs.end()) continue;
                const auto registers = constants_at_call(memory, ranges, call_pc, gp);
                const auto value = registers[api->second->argument_register];
                if (!value) continue;
                const bool executable = psprecomp::is_executable_address(ranges, *value);
                result.code_pointers.push_back({call_pc, api->second->function, *value, executable});
                if (executable && !entries.contains(*value)) found.try_emplace(*value, api->second->seed_source);
            }
            result.cfgs.emplace(function.entry, std::move(cfg));
        }
        std::set<std::uint32_t> jump_targets;
        for (const auto &[entry, cfg] : result.cfgs)
            for (const auto &[start, block] : cfg.blocks)
                if (block.exit == Exit::Jump) jump_targets.insert(block.target);
        add_gap_seeds(memory, code, result.program.covered_labels, entries, jump_targets, found);
        std::sort(result.jump_tables.begin(), result.jump_tables.end(),
                  [](const JumpTable &a, const JumpTable &b) { return a.jump_pc < b.jump_pc; });
        result.jump_tables.erase(std::unique(result.jump_tables.begin(), result.jump_tables.end(),
                                             [](const JumpTable &a, const JumpTable &b) { return a.jump_pc == b.jump_pc; }),
                                 result.jump_tables.end());
        if (found.empty()) break;
        for (auto &[address, source] : found) {
            analysis.extra_seeds.try_emplace(address, source);
            result.extra_seeds.try_emplace(address, source);
        }
    }
    if (result.iterations > options.max_iterations)
        throw Error("Function discovery did not settle after " + std::to_string(options.max_iterations) + " iterations");
    for (const auto &[address, source] : analysis.extra_seeds) result.extra_seeds.try_emplace(address, source);
    return result;
}

std::vector<std::uint32_t> read_feedback(const std::filesystem::path &path) {
    std::ifstream in(path);
    if (!in) throw Error("Cannot open feedback file " + path.string());
    std::vector<std::uint32_t> addresses;
    std::string line;
    for (std::size_t number = 1; std::getline(in, line); ++number) {
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') continue;
        const auto end = line.find_first_of(" \t\r,", first);
        const std::string token = line.substr(first, end == std::string::npos ? std::string::npos : end - first);
        const auto fail = [&] {
            throw Error(path.string() + ":" + std::to_string(number) + ": expected a code address like 0x08804ABC, got '" +
                        token + "'");
        };
        if (token.size() < 3u || token.size() > 10u || token[0] != '0' || (token[1] != 'x' && token[1] != 'X')) fail();
        std::uint32_t value = 0;
        for (std::size_t i = 2; i < token.size(); ++i) {
            const char c = token[i];
            std::uint32_t digit;
            if (c >= '0' && c <= '9') digit = static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<std::uint32_t>(c - 'A' + 10);
            else fail();
            value = (value << 4u) | digit;
        }
        if ((value & 3u) != 0u) fail();
        addresses.push_back(value);
    }
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
    return addresses;
}

} // namespace anypsp::analysis
