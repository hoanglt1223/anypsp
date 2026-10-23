// SPDX-License-Identifier: GPL-2.0-or-later
//
// The machine state seen by the Allegrex interpreter in this directory, which
// is adapted from PPSSPP (Core/MIPS/Interpreter*.cpp, MIPSVFPUUtils.cpp,
// MIPSTables.cpp, MIPS.h; GPL-2.0-or-later). MIPSState keeps PPSSPP's member
// names but points into psprecomp::AllegrexContext and GuestMemory, so the
// adapted code reads like the original.

#pragma once

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"

#include <array>
#include <cstdint>

namespace psprecomp {
class Runtime;
}

namespace anypsp::cpu {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using MIPSOpcode = u32;

enum {
    VFPU_CTRL_SPREFIX,
    VFPU_CTRL_TPREFIX,
    VFPU_CTRL_DPREFIX,
    VFPU_CTRL_CC,
    VFPU_CTRL_INF4,
    VFPU_CTRL_RSV5,
    VFPU_CTRL_RSV6,
    VFPU_CTRL_REV,
    VFPU_CTRL_RCX0,
    VFPU_CTRL_RCX1,
    VFPU_CTRL_RCX2,
    VFPU_CTRL_RCX3,
    VFPU_CTRL_RCX4,
    VFPU_CTRL_RCX5,
    VFPU_CTRL_RCX6,
    VFPU_CTRL_RCX7,
    VFPU_CTRL_MAX,
};

enum VCondition {
    VC_FL, VC_EQ, VC_LT, VC_LE, VC_TR, VC_NE, VC_GE, VC_GT,
    VC_EZ, VC_EN, VC_EI, VC_ES, VC_NZ, VC_NN, VC_NI, VC_NS,
};

enum {
    FCR31_FLAG_DIV_BY_ZERO = 1 << 5,
    FCR31_ENABLE_DIV_BY_ZERO = 1 << 10,
    FCR31_CAUSE_DIV_BY_ZERO = 1 << 15,
};

enum { MIPS_REG_RA = 31 };

inline int VFPUPrefixSwizzle(u32 prefix, int i) { return (prefix >> (i * 2)) & 3; }
inline bool VFPUPrefixAbs(u32 prefix, int i) { return (prefix >> (8 + i)) & 1; }
inline bool VFPUPrefixIsConstant(u32 prefix, int i) { return (prefix >> (12 + i)) & 1; }
inline bool VFPUPrefixNegate(u32 prefix, int i) { return (prefix >> (16 + i)) & 1; }
inline int VFPUPrefixSat(u32 prefix, int i) { return (prefix >> (i * 2)) & 3; }

extern const float vfpuPrefixConstants[8];
extern const float cst_constants[32];

// PPSSPP numbers VFPU registers mtx*4 + column + row*32; AllegrexContext keeps
// them as matrix*16 + column*4 + row.
inline constexpr std::array<u8, 128> voffset = [] {
    std::array<u8, 128> table{};
    for (u32 reg = 0; reg < 128u; ++reg)
        table[reg] = static_cast<u8>(psprecomp::AllegrexContext::vfpu_scalar_index(reg));
    return table;
}();

// Address checks and unchecked accesses with PPSSPP's names, over GuestMemory.
struct MemorySpace {
    psprecomp::GuestMemory &mem;

    bool IsValidAddress(u32 address) const { return mem.contains(address, 1u); }
    bool IsValid2AlignedAddress(u32 address) const { return (address & 1u) == 0u && mem.contains(address, 2u); }
    bool IsValid4AlignedAddress(u32 address) const { return (address & 3u) == 0u && mem.contains(address, 4u); }
    u8 ReadUnchecked_U8(u32 address) const { return mem.aot_load8(address); }
    u16 ReadUnchecked_U16(u32 address) const { return mem.aot_load16(address); }
    u32 ReadUnchecked_U32(u32 address) const { return mem.aot_load32(address); }
    float ReadUnchecked_Float(u32 address) const;
    void WriteUnchecked_U8(u8 value, u32 address) { mem.aot_store8(address, value); }
    void WriteUnchecked_U16(u16 value, u32 address) { mem.aot_store16(address, value); }
    void WriteUnchecked_U32(u32 value, u32 address) { mem.aot_store32(address, value); }
    void WriteUnchecked_Float(float value, u32 address);
};

// Why execution must stop after an instruction (break, an illegal or
// unimplemented encoding, an access outside guest memory when strict).
enum class Fault { None, Break, Illegal, Syscall, Trap };

struct MIPSState {
    MIPSState(psprecomp::Runtime *runtime, psprecomp::AllegrexContext &context, psprecomp::GuestMemory &memory) noexcept;

    psprecomp::AllegrexContext &ctx;
    psprecomp::Runtime *rt;
    MemorySpace spaceStorage;
    MemorySpace *space;

    u32 *r;
    float *f;
    u32 *fi;
    s32 *fs;
    float *v;
    u32 *vi;
    u32 *vfpuCtrl;
    u32 &hi;
    u32 &lo;
    u32 &fcr31;

    u32 pc{};
    u32 nextPC{};
    bool inDelaySlot{};

    Fault fault{Fault::None};
    u32 faultPC{};

    u8 VfpuWriteMask() const { return static_cast<u8>((vfpuCtrl[VFPU_CTRL_DPREFIX] >> 8) & 0xF); }
    bool VfpuWriteMask(int i) const { return ((vfpuCtrl[VFPU_CTRL_DPREFIX] >> (8 + i)) & 1) != 0; }

    void raise(Fault kind) noexcept {
        if (fault == Fault::None) {
            fault = kind;
            faultPC = pc;
        }
    }
};

// Load-linked flag. There is one CPU, so one flag; eret and thread switches clear it.
extern u32 g_llBit;

// Bad guest accesses follow PPSSPP's default: loads read zero, stores are
// dropped, execution continues. The first few are reported on stderr.
void Core_MemoryException(MIPSState *mips, u32 address, int size, const char *what);

} // namespace anypsp::cpu
