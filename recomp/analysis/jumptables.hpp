// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cfg.hpp"

#include <array>
#include <optional>

namespace anypsp::analysis {

inline constexpr std::size_t kMaxUnboundedTableEntries = 256u;

// Recognises `jr rX` where rX was loaded from table + (index << 2), e.g.
//   sltiu v1, a0, N; beqz v1, default; sll v0, a0, 2; lui v1, %hi(t); addu v0, v0, v1; lw v0, %lo(t)(v0); jr v0
// by evaluating the straight-line code that leads to `jump_pc`. The entry count
// comes from the sltiu bound when one is found; otherwise entries are read until
// one is not a code address.
[[nodiscard]] std::optional<JumpTable> resolve_jump_table(const psprecomp::GuestMemory &memory,
                                                          const std::vector<ExecutableRange> &ranges,
                                                          std::uint32_t jump_pc, std::uint32_t gp);

// Constant register values just before the call at `call_pc` transfers control
// (its delay slot included), from the straight-line code that leads to it.
[[nodiscard]] std::array<std::optional<std::uint32_t>, 32> constants_at_call(const psprecomp::GuestMemory &memory,
                                                                            const std::vector<ExecutableRange> &ranges,
                                                                            std::uint32_t call_pc, std::uint32_t gp);

} // namespace anypsp::analysis
