// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "psprecomp/guest_memory.hpp"
#include "psprecomp/program_analysis.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace anypsp::analysis {

using psprecomp::ExecutableRange;

enum class Exit : std::uint8_t {
    Fallthrough,  // runs into the next block (which is a branch target)
    Branch,       // conditional branch: taken target and fall-through
    Jump,         // j inside the function
    TailCall,     // j to another function's entry
    Call,         // jal/bal: continues at the return address
    IndirectCall, // jalr
    Return,       // jr ra
    IndirectJump, // jr through a register other than ra (jump table or tail call)
    End,          // ran out of executable memory or hit an undecodable word
};

struct Block {
    std::uint32_t start{};
    std::uint32_t end{};        // one past the last instruction, delay slot included
    std::uint32_t branch_pc{};  // the control instruction, when `exit` is not Fallthrough/End
    Exit exit{Exit::End};
    bool likely{};              // branch-likely: the delay slot runs on the taken edge only
    std::uint32_t target{};     // Branch/Jump/TailCall/Call target
    std::vector<std::uint32_t> successors; // blocks of this function, in address order
};

struct JumpTable {
    std::uint32_t jump_pc{};
    std::uint32_t table{};
    std::vector<std::uint32_t> targets;
    bool bounded{}; // the entry count came from an sltiu bound check
};

struct FunctionCfg {
    std::uint32_t entry{};
    std::map<std::uint32_t, Block> blocks;
    std::set<std::uint32_t> calls;           // direct call targets
    std::vector<std::uint32_t> call_sites;   // jal/bal addresses
    std::vector<std::uint32_t> unresolved_indirect_jumps;
    std::vector<JumpTable> jump_tables;
    std::set<std::uint32_t> delay_slot_targets;     // branch targets that are another branch's delay slot
    std::vector<std::uint32_t> branches_in_delay_slots; // undefined behaviour on Allegrex; reported
    bool truncated{};
};

struct CfgOptions {
    const std::set<std::uint32_t> *function_entries{}; // j to one of these is a tail call
    std::uint32_t gp{};                                 // module gp, for jump-table address arithmetic
    std::size_t max_instructions{131072u};
};

// Builds the control-flow graph of the function at `entry`, resolving jump tables
// so their targets become blocks of the function.
[[nodiscard]] FunctionCfg build_cfg(const psprecomp::GuestMemory &memory, const std::vector<ExecutableRange> &ranges,
                                    std::uint32_t entry, const CfgOptions &options = {});

} // namespace anypsp::analysis
