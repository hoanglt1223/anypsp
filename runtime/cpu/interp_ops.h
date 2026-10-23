// SPDX-License-Identifier: GPL-2.0-or-later
//
// Internal declarations shared by the interpreter sources adapted from PPSSPP
// (Core/MIPS/Interpreter.h, InterpreterVFPU.h, MIPSTables.h; GPL-2.0-or-later).

#pragma once

#include "mips_state.h"

namespace anypsp::cpu {

#define CONDTYPE_MASK   0x00000007
#define CONDTYPE_EQ     0x00000001
#define CONDTYPE_NE     0x00000002
#define CONDTYPE_LEZ    0x00000003
#define CONDTYPE_GTZ    0x00000004
#define CONDTYPE_LTZ    0x00000005
#define CONDTYPE_GEZ    0x00000006

#define CONDTYPE_FPUFALSE   CONDTYPE_EQ
#define CONDTYPE_FPUTRUE    CONDTYPE_NE

// as long as the other flags are checked,
// there is no way to misinterpret these
// as CONDTYPE_X
#define MEMTYPE_MASK    0x00000007ULL
#define MEMTYPE_BYTE    0x00000001ULL
#define MEMTYPE_HWORD   0x00000002ULL
#define MEMTYPE_WORD    0x00000003ULL
#define MEMTYPE_FLOAT   0x00000004ULL
#define MEMTYPE_VQUAD   0x00000005ULL

#define IS_CONDMOVE     0x00000008ULL
#define DELAYSLOT       0x00000010ULL
#define BAD_INSTRUCTION 0x00000020ULL
#define LIKELY          0x00000040ULL
#define IS_CONDBRANCH   0x00000080ULL
#define IS_JUMP         0x00000100ULL

#define IN_RS           0x00000200ULL
#define IN_RS_ADDR      (0x00000400ULL | IN_RS)
#define IN_RS_SHIFT     (0x00000800ULL | IN_RS)
#define IN_RT           0x00001000ULL
#define IN_SA           0x00002000ULL
#define IN_IMM16        0x00004000ULL
#define IN_IMM26        0x00008000ULL
#define IN_MEM          0x00010000ULL
#define IN_OTHER        0x00020000ULL
#define IN_FPUFLAG      0x00040000ULL
#define IN_VFPU_CC      0x00080000ULL

#define OUT_RT          0x00100000ULL
#define OUT_RD          0x00200000ULL
#define OUT_RA          0x00400000ULL
#define OUT_MEM         0x00800000ULL
#define OUT_OTHER       0x01000000ULL
#define OUT_FPUFLAG     0x02000000ULL
#define OUT_VFPU_CC     0x04000000ULL
#define OUT_EAT_PREFIX  0x08000000ULL

#define VFPU_NO_PREFIX  0x10000000ULL
#define OUT_VFPU_PREFIX 0x20000000ULL
#define IS_VFPU         0x40000000ULL
#define IS_FPU          0x80000000ULL

#define IN_FS           0x000100000000ULL
#define IN_FT           0x000200000000ULL
#define IN_LO           0x000400000000ULL
#define IN_HI           0x000800000000ULL

#define OUT_FD          0x001000000000ULL
#define OUT_FS          0x002000000000ULL
#define OUT_LO          0x004000000000ULL
#define OUT_HI          0x008000000000ULL

#define IN_VS           0x010000000000ULL
#define IN_VT           0x020000000000ULL
#define OUT_FT          0x040000000000ULL

#define OUT_VD          0x100000000000ULL

#define IS_SYSCALL      0x200000000000ULL
// Moves the PC somewhere the compiler can't see (eret), so the block ends after it.
#define ENDS_BLOCK      0x400000000000ULL

struct MIPSInfo {
	MIPSInfo() : value(0), cycles(0) {}

	explicit MIPSInfo(u64 v, u16 c = 0) : value(v), cycles(c) {
		if (c == 0) {
			cycles = 1;
			if (v & IS_VFPU)
				cycles++;
		}
	}

	u64 operator&(const u64 &arg) const { return value & arg; }

