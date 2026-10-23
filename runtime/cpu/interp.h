// SPDX-License-Identifier: GPL-2.0-or-later
//
// The reference Allegrex interpreter: integer, FPU and VFPU semantics adapted
// from PPSSPP's interpreter. Generated code calls execute() for instructions it
// does not translate inline, and the runtime falls back to interpret() for
// code that has no translation.

#pragma once

#include "psprecomp/allegrex_context.hpp"

#include <cstdint>

namespace psprecomp {
class Runtime;
}

namespace anypsp::cpu {

struct InstructionInfo {
    const char *name;
    bool valid;          // decodes to an instruction the interpreter implements
    bool control_flow;   // branch, jump, syscall or eret: execute() does not take these
    bool delay_slot;
    bool likely;
};

[[nodiscard]] InstructionInfo instruction_info(std::uint32_t op) noexcept;

// Runs one instruction that does not transfer control, at `pc`. On a fault
// (break, an illegal encoding, a control-flow instruction) it stops the
// runtime with a message and returns false.
bool execute(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx, std::uint32_t pc, std::uint32_t op);

// Interprets from ctx.pc, delay slots included, until control reaches a
// registered (translated or HLE) function, the runtime stops, or about
// `budget` instructions have run. Jumps to code without a translation are
// recorded as runtime feedback.
void interpret(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx, std::uint32_t budget);

// Guest code runs with the host FPU in the guest's rounding and flush-to-zero
// mode. Host code (HLE functions, the kernel between time slices) runs after
// leave_guest(), which restores the host mode and, like a syscall or an
// interrupt on the PSP, clears the ll/sc link. Both are cheap in the default mode.
void enter_guest(const psprecomp::AllegrexContext &ctx);
void leave_guest();

} // namespace anypsp::cpu
