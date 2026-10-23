// SPDX-License-Identifier: GPL-2.0-or-later

#include "cfg.hpp"

#include "jumptables.hpp"

#include "psprecomp/decoder.hpp"

#include <algorithm>
#include <deque>

namespace anypsp::analysis {
namespace {

using psprecomp::DecodedInstruction;
using psprecomp::OpcodeKind;
using psprecomp::is_executable_address;

bool is_conditional(OpcodeKind kind) {
    switch (kind) {
    case OpcodeKind::Beq: case OpcodeKind::Bne: case OpcodeKind::Beql: case OpcodeKind::Bnel:
    case OpcodeKind::Blez: case OpcodeKind::Bgtz: case OpcodeKind::Blezl: case OpcodeKind::Bgtzl:
    case OpcodeKind::Bltz: case OpcodeKind::Bgez: case OpcodeKind::Bltzl: case OpcodeKind::Bgezl:
    case OpcodeKind::Bc1f: case OpcodeKind::Bc1t: case OpcodeKind::Bc1fl: case OpcodeKind::Bc1tl:
    case OpcodeKind::Bvf: case OpcodeKind::Bvt: case OpcodeKind::Bvfl: case OpcodeKind::Bvtl:
        return true;
    default:
        return false;
    }
}

bool is_branch_and_link(OpcodeKind kind) {
    return kind == OpcodeKind::Bltzal || kind == OpcodeKind::Bgezal || kind == OpcodeKind::Bltzall ||
           kind == OpcodeKind::Bgezall;
}

bool is_likely(OpcodeKind kind) {
    switch (kind) {
    case OpcodeKind::Beql: case OpcodeKind::Bnel: case OpcodeKind::Blezl: case OpcodeKind::Bgtzl:
    case OpcodeKind::Bltzl: case OpcodeKind::Bgezl: case OpcodeKind::Bltzall: case OpcodeKind::Bgezall:
    case OpcodeKind::Bc1fl: case OpcodeKind::Bc1tl: case OpcodeKind::Bvfl: case OpcodeKind::Bvtl:
        return true;
    default:
        return false;
    }
}

std::uint32_t branch_target(std::uint32_t pc, const DecodedInstruction &d) {
    return pc + 4u + static_cast<std::uint32_t>(static_cast<std::int32_t>(d.immediate) * 4);
}

std::uint32_t jump_target(std::uint32_t pc, const DecodedInstruction &d) {
    return ((pc + 4u) & 0xF0000000u) | (d.target << 2u);
}

class Builder {
public:
    Builder(const psprecomp::GuestMemory &memory, const std::vector<ExecutableRange> &ranges, std::uint32_t entry,
            const CfgOptions &options)
        : memory_(memory), ranges_(ranges), options_(options) {
        cfg_.entry = entry;
    }

    FunctionCfg build() {
        if (!is_executable_address(ranges_, cfg_.entry)) return std::move(cfg_);
        add_leader(cfg_.entry);
        while (!pending_.empty() && !cfg_.truncated) {
            const std::uint32_t leader = pending_.front();
            pending_.pop_front();
            if (!heads_.contains(leader)) explore(leader);
        }
        for (const auto leader : leaders_) {
            if (heads_.contains(leader - 4u) && decode(leader - 4u).has_delay_slot()) cfg_.delay_slot_targets.insert(leader);
            form_block(leader);
        }
        return std::move(cfg_);
    }

private:
    DecodedInstruction decode(std::uint32_t pc) const { return psprecomp::decode_allegrex(memory_.load32(pc)); }

    void add_leader(std::uint32_t address) {
        if (is_executable_address(ranges_, address) && leaders_.insert(address).second) pending_.push_back(address);
    }

    bool is_other_function(std::uint32_t target) const {
        return target != cfg_.entry && options_.function_entries != nullptr && options_.function_entries->contains(target);
    }