	u64 value : 48;
	u64 cycles : 16;
};

using MIPSInterpretFunc = void (*)(MIPSState *mips, MIPSOpcode opcode);

MIPSInfo MIPSGetInfo(MIPSOpcode op);
const char *MIPSGetName(MIPSOpcode op);
MIPSInterpretFunc MIPSGetInterpretFunc(MIPSOpcode op);

inline constexpr u32 FCR0_VALUE = 0x00003351;

// Puts the host FPU into the guest's rounding and flush-to-zero mode (fcr31
// bits 0-1 and 24), or back to the host default when those are clear.
void ApplyHostRoundingMode(const MIPSState *mips);
void RestoreHostRoundingMode();

bool InterruptsEnabled(MIPSState *mips);
void SetInterruptsEnabled(MIPSState *mips, bool enabled);

void EatPrefixes(MIPSState *mips);

namespace MIPSInt {
	void Int_Allegrex(MIPSState *mips, MIPSOpcode op);
	void Int_Allegrex2(MIPSState *mips, MIPSOpcode op);
	void Int_Break(MIPSState *mips, MIPSOpcode op);
	void Int_Cache(MIPSState *mips, MIPSOpcode op);
	void Int_ColorConv(MIPSState *mips, MIPSOpcode op);
	void Int_Cop0(MIPSState *mips, MIPSOpcode op);
	void Int_CrossQuat(MIPSState *mips, MIPSOpcode op);
	void Int_Eret(MIPSState *mips, MIPSOpcode op);
	void Int_FPU2op(MIPSState *mips, MIPSOpcode op);
	void Int_FPU3op(MIPSState *mips, MIPSOpcode op);
	void Int_FPUBranch(MIPSState *mips, MIPSOpcode op);
	void Int_FPUComp(MIPSState *mips, MIPSOpcode op);
	void Int_FPULS(MIPSState *mips, MIPSOpcode op);
	void Int_Halt(MIPSState *mips, MIPSOpcode op);
	void Int_IType(MIPSState *mips, MIPSOpcode op);
	void Int_ITypeMem(MIPSState *mips, MIPSOpcode op);
	void Int_Interrupt(MIPSState *mips, MIPSOpcode op);
	void Int_JumpRegType(MIPSState *mips, MIPSOpcode op);
	void Int_JumpType(MIPSState *mips, MIPSOpcode op);
	void Int_Me2C(MIPSState *mips, MIPSOpcode op);
	void Int_Mftv(MIPSState *mips, MIPSOpcode op);
	void Int_MulDivType(MIPSState *mips, MIPSOpcode op);
	void Int_RType2(MIPSState *mips, MIPSOpcode op);
	void Int_RType3(MIPSState *mips, MIPSOpcode op);
	void Int_RelBranch(MIPSState *mips, MIPSOpcode op);
	void Int_RelBranchRI(MIPSState *mips, MIPSOpcode op);
	void Int_SV(MIPSState *mips, MIPSOpcode op);
	void Int_SVQ(MIPSState *mips, MIPSOpcode op);
	void Int_ShiftType(MIPSState *mips, MIPSOpcode op);
	void Int_Special2(MIPSState *mips, MIPSOpcode op);
	void Int_Special3(MIPSState *mips, MIPSOpcode op);
	void Int_StoreSync(MIPSState *mips, MIPSOpcode op);
	void Int_Sync(MIPSState *mips, MIPSOpcode op);
	void Int_Syscall(MIPSState *mips, MIPSOpcode op);
	void Int_VBranch(MIPSState *mips, MIPSOpcode op);
	void Int_VDot(MIPSState *mips, MIPSOpcode op);
	void Int_VHdp(MIPSState *mips, MIPSOpcode op);
	void Int_VMatrixInit(MIPSState *mips, MIPSOpcode op);
	void Int_VPFX(MIPSState *mips, MIPSOpcode op);
	void Int_VScl(MIPSState *mips, MIPSOpcode op);
	void Int_VV2Op(MIPSState *mips, MIPSOpcode op);
	void Int_VVectorInit(MIPSState *mips, MIPSOpcode op);
	void Int_Vavg(MIPSState *mips, MIPSOpcode op);
	void Int_Vbfy(MIPSState *mips, MIPSOpcode op);
	void Int_Vcmov(MIPSState *mips, MIPSOpcode op);
	void Int_Vcmp(MIPSState *mips, MIPSOpcode op);
	void Int_Vcrs(MIPSState *mips, MIPSOpcode op);
	void Int_Vcst(MIPSState *mips, MIPSOpcode op);
	void Int_Vdet(MIPSState *mips, MIPSOpcode op);
	void Int_VecDo3(MIPSState *mips, MIPSOpcode op);
	void Int_Vf2h(MIPSState *mips, MIPSOpcode op);
	void Int_Vf2i(MIPSState *mips, MIPSOpcode op);
	void Int_Vfad(MIPSState *mips, MIPSOpcode op);
	void Int_Vflush(MIPSState *mips, MIPSOpcode op);
	void Int_Vh2f(MIPSState *mips, MIPSOpcode op);
	void Int_Vi2f(MIPSState *mips, MIPSOpcode op);
	void Int_Vi2x(MIPSState *mips, MIPSOpcode op);
	void Int_Vidt(MIPSState *mips, MIPSOpcode op);
	void Int_Viim(MIPSState *mips, MIPSOpcode op);
	void Int_Vlgb(MIPSState *mips, MIPSOpcode op);
	void Int_Vmfvc(MIPSState *mips, MIPSOpcode op);
	void Int_Vminmax(MIPSState *mips, MIPSOpcode op);
	void Int_Vmmov(MIPSState *mips, MIPSOpcode op);
	void Int_Vmmul(MIPSState *mips, MIPSOpcode op);
	void Int_Vmscl(MIPSState *mips, MIPSOpcode op);
	void Int_Vmtvc(MIPSState *mips, MIPSOpcode op);
	void Int_Vocp(MIPSState *mips, MIPSOpcode op);
	void Int_VrndX(MIPSState *mips, MIPSOpcode op);
	void Int_Vrnds(MIPSState *mips, MIPSOpcode op);
	void Int_Vrot(MIPSState *mips, MIPSOpcode op);
	void Int_Vsbn(MIPSState *mips, MIPSOpcode op);
	void Int_Vsbz(MIPSState *mips, MIPSOpcode op);
	void Int_Vscmp(MIPSState *mips, MIPSOpcode op);
	void Int_Vsge(MIPSState *mips, MIPSOpcode op);
	void Int_Vsgn(MIPSState *mips, MIPSOpcode op);
	void Int_Vslt(MIPSState *mips, MIPSOpcode op);
	void Int_Vsocp(MIPSState *mips, MIPSOpcode op);
	void Int_Vsrt1(MIPSState *mips, MIPSOpcode op);
	void Int_Vsrt2(MIPSState *mips, MIPSOpcode op);
	void Int_Vsrt3(MIPSState *mips, MIPSOpcode op);
	void Int_Vsrt4(MIPSState *mips, MIPSOpcode op);
	void Int_Vtfm(MIPSState *mips, MIPSOpcode op);
	void Int_Vwbn(MIPSState *mips, MIPSOpcode op);
	void Int_Vx2i(MIPSState *mips, MIPSOpcode op);
	void Int_mxc1(MIPSState *mips, MIPSOpcode op);
} // namespace MIPSInt

} // namespace anypsp::cpu
