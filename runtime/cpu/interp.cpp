// SPDX-License-Identifier: GPL-2.0-or-later
//
// Entry points of the reference interpreter. The run loop follows PPSSPP's
// (Core/MIPS/MIPSTables.cpp, RunUntilDowncountZeroFast) and the host rounding
// mode switch PPSSPP's ApplyHostRoundingMode (Core/MIPS/MIPS.cpp),
// GPL-2.0-or-later.

#include "interp.h"

#include "interp_ops.h"
#include "ppsspp_compat.h"

#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <cfenv>
#include <cstdlib>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <limits>
#include <string>

namespace anypsp::cpu {

const float vfpuPrefixConstants[8] = {0.f, 1.f, 2.f, 0.5f, 3.f, 1.f / 3.f, 0.25f, 1.f / 6.f};

// The vcst constants, written as PPSSPP's Core/MIPS/MIPS.cpp computes them.
const float cst_constants[32] = {
    0.0f,
    std::numeric_limits<float>::max(),
    std::sqrt(2.0f),
    std::sqrt(0.5f),
    2.0f / std::sqrt(3.14159265358979323846f),
    2.0f / 3.14159265358979323846f,
    1.0f / 3.14159265358979323846f,
    3.14159265358979323846f / 4,
    3.14159265358979323846f / 2,
    3.14159265358979323846f,
    2.71828182845904523536f,
    1.44269504088896340736f,
    0.434294481903251827651f,
    0.693147180559945309417f,
    2.30258509299404568402f,
    2 * 3.14159265358979323846f,
    3.14159265358979323846f / 6,
    std::log10(2.0f),
    std::log(10.0f) / std::log(2.0f),
    std::sqrt(3.0f) / 2.0f,
};

u32 g_llBit = 0;

namespace {

// ANYPSP_CPU_PROFILE=<file>: at exit, write how often generated code fell back to the
// interpreter, per instruction name (execute) and per entry address (interpret).
struct Profile {
    std::string path;
    std::unordered_map<const char *, std::uint64_t> executed;
    std::unordered_map<std::uint32_t, std::pair<std::uint64_t, std::uint64_t>> interpreted; // entries, steps

