// SPDX-License-Identifier: GPL-2.0-or-later
//
// C++ emission for single Allegrex instructions, shared by psp_recomp and the
// differential tests that compile its output.

#pragma once

#include "psprecomp/decoder.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace anypsp::codegen {

// `$zero` as a literal, any other GPR as a context access.
std::string reg(std::uint32_t index);

// Inline C++ for the instruction kinds the generator translates itself.
std::string emit_regular(const psprecomp::DecodedInstruction &d, std::uint32_t pc);
std::optional<std::string> emit_extra_vfpu(const psprecomp::DecodedInstruction &d);

// C++ for one instruction that does not transfer control: inline code where
// the inline translation is known to match the reference interpreter, a call
// into it otherwise. Ends with `return;` on paths that stop the guest.
std::string emit_instruction(const psprecomp::DecodedInstruction &d, std::uint32_t pc);

// Whether emit_instruction translates this instruction inline.
bool translated_inline(const psprecomp::DecodedInstruction &d);

bool is_branch(psprecomp::OpcodeKind kind);
bool is_likely_branch(psprecomp::OpcodeKind kind);
bool is_link_branch(psprecomp::OpcodeKind kind);
std::string branch_condition(const psprecomp::DecodedInstruction &d);

// The source passes psp_recomp applies to generated units before writing them.
std::string lower_generated_source(std::string text);

} // namespace anypsp::codegen
