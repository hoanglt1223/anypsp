// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cfg.hpp"

#include "elf_prx.hpp"

#include "psprecomp/program_analysis.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace anypsp::analysis {

// A system call that takes a code address in an argument register.
struct CodePointerApi {
    const char *library;
    const char *function;
    std::uint32_t argument_register;
    const char *seed_source;
};
[[nodiscard]] const std::vector<CodePointerApi> &code_pointer_apis();

struct CodePointer {
    std::uint32_t call_pc{};
    std::string function;
    std::uint32_t address{};
    bool executable{};
};

struct DiscoveryOptions {
    std::vector<std::uint32_t> feedback; // code addresses the runtime reached without translated code
    std::size_t max_iterations{16u};
};

struct Discovery {
    psprecomp::ProgramAnalysis program;
    std::map<std::uint32_t, std::string> extra_seeds;
    std::map<std::uint32_t, FunctionCfg> cfgs;
    std::vector<JumpTable> jump_tables;
    std::vector<CodePointer> code_pointers;
    std::vector<std::uint32_t> feedback_used;
    std::vector<std::uint32_t> feedback_outside; // not code of this module
    std::size_t iterations{};
};

// Function discovery for one loaded module: the base analysis (entry, jal targets,
// materialised and relocated code pointers, prologues) plus exports, runtime
// feedback, jump-table targets and code addresses passed to the system calls in
// code_pointer_apis(), repeated until no new function entry appears.
[[nodiscard]] Discovery discover_functions(const loader::LoadedModule &module, const psprecomp::GuestMemory &memory,
                                           const DiscoveryOptions &options = {});

// Feedback file: one code address per line ("0x08804ABC", anything after it is a
// comment), '#' comment lines. Throws with the line number on anything else.
[[nodiscard]] std::vector<std::uint32_t> read_feedback(const std::filesystem::path &path);

} // namespace anypsp::analysis