    Profile() {
        if (const char *p = std::getenv("ANYPSP_CPU_PROFILE"); p != nullptr && *p != '\0') path = p;
    }
    ~Profile() {
        if (path.empty()) return;
        std::ofstream out(path, std::ios::binary);
        if (!out) return;
        std::vector<std::pair<const char *, std::uint64_t>> ops(executed.begin(), executed.end());
        std::sort(ops.begin(), ops.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        std::uint64_t total = 0;
        for (const auto &[name, count] : ops) total += count;
        out << "execute " << total << "\n";
        for (const auto &[name, count] : ops) out << "op " << name << " " << count << "\n";
        std::vector<std::pair<std::uint32_t, std::pair<std::uint64_t, std::uint64_t>>> pcs(interpreted.begin(),
                                                                                           interpreted.end());
        std::sort(pcs.begin(), pcs.end(), [](const auto &a, const auto &b) { return a.second.second > b.second.second; });
        std::uint64_t steps = 0;
        for (const auto &entry : pcs) steps += entry.second.second;
        out << "interpret " << steps << "\n";
        for (const auto &[pc, counts] : pcs)
            out << "pc " << psprecomp::hex32(pc) << " " << counts.first << " " << counts.second << "\n";
    }
};

Profile g_profile;
const bool g_profiling = !g_profile.path.empty();

} // namespace

MIPSState::MIPSState(psprecomp::Runtime *runtime, psprecomp::AllegrexContext &context,
                     psprecomp::GuestMemory &memory) noexcept
    : ctx(context), rt(runtime), spaceStorage{memory}, space(&spaceStorage), r(context.gpr.data()),
      f(context.fpr.data()), fi(reinterpret_cast<u32 *>(context.fpr.data())),
      fs(reinterpret_cast<s32 *>(context.fpr.data())), v(context.vfpu.data()),
      vi(reinterpret_cast<u32 *>(context.vfpu.data())), vfpuCtrl(context.vfpu_ctrl.data()), hi(context.hi),
      lo(context.lo), fcr31(context.fcr31) {}

float MemorySpace::ReadUnchecked_Float(u32 address) const { return std::bit_cast<float>(mem.aot_load32(address)); }
void MemorySpace::WriteUnchecked_Float(float value, u32 address) { mem.aot_store32(address, std::bit_cast<u32>(value)); }

void Core_MemoryException(MIPSState *mips, u32 address, int size, const char *what) {
    static int reported = 0;
    if (reported < 16) {
        ++reported;
        std::cerr << "[cpu] bad " << size << "-byte " << what << " access at " << psprecomp::hex32(address)
                  << " (pc " << psprecomp::hex32(mips->pc) << ")" << (reported == 16 ? "; further ones not shown" : "")
                  << "\n";
    }
}

bool InterruptsEnabled(MIPSState *mips) { return mips->rt == nullptr || mips->rt->interrupts_enabled(); }

void SetInterruptsEnabled(MIPSState *mips, bool enabled) {
    if (mips->rt != nullptr) mips->rt->set_interrupts_enabled(enabled);
}

namespace {

bool g_hostModeSaved = false;
#if defined(ANYPSP_ARCH_SSE2)
u32 g_savedHostMode = 0;
#else
int g_savedHostRound = FE_TONEAREST;
#endif

void apply_mode(u32 fcr31) {
    const u32 bits = fcr31 & 0x01000003u;
    if (bits == 0u) {
        RestoreHostRoundingMode();
        return;
    }
    int rmode = static_cast<int>(bits & 3u);
#if defined(ANYPSP_ARCH_SSE2)
    if (!g_hostModeSaved) {
        g_savedHostMode = _mm_getcsr() & ~0xE040u;
        g_hostModeSaved = true;
    }
    u32 csr = g_savedHostMode;
    // MIPS: nearest, zero, +inf, -inf. MXCSR: nearest, -inf, +inf, zero.
    if (rmode & 1) rmode ^= 2;
    csr |= static_cast<u32>(rmode) << 13;
    if ((bits & 0x01000000u) != 0u) csr |= 0x8040u;  // FTZ and DAZ
    _mm_setcsr(csr);
#else
    // No portable flush-to-zero; only the rounding mode follows the guest here.
    static const int lookup[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
    if (!g_hostModeSaved) {
        g_savedHostRound = std::fegetround();
        g_hostModeSaved = true;
    }
    std::fesetround(lookup[rmode]);
#endif
}

std::string fault_message(Fault fault, u32 op) {
    switch (fault) {
    case Fault::Break: return "break";
    case Fault::Syscall: return "syscall without an HLE binding";
    case Fault::Trap: return "trap";
    case Fault::Illegal:
    case Fault::None: break;
    }
    return std::string("illegal or unimplemented instruction (") + MIPSGetName(op) + ")";
}

} // namespace

void ApplyHostRoundingMode(const MIPSState *mips) { apply_mode(mips->fcr31); }

void RestoreHostRoundingMode() {
    if (!g_hostModeSaved) return;
    g_hostModeSaved = false;
#if defined(ANYPSP_ARCH_SSE2)
    _mm_setcsr(g_savedHostMode);
#else
    std::fesetround(g_savedHostRound);
#endif
}

void enter_guest(const psprecomp::AllegrexContext &ctx) { apply_mode(ctx.fcr31); }

void leave_guest() {
    RestoreHostRoundingMode();
    g_llBit = 0;
}

InstructionInfo instruction_info(std::uint32_t op) noexcept {
    const MIPSInfo info = MIPSGetInfo(op);
    const bool valid = (info & BAD_INSTRUCTION) == 0 && MIPSGetInterpretFunc(op) != nullptr;
    return InstructionInfo{
        MIPSGetName(op),
        valid,
        (info & (IS_JUMP | IS_CONDBRANCH | IS_SYSCALL | ENDS_BLOCK)) != 0,
        (info & DELAYSLOT) != 0,
        (info & LIKELY) != 0,
    };
}

bool execute(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx, std::uint32_t pc, std::uint32_t op) {
    MIPSState mips(&rt, ctx, rt.memory());
    mips.pc = pc;
    const MIPSInfo info = MIPSGetInfo(op);
    const MIPSInterpretFunc function = MIPSGetInterpretFunc(op);
    if (function == nullptr || (info & (IS_JUMP | IS_CONDBRANCH | IS_SYSCALL | ENDS_BLOCK)) != 0) {
        rt.unsupported(pc, op, function == nullptr ? fault_message(Fault::Illegal, op)
                                                   : std::string(MIPSGetName(op)) + " outside the translated control flow");
        return false;
    }
    if (g_profiling) ++g_profile.executed[MIPSGetName(op)];
    function(&mips, op);
    if (mips.fault != Fault::None) {
        rt.unsupported(pc, op, fault_message(mips.fault, op));
        return false;
    }
    return true;
}

void interpret(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx, std::uint32_t budget) {
    auto &memory = rt.memory();
    MIPSState mips(&rt, ctx, memory);
    mips.pc = ctx.pc;
    apply_mode(ctx.fcr31);
    u32 branch_op = 0;
    const u32 entry = ctx.pc;
    std::uint32_t step = 0;
    for (;; ++step) {
        const u32 pc = mips.pc;
        if (!mips.inDelaySlot) {
            if (step != 0u && (rt.has_function(pc) || step >= budget || rt.stopped())) break;
        }
        if (!mips.space->IsValid4AlignedAddress(pc)) {
            rt.stop("No recompiled function registered at " + psprecomp::hex32(pc));
            break;
        }
        const u32 op = memory.load32(pc);
        const bool was_in_delay_slot = mips.inDelaySlot;
        const MIPSInterpretFunc function = MIPSGetInterpretFunc(op);
        if (function == nullptr) mips.raise(Fault::Illegal);
        else function(&mips, op);
        if (mips.fault != Fault::None) {
            rt.unsupported(mips.faultPC, op, fault_message(mips.fault, op));
            break;
        }
        if (mips.inDelaySlot && !was_in_delay_slot) branch_op = op;
        if (mips.inDelaySlot && was_in_delay_slot) {
            mips.pc = mips.nextPC;
            mips.inDelaySlot = false;
            if ((MIPSGetInfo(branch_op) & IS_JUMP) != 0 && !rt.has_function(mips.pc))
                psprecomp::record_missing_code(mips.pc);
        }
    }
    ctx.pc = mips.pc;
    if (g_profiling) {
        auto &counts = g_profile.interpreted[entry];
        ++counts.first;
        counts.second += step;
    }
}

} // namespace anypsp::cpu