    void explore(std::uint32_t leader) {
        for (std::uint32_t pc = leader; is_executable_address(ranges_, pc); pc += 4u) {
            if (pc != leader && (leaders_.contains(pc) || heads_.contains(pc))) return;
            if (++instructions_ > options_.max_instructions) {
                cfg_.truncated = true;
                return;
            }
            heads_.insert(pc);
            const auto d = decode(pc);
            if (!d.has_delay_slot()) continue;

            const std::uint32_t slot = pc + 4u;
            if (is_executable_address(ranges_, slot)) {
                const auto slot_instruction = decode(slot);
                if (slot_instruction.is_control_flow() && slot_instruction.kind != OpcodeKind::Syscall)
                    cfg_.branches_in_delay_slots.push_back(slot);
            }
            if (is_conditional(d.kind)) {
                add_leader(branch_target(pc, d));
                add_leader(pc + 8u);
            } else if (is_branch_and_link(d.kind)) {
                const std::uint32_t target = branch_target(pc, d);
                if (is_executable_address(ranges_, target)) cfg_.calls.insert(target);
                cfg_.call_sites.push_back(pc);
                add_leader(pc + 8u);
            } else if (d.kind == OpcodeKind::J) {
                const std::uint32_t target = jump_target(pc, d);
                if (!is_other_function(target)) add_leader(target);
            } else if (d.kind == OpcodeKind::Jal) {
                const std::uint32_t target = jump_target(pc, d);
                if (is_executable_address(ranges_, target)) cfg_.calls.insert(target);
                cfg_.call_sites.push_back(pc);
                add_leader(pc + 8u);
            } else if (d.kind == OpcodeKind::Jalr) {
                add_leader(pc + 8u);
            } else if (d.kind == OpcodeKind::Jr && d.rs != 31u) {
                if (auto table = resolve_jump_table(memory_, ranges_, pc, options_.gp)) {
                    for (const auto target : table->targets) add_leader(target);
                    cfg_.jump_tables.push_back(std::move(*table));
                } else {
                    cfg_.unresolved_indirect_jumps.push_back(pc);
                }
            }
            return;
        }
    }

    void form_block(std::uint32_t leader) {
        Block block;
        block.start = leader;
        std::uint32_t pc = leader;
        while (true) {
            if (!is_executable_address(ranges_, pc)) {
                block.exit = Exit::End;
                block.end = pc;
                break;
            }
            const auto d = decode(pc);
            if (d.has_delay_slot()) {
                block.branch_pc = pc;
                block.end = pc + 8u;
                classify(block, d, pc);
                break;
            }
            pc += 4u;
            if (leaders_.contains(pc)) {
                block.exit = Exit::Fallthrough;
                block.end = pc;
                block.successors.push_back(pc);
                break;
            }
        }
        std::sort(block.successors.begin(), block.successors.end());
        block.successors.erase(std::unique(block.successors.begin(), block.successors.end()), block.successors.end());
        cfg_.blocks.emplace(leader, std::move(block));
    }

    void successor(Block &block, std::uint32_t address) const {
        if (leaders_.contains(address)) block.successors.push_back(address);
    }

    void classify(Block &block, const DecodedInstruction &d, std::uint32_t pc) {
        if (is_conditional(d.kind)) {
            block.exit = Exit::Branch;
            block.likely = is_likely(d.kind);
            block.target = branch_target(pc, d);
            successor(block, block.target);
            successor(block, pc + 8u);
        } else if (is_branch_and_link(d.kind)) {
            block.exit = Exit::Call;
            block.likely = is_likely(d.kind);
            block.target = branch_target(pc, d);
            successor(block, pc + 8u);
        } else if (d.kind == OpcodeKind::J) {
            block.target = jump_target(pc, d);
            block.exit = is_other_function(block.target) ? Exit::TailCall : Exit::Jump;
            if (block.exit == Exit::Jump) successor(block, block.target);
        } else if (d.kind == OpcodeKind::Jal) {
            block.exit = Exit::Call;
            block.target = jump_target(pc, d);
            successor(block, pc + 8u);
        } else if (d.kind == OpcodeKind::Jalr) {
            block.exit = Exit::IndirectCall;
            successor(block, pc + 8u);
        } else if (d.rs == 31u) {
            block.exit = Exit::Return;
        } else {
            block.exit = Exit::IndirectJump;
            for (const auto &table : cfg_.jump_tables)
                if (table.jump_pc == pc)
                    for (const auto target : table.targets) successor(block, target);
        }
    }

    const psprecomp::GuestMemory &memory_;
    const std::vector<ExecutableRange> &ranges_;
    const CfgOptions &options_;
    FunctionCfg cfg_;
    std::set<std::uint32_t> leaders_;
    std::set<std::uint32_t> heads_; // words decoded as instructions in their own right (not as delay slots)
    std::deque<std::uint32_t> pending_;
    std::size_t instructions_{};
};

} // namespace

FunctionCfg build_cfg(const psprecomp::GuestMemory &memory, const std::vector<ExecutableRange> &ranges,
                      std::uint32_t entry, const CfgOptions &options) {
    return Builder(memory, ranges, entry, options).build();
}

} // namespace anypsp::analysis
