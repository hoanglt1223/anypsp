// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

// Adapted for AnyPSP from PPSSPP Core/MIPS/Interpreter.cpp at 902e5ccd8c14:
// namespace anypsp::cpu, PPSSPP includes replaced by ppsspp_compat.h, memory
// through GuestMemory (MIPSState::space), host-side subsystems (MMIO, the Media
// Engine, the JIT, replacement functions, reporting) removed.

#include "interp_ops.h"
#include "ppsspp_compat.h"
#include "vfpu_utils.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace anypsp::cpu {

#define R(i) (mips->r[i])
#define F(i) (mips->f[i])
#define FI(i) (mips->fi[i])
#define FsI(i) (mips->fs[i])
#define PC (mips->pc)

#define _SIMM16_SHL2 ((u32)(s32)(s16)(op & 0xFFFF) << 2)
#define _RS   ((op>>21) & 0x1F)
#define _RT   ((op>>16) & 0x1F)
#define _RD   ((op>>11) & 0x1F)
#define _FS   ((op>>11) & 0x1F)
#define _FT   ((op>>16) & 0x1F)
#define _FD   ((op>>6 ) & 0x1F)
#define _POS  ((op>>6 ) & 0x1F)
#define _SIZE ((op>>11) & 0x1F)

#define FPCOND ((mips->fcr31 >> 23) & 1)
#define HI mips->hi
#define LO mips->lo

static inline void DelayBranchTo(MIPSState *mips, u32 where) {
	PC += 4;
	mips->nextPC = where;
	mips->inDelaySlot = true;
}

static inline void SkipLikely(MIPSState *mips) {
	const u32 delaySlotPC = PC + 4;
	const MIPSOpcode delaySlotOp(mips->space->IsValid4AlignedAddress(delaySlotPC) ? mips->space->ReadUnchecked_U32(delaySlotPC) : 0);
	// Don't actually skip if it is a jump (seen in Brooktown High.)
	if (MIPSGetInfo(delaySlotOp) & IS_JUMP) {
		PC += 4;
	} else {
		PC += 8;
	}
}

static u32 g_cop0[32];
static u32 g_cop0Control[32];

namespace MIPSInt {
	void Int_Cache(MIPSState *mips, MIPSOpcode op) {
		// Cache operations need no emulation: translated and interpreted code is
		// looked up by the bytes in memory.
		PC += 4;
	}

	void Int_Syscall(MIPSState *mips, MIPSOpcode op) {
		// Import stubs are bound to HLE functions before they run, so a syscall that
		// reaches the interpreter has no handler.
		mips->raise(Fault::Syscall);
	}

	void Int_Sync(MIPSState *mips, MIPSOpcode op) {
		//DEBUG_LOG(Log::CPU, "sync");
		PC += 4;
	}

	void Int_Break(MIPSState *mips, MIPSOpcode op) {
		mips->raise(Fault::Break);
	}

	void Int_RelBranch(MIPSState *mips, MIPSOpcode op) {
		int imm = _SIMM16_SHL2;
		int rs = _RS;
		int rt = _RT;
		u32 addr = PC + imm + 4;

		switch (op >> 26)
		{
		case 4:  if (R(rt) == R(rs))  DelayBranchTo(mips, addr); else PC += 4; break; //beq
		case 5:  if (R(rt) != R(rs))  DelayBranchTo(mips, addr); else PC += 4; break; //bne
		case 6:  if ((s32)R(rs) <= 0) DelayBranchTo(mips, addr); else PC += 4; break; //blez
		case 7:  if ((s32)R(rs) > 0) DelayBranchTo(mips, addr); else PC += 4; break; //bgtz

		case 20: if (R(rt) == R(rs))  DelayBranchTo(mips, addr); else SkipLikely(mips); break; //beql
		case 21: if (R(rt) != R(rs))  DelayBranchTo(mips, addr); else SkipLikely(mips); break; //bnel
		case 22: if ((s32)R(rs) <= 0) DelayBranchTo(mips, addr); else SkipLikely(mips); break; //blezl
		case 23: if ((s32)R(rs) >  0) DelayBranchTo(mips, addr); else SkipLikely(mips); break; //bgtzl

		default:
			_dbg_assert_msg_(false,"Trying to interpret instruction that can't be interpreted");
			break;
		}
	}

	void Int_RelBranchRI(MIPSState *mips, MIPSOpcode op) {
		int imm = _SIMM16_SHL2;
		int rs = _RS;
		u32 addr = PC + imm + 4;
		// The linking ones test rs from before they link, ra included (cpu/cpu_alu/branch_link_ra).
		const s32 value = (s32)R(rs);

		switch ((op>>16) & 0x1F) {
		case 0: if (value <  0) DelayBranchTo(mips, addr); else PC += 4; break;//bltz
		case 1: if (value >= 0) DelayBranchTo(mips, addr); else PC += 4; break;//bgez
		case 2: if (value <  0) DelayBranchTo(mips, addr); else SkipLikely(mips); break;//bltzl
		case 3: if (value >= 0) DelayBranchTo(mips, addr); else SkipLikely(mips); break;//bgezl
		case 16: R(MIPS_REG_RA) = PC + 8; if (value <  0) DelayBranchTo(mips, addr); else PC += 4; break;//bltzal
		case 17: R(MIPS_REG_RA) = PC + 8; if (value >= 0) DelayBranchTo(mips, addr); else PC += 4; break;//bgezal
		case 18: R(MIPS_REG_RA) = PC + 8; if (value <  0) DelayBranchTo(mips, addr); else SkipLikely(mips); break;//bltzall
		case 19: R(MIPS_REG_RA) = PC + 8; if (value >= 0) DelayBranchTo(mips, addr); else SkipLikely(mips); break;//bgezall
		default:
			_dbg_assert_msg_(false,"Trying to interpret instruction that can't be interpreted");
			break;
		}
	}

	void Int_VBranch(MIPSState *mips, MIPSOpcode op) {
		int imm = _SIMM16_SHL2;
		u32 addr = PC + imm + 4;

		// x, y, z, w, any, all, (invalid), (invalid)
		int imm3 = (op>>18)&7;
		int val = (mips->vfpuCtrl[VFPU_CTRL_CC] >> imm3) & 1;

		switch ((op >> 16) & 3) {
		case 0: if (!val) DelayBranchTo(mips, addr); else PC += 4; break; //bvf
		case 1: if ( val) DelayBranchTo(mips, addr); else PC += 4; break; //bvt
		case 2: if (!val) DelayBranchTo(mips, addr); else SkipLikely(mips); break; //bvfl
		case 3: if ( val) DelayBranchTo(mips, addr); else SkipLikely(mips); break; //bvtl
		}
	}

	void Int_FPUBranch(MIPSState *mips, MIPSOpcode op) {
		int imm = _SIMM16_SHL2;
		u32 addr = PC + imm + 4;
		switch((op >> 16) & 0x1f)
		{
		case 0: if (!FPCOND) DelayBranchTo(mips, addr); else PC += 4; break;//bc1f
		case 1: if ( FPCOND) DelayBranchTo(mips, addr); else PC += 4; break;//bc1t
		case 2: if (!FPCOND) DelayBranchTo(mips, addr); else SkipLikely(mips); break;//bc1fl
		case 3: if ( FPCOND) DelayBranchTo(mips, addr); else SkipLikely(mips); break;//bc1tl
		default:
			_dbg_assert_msg_(false,"Trying to interpret instruction that can't be interpreted");
			break;
		}
	}

	void Int_JumpType(MIPSState *mips, MIPSOpcode op) {
		if (mips->inDelaySlot) {
			// There's one of these in Star Soldier at 0881808c, which seems benign.
			ERROR_LOG(Log::CPU, "Jump in delay slot at %08x", mips->pc);
		}

		const u32 off = ((op & 0x03FFFFFF) << 2);
		const u32 addr = (mips->pc & 0xF0000000) | off;

		switch (op >> 26) {
		case 2: //j
			if (!mips->inDelaySlot)
				DelayBranchTo(mips, addr);
			break;
		case 3: //jal
			R(MIPS_REG_RA) = PC + 8;
			if (!mips->inDelaySlot)
				DelayBranchTo(mips, addr);
			break;
		default:
			_dbg_assert_msg_(false,"Trying to interpret instruction that can't be interpreted");
			break;
		}
	}

	void Int_JumpRegType(MIPSState *mips, MIPSOpcode op) {
		if (mips->inDelaySlot) {
			// There's one of these in Star Soldier at 0881808c, which seems benign.
			ERROR_LOG(Log::CPU, "Jump in delay slot at %08x", mips->pc);
		}

		int rs = _RS;
		int rd = _RD;
		u32 addr = R(rs);
		switch (op & 0x3f) {
		case 8: //jr
			if (!mips->inDelaySlot)
				DelayBranchTo(mips, addr);
			break;
		case 9: //jalr
			if (rd != 0)
				R(rd) = PC + 8;
			// Update rd, but otherwise do not take the branch if we're branching.
			if (!mips->inDelaySlot)
				DelayBranchTo(mips, addr);
			break;
		}
	}

	void Int_IType(MIPSState *mips, MIPSOpcode op) {
		u32 uimm = op & 0xFFFF;
		u32 suimm = SignExtend16ToU32(op);
		s32 simm = SignExtend16ToS32(op);

		int rt = _RT;
		int rs = _RS;

		if (rt == 0) { //destination register is zero register
			PC += 4;
			return; //nop
		}

		switch (op>>26)
		{
		case 8:	R(rt) = R(rs) + simm; break; //addi
		case 9:	R(rt) = R(rs) + simm; break;	//addiu
		case 10: R(rt) = (s32)R(rs) < simm; break; //slti
		case 11: R(rt) = R(rs) < suimm; break; //sltiu
		case 12: R(rt) = R(rs) & uimm; break; //andi
		case 13: R(rt) = R(rs) | uimm; break; //ori
		case 14: R(rt) = R(rs) ^ uimm; break; //xori
		case 15: R(rt) = uimm << 16;	 break; //lui
		default:
			_dbg_assert_msg_(false,"Trying to interpret instruction that can't be interpreted");
			break;
		}
		PC += 4;
	}

	void Int_StoreSync(MIPSState *mips, MIPSOpcode op) {
		int imm = (signed short)(op & 0xFFFF);
		int rt = _RT;
		int rs = _RS;
		u32 addr = R(rs) + imm;

		switch (op >> 26) {
		case 48: // ll
			if (rt != 0) {
				if (!mips->space->IsValid4AlignedAddress(addr)) {
					Core_MemoryException(mips, addr, 4, "ll");
					R(rt) = 0;
				} else {
					R(rt) = mips->space->ReadUnchecked_U32(addr);
				}
			}
			g_llBit = 1;
			break;
		case 56: // sc
			if (g_llBit) {
				if (!mips->space->IsValid4AlignedAddress(addr)) {
					Core_MemoryException(mips, addr, 4, "sc");
				} else {
					mips->space->WriteUnchecked_U32(R(rt), addr);
				}
				// Report success even if the store got dropped - reporting failure just makes
				// the usual retry loop spin on the same bad address forever.
				if (rt != 0) {
					R(rt) = 1;
				}
			} else if (rt != 0) {
				R(rt) = 0;
			}
			break;
		default:
			_dbg_assert_msg_(false,"Trying to interpret instruction that can't be interpreted");
			break;
		}
		PC += 4;
	}

	void Int_RType3(MIPSState *mips, MIPSOpcode op) {
		int rt = _RT;
		int rs = _RS;
		int rd = _RD;

		// Don't change $zr.
		if (rd == 0) {
			PC += 4;
			return;
		}

		switch (op & 63) {
		case 10: if (R(rt) == 0) R(rd) = R(rs); break; //movz
		case 11: if (R(rt) != 0) R(rd) = R(rs); break; //movn
		case 32: R(rd) = R(rs) + R(rt);		break; //add (exception on overflow)
		case 33: R(rd) = R(rs) + R(rt);		break; //addu
		case 34: R(rd) = R(rs) - R(rt);		break; //sub (exception on overflow)
		case 35: R(rd) = R(rs) - R(rt);		break; //subu
		case 36: R(rd) = R(rs) & R(rt);		break; //and
		case 37: R(rd) = R(rs) | R(rt);		break; //or
		case 38: R(rd) = R(rs) ^ R(rt);		break; //xor
		case 39: R(rd) = ~(R(rs) | R(rt)); break; //nor
		case 42: R(rd) = (s32)R(rs) < (s32)R(rt); break; //slt
		case 43: R(rd) = R(rs) < R(rt);		break; //sltu
		case 44: R(rd) = ((s32)R(rs) > (s32)R(rt)) ? R(rs) : R(rt); break; //max
		case 45: R(rd) = ((s32)R(rs) < (s32)R(rt)) ? R(rs) : R(rt); break;//min
		default:
			_dbg_assert_msg_( 0, "Unknown MIPS instruction %08x", op.encoding);
			break;
		}
		PC += 4;
	}

	// On a bad access, loads produce zero, stores are dropped, and PC advances either way.
	void Int_ITypeMem(MIPSState *mips, MIPSOpcode op) {
		int imm = (signed short)(op&0xFFFF);
		int rt = _RT;
		int rs = _RS;
		const u32 addr = R(rs) + imm;

		if (((op >> 29) & 1) == 0 && rt == 0) {
			// Don't load anything into $zr
			PC += 4;
			return;
		}

		switch (op >> 26) {
		case 32:
			if (!mips->space->IsValidAddress(addr)) {
				Core_MemoryException(mips, addr, 1, "lb");
				R(rt) = 0;
				break;
			}
			R(rt) = SignExtend8ToU32(mips->space->ReadUnchecked_U8(addr));
			break; //lb
		case 33:
			if (!mips->space->IsValid2AlignedAddress(addr)) {
				Core_MemoryException(mips, addr, 2, "lh");
				R(rt) = 0;
				break;
			}
			R(rt) = SignExtend16ToU32(mips->space->ReadUnchecked_U16(addr));
			break; //lh
		case 35:
			if (!mips->space->IsValid4AlignedAddress(addr)) {
				Core_MemoryException(mips, addr, 4, "lw");
				R(rt) = 0;
				break;
			}
			R(rt) = mips->space->ReadUnchecked_U32(addr);
			break; //lw
		case 36:
			if (!mips->space->IsValidAddress(addr)) {
				Core_MemoryException(mips, addr, 1, "lbu");
				R(rt) = 0;
				break;
			}
			R(rt) = mips->space->ReadUnchecked_U8(addr);
			break; //lbu
		case 37:
			if (!mips->space->IsValid2AlignedAddress(addr)) {
				Core_MemoryException(mips, addr, 2, "lhu");
				R(rt) = 0;
				break;
			}
			R(rt) = mips->space->ReadUnchecked_U16(addr);
			break; //lhu
		case 40:
			if (!mips->space->IsValidAddress(addr)) {
				Core_MemoryException(mips, addr, 1, "sb");
				break;
			}
			mips->space->WriteUnchecked_U8(R(rt), addr);
			break; //sb
		case 41:
			if (!mips->space->IsValid2AlignedAddress(addr)) {
				Core_MemoryException(mips, addr, 2, "sh");
				break;
			}
			mips->space->WriteUnchecked_U16(R(rt), addr);
			break; //sh
		case 43:
			if (!mips->space->IsValid4AlignedAddress(addr)) {
				Core_MemoryException(mips, addr, 4, "sw");
				break;
			}
			mips->space->WriteUnchecked_U32(R(rt), addr);
			break; //sw

		// When there's an LWL and an LWR together, we should be able to peephole optimize that
		// into a single non-alignment-checking LW.
		case 34: //lwl
			{
				// Not checking for alignment here - the actual read will be aligned.
				u32 mem = 0;
				if (mips->space->IsValidAddress(addr)) {
					mem = mips->space->ReadUnchecked_U32(addr & 0xfffffffc);
				} else {
					Core_MemoryException(mips, addr, 4, "lwl");
				}
				u32 shift = (addr & 3) * 8;
				u32 result = ( u32(R(rt)) & (0x00ffffff >> shift) ) | ( mem << (24 - shift) );
				R(rt) = result;
			}
			break;

		case 38: //lwr
			{
				// Not checking for alignment here - the actual read will be aligned.
				u32 mem = 0;
				if (mips->space->IsValidAddress(addr)) {
					mem = mips->space->ReadUnchecked_U32(addr & 0xfffffffc);
				} else {
					Core_MemoryException(mips, addr, 4, "lwr");
				}
				u32 shift = (addr & 3) * 8;
				u32 regval = R(rt);
				u32 result = ( regval & (0xffffff00 << (24 - shift)) ) | ( mem	>> shift );
				R(rt) = result;
			}
			break;

		case 42: //swl
			{
				// Not checking for alignment here - the actual read/write will be aligned.
				if (!mips->space->IsValidAddress(addr)) {
					Core_MemoryException(mips, addr, 4, "swl");
					break;
				}
				u32 shift = (addr & 3) * 8;
				u32 mem = mips->space->ReadUnchecked_U32(addr & 0xfffffffc);
				u32 result = ( ( u32(R(rt)) >>	(24 - shift) ) ) | (	mem & (0xffffff00 << shift) );
				mips->space->WriteUnchecked_U32(result, (addr & 0xfffffffc));
			}
			break;

		case 46: //swr
			{
				// Not checking for alignment here - the actual read/write will be aligned.
				if (!mips->space->IsValidAddress(addr)) {
					Core_MemoryException(mips, addr, 4, "swr");
					break;
				}
				u32 shift = (addr & 3) << 3;
				u32 mem = mips->space->ReadUnchecked_U32(addr & 0xfffffffc);
				u32 result = ( ( u32(R(rt)) << shift ) | (mem	& (0x00ffffff >> (24 - shift)) ) );
				mips->space->WriteUnchecked_U32(result, (addr & 0xfffffffc));
			}
			break;

		default:
			_dbg_assert_msg_(false,"Trying to interpret Mem instruction that can't be interpreted");
			break;
		}
		PC += 4;
	}

	void Int_FPULS(MIPSState *mips, MIPSOpcode op) 	{
		s32 offset = (s16)(op & 0xFFFF);
		int ft = _FT;
		int rs = _RS;
		u32 addr = R(rs) + offset;

		switch (op >> 26) {
		case 49:
			if (!mips->space->IsValid4AlignedAddress(addr)) {
				Core_MemoryException(mips, addr, 4, "lwc1");
				FI(ft) = 0;
				break;
			}
			FI(ft) = mips->space->ReadUnchecked_U32(addr);
			break; //lwc1
		case 57:
			if (!mips->space->IsValid4AlignedAddress(addr)) {
				Core_MemoryException(mips, addr, 4, "swc1");
				break;
			}
			mips->space->WriteUnchecked_U32(FI(ft), addr);
			break; //swc1
		default:
			_dbg_assert_msg_(false,"Trying to interpret FPULS instruction that can't be interpreted");
			break;
		}
		PC += 4;
	}

	void Int_mxc1(MIPSState *mips, MIPSOpcode op)
	{
		int fs = _FS;
		int rt = _RT;

		switch ((op>>21)&0x1f) {
		case 0: //mfc1
			if (rt != 0)
				R(rt) = FI(fs);
			break;

		case 2: //cfc1
			if (rt != 0) {
				if (fs == 31) {
					R(rt) = mips->fcr31;
				} else if (fs == 0) {
					R(rt) = FCR0_VALUE;
				} else {
					WARN_LOG_REPORT(Log::CPU, "ReadFCR: Unexpected reg %d", fs);
					R(rt) = 0;
				}
				break;
			}
			break;

		case 4: //mtc1
			FI(fs) = R(rt);
			break;

		case 6: //ctc1
			{
				u32 value = R(rt);
				if (fs == 31) {
					mips->fcr31 = value & 0x0181FFFF;
					// The host FPU runs in the guest's rounding mode, so it has to switch
					// right here rather than at the next block boundary.
					ApplyHostRoundingMode(mips);
				} else {
					WARN_LOG_REPORT(Log::CPU, "WriteFCR: Unexpected reg %d (value %08x)", fs, value);
				}
				DEBUG_LOG(Log::CPU, "FCR%i written to, value %08x", fs, value);
				break;
			}

		default:
			_dbg_assert_msg_(false,"Trying to interpret instruction that can't be interpreted");
			break;
		}
		PC += 4;
	}

	// COP0, for kernel-mode code (e.g. flash0:/reboot.bin) that reads/writes it directly - ordinary
	// PSP user-mode code never executes these instructions (kernel state is reached via HLE
	// syscalls instead). Writes read back; nothing models interrupts, exceptions or the TLB.
	void Int_Cop0(MIPSState *mips, MIPSOpcode op) {
		// COP0, for kernel-mode code that reads/writes it directly. Writes read back;
		// nothing models interrupts, exceptions or the TLB.
		int rt = _RT;
		int rd = _RD;

		switch ((op >> 21) & 0x1f) {
		case 0: //mfc0
			if (rt != 0)
				R(rt) = g_cop0[rd];
			break;
		case 2: //cfc0
			if (rt != 0)
				R(rt) = g_cop0Control[rd];
			break;
		case 6: //ctc0
			g_cop0Control[rd] = R(rt);
			break;
		case 4: //mtc0
			g_cop0[rd] = R(rt);
			break;
		case 10: //rdpgpr
			if (rt != 0)
				R(rt) = R(rd);
			break;
		case 11: //mfmc0 (di/ei)
			if (rt != 0)
				R(rt) = g_cop0[12];  // Status
			break;
		case 14: //wrpgpr
			R(rd) = R(rt);
			break;
		default:
			break;
		}
		PC += 4;
	}

	// Return from an exception, as MIPS32 defines it (cpu/cop0/eret): to ErrorEPC (30) if Status.ERL
	// is set, clearing only that, otherwise to EPC (14), clearing Status.EXL. No delay slot.
	void Int_Eret(MIPSState *mips, MIPSOpcode op) {
		u32 &status = g_cop0[12];
		if (status & 4) {
			mips->pc = g_cop0[30];
			status &= ~4;
		} else {
			mips->pc = g_cop0[14];
			status &= ~2;
		}
		g_llBit = 0;
	}

	void Int_RType2(MIPSState *mips, MIPSOpcode op) {
		int rs = _RS;
		int rd = _RD;

		// Don't change $zr.
		if (rd == 0) {
			PC += 4;
			return;
		}

		switch (op & 63) {
		case 22:	//clz
			R(rd) = clz32(R(rs));
			break;
		case 23: //clo
			R(rd) = clz32(~R(rs));
			break;
		default:
			_dbg_assert_msg_(false,"Trying to interpret instruction that can't be interpreted");
			break;
		}
		PC += 4;
	}

	void Int_MulDivType(MIPSState *mips, MIPSOpcode op)
	{
		int rt = _RT;
		int rs = _RS;
		int rd = _RD;

		switch (op & 63)
		{
		case 24: //mult
			{
				s64 result = (s64)(s32)R(rs) * (s64)(s32)R(rt);
				u64 resultBits = (u64)(result);
				LO = (u32)(resultBits);
				HI = (u32)(resultBits>>32);
			}
			break;
		case 25: //multu
			{
				u64 resultBits = (u64)R(rs) * (u64)R(rt);
				LO = (u32)(resultBits);
				HI = (u32)(resultBits>>32);
			}
			break;
		case 28: //madd
			{
				u32 a=R(rs),b=R(rt),hi=HI,lo=LO;
				u64 origValBits = (u64)lo | ((u64)(hi)<<32);
				s64 origVal = (s64)origValBits;
				s64 result = origVal + (s64)(s32)a * (s64)(s32)b;
				u64 resultBits = (u64)(result);
				LO = (u32)(resultBits);
				HI = (u32)(resultBits>>32);
			}
			break;
		case 29: //maddu
			{
				u32 a=R(rs),b=R(rt),hi=HI,lo=LO;
				u64 origVal = (u64)lo | ((u64)(hi)<<32);
				u64 result = origVal + (u64)a * (u64)b;
				LO = (u32)(result);
				HI = (u32)(result>>32);
			}
			break;
		case 46: //msub
			{
				u32 a=R(rs),b=R(rt),hi=HI,lo=LO;
				u64 origValBits = (u64)lo | ((u64)(hi)<<32);
				s64 origVal = (s64)origValBits;
				s64 result = origVal - (s64)(s32)a * (s64)(s32)b;
				u64 resultBits = (u64)(result);
				LO = (u32)(resultBits);
				HI = (u32)(resultBits>>32);
			}
			break;
		case 47: //msubu
			{
				u32 a=R(rs),b=R(rt),hi=HI,lo=LO;
				u64 origVal = (u64)lo | ((u64)(hi)<<32);
				u64 result = origVal - (u64)a * (u64)b;
				LO = (u32)(result);
				HI = (u32)(result>>32);
			}
			break;
		case 16: if (rd != 0) R(rd) = HI; break; //mfhi
		case 17: HI = R(rs); break; //mthi
		case 18: if (rd != 0) R(rd) = LO; break; //mflo
		case 19: LO = R(rs); break; //mtlo
		case 26: //div
			{
				s32 a = (s32)R(rs);
				s32 b = (s32)R(rt);
				if (a == (s32)0x80000000 && b == -1) {
					// The one overflow. Hardware leaves the remainder at zero (cpu/cpu_alu/cpu_div).
					LO = 0x80000000;
					HI = 0;
				} else if (b != 0) {
					LO = (u32)(a / b);
					HI = (u32)(a % b);
				} else {
					LO = a < 0 ? 1 : -1;
					HI = a;
				}
			}
			break;
		case 27: //divu
			{
				u32 a = R(rs);
				u32 b = R(rt);
				if (b != 0) {
					LO = (a/b);
					HI = (a%b);
				} else {
					LO = a <= 0xFFFF ? 0xFFFF : -1;
					HI = a;
				}
			}
			break;

		default:
			_dbg_assert_msg_(false,"Trying to interpret instruction that can't be interpreted");
			break;
		}
		PC += 4;
	}


	void Int_ShiftType(MIPSState *mips, MIPSOpcode op)
	{
		int rt = _RT;
		int rs = _RS;
		int rd = _RD;
		int sa = _FD;

		// Don't change $zr.
		if (rd == 0)
		{
			PC += 4;
			return;
		}

		switch (op & 0x3f)
		{
		case 0: R(rd) = R(rt) << sa; break; //sll
		case 2:
			// Only the low bit of the unused rs field counts (cpu/cpu_alu/shift_encodings).
			if (_RS & 1) { //rotr
				R(rd) = __rotr(R(rt), sa);
			} else { //srl
				R(rd) = R(rt) >> sa;
			}
			break;

		case 3: R(rd) = (u32)(((s32)R(rt)) >> sa); break; //sra
		case 4: R(rd) = R(rt) << (R(rs) & 0x1F); break; //sllv
		case 6:
			// Same for the unused sa field.
			if (_FD & 1) { // rotrv
				R(rd) = __rotr(R(rt), R(rs));
			} else { //srlv
				R(rd) = R(rt) >> (R(rs) & 0x1F);
			}
			break;
		case 7: R(rd) = (u32)(((s32)R(rt)) >> (R(rs) & 0x1F)); break; //srav
		default:
			_dbg_assert_msg_(false,"Trying to interpret instruction that can't be interpreted");
			break;
		}
		PC += 4;
	}

	void Int_Allegrex(MIPSState *mips, MIPSOpcode op) {
		int rt = _RT;
		int rd = _RD;

		// Don't change $zr.
		if (rd == 0) {
			PC += 4;
			return;
		}

		switch (AllegrexBitOp(op)) {
		case 16: // seb
			R(rd) = SignExtend8ToU32(R(rt));
			break;

		case 20: // bitrev
			R(rd) = ReverseBits32(R(rt));
			break;

		case 24: // seh
			R(rd) = SignExtend16ToU32(R(rt));
			break;

		default:
			_dbg_assert_msg_(false,"Trying to interpret ALLEGREX instruction that can't be interpreted");
			break;
		}
		PC += 4;
	}

	void Int_Allegrex2(MIPSState *mips, MIPSOpcode op) {
		int rt = _RT;
		int rd = _RD;

		// Don't change $zr.
		if (rd == 0) {
			PC += 4;
			return;
		}

		switch (AllegrexBitOp(op)) {
		case 2: //wsbh
			R(rd) = ((R(rt) & 0xFF00FF00) >> 8) | ((R(rt) & 0x00FF00FF) << 8);
			break;
		case 3: //wsbw
			R(rd) = swap32(R(rt));
			break;
		default:
			_dbg_assert_msg_(false,"Trying to interpret ALLEGREX instruction that can't be interpreted");
			break;
		}
		PC += 4;
	}

	// Only the Media Engine runs code that halts. It sleeps until an interrupt.
	void Int_Halt(MIPSState *mips, MIPSOpcode op) {
		// Only the Media Engine halts.
		mips->raise(Fault::Illegal);
	}

	// An unknown Media Engine instruction (primary opcode 0x2C): Sony's ME firmware runs 0xB0E20000
	// (with v0 = 1) and 0xB0E00000 around a reset of sysreg 0x50/0x54 bits when it resumes from sleep
	// (t2img 0x88300948). Possibly a VME control write. Ignored.
	void Int_Me2C(MIPSState *mips, MIPSOpcode op) {
		// A Media Engine-only encoding.
		mips->raise(Fault::Illegal);
	}

	// The interrupt enable flag, shared with the HLE sceKernelCpuSuspendIntr/ResumeIntr, which on
	// hardware are just mfic v0, $0; mtic zero, $0 and mtic a0, $0. Only $0 exists.
	void Int_Special2(MIPSState *mips, MIPSOpcode op) {
		int rt = _RT;
		switch (op & 0x3F) {
		case 36:  // mfic
			if (rt != 0)
				R(rt) = InterruptsEnabled(mips) ? 1 : 0;
			break;
		case 38:  // mtic
			// Only bit 0 counts: mtic 2 disables.
			SetInterruptsEnabled(mips, (R(rt) & 1) != 0);
			break;
		}
		PC += 4;
	}

	void Int_Special3(MIPSState *mips, MIPSOpcode op) {
		int rs = _RS;
		int rt = _RT;
		int pos = _POS;

		// Don't change $zr.
		if (rt == 0) {
			PC += 4;
			return;
		}

		switch (op & 0x3f) {
		case 0x0: //ext
			{
				int size = _SIZE + 1;
				u32 sourcemask = 0xFFFFFFFFUL >> (32 - size);
				R(rt) = (R(rs) >> pos) & sourcemask;
			}
			break;
		case 0x4: //ins
			{
				// The size field actually holds msb (= pos + size - 1), so build the mask from
				// that and shift it down, the way the JITs do. Computing the width as
				// (_SIZE + 1) - pos instead would shift by 32 or more when msb < pos - undefined
				// behavior, and on x86 it yields an all-ones mask that writes bits the JITs leave
				// alone. Hardware calls that encoding unpredictable, so all we need is to be
				// consistent and not invoke UB.
				const u32 mask = 0xFFFFFFFFUL >> (31 - _SIZE);
				const u32 sourcemask = mask >> pos;
				const u32 destmask = sourcemask << pos;
				R(rt) = (R(rt) & ~destmask) | ((R(rs) & sourcemask) << pos);
			}
			break;
		}

		PC += 4;
	}

	void Int_FPU2op(MIPSState *mips, MIPSOpcode op) {
		int fs = _FS;
		int fd = _FD;

		switch (op & 0x3f)
		{
		case 4:	//sqrt
			F(fd) = sqrtf(F(fs));
			// A negative input gives a positive NaN, not the host's (cpu/fpu/roundmode).
			if (F(fs) < 0.0f) {
				FsI(fd) = 0x7FC00000;
			}
			break;
		case 5:	F(fd)	= fabsf(F(fs)); break; //abs
		case 6:	F(fd)	= F(fs); break; //mov
		case 7:	F(fd)	= -F(fs); break; //neg
		case 12:
		case 13:
		case 14:
		case 15:
			switch (op & 0x3f)
			{
			// round.w.s is round-half-to-even, not half-away-from-zero - and its mode is fixed,
			// so unlike cvt.w.s below it must not follow fcr31. round_ieee_754 is both.
			case 12: FsI(fd) = SaturatedFloatToInt(round_ieee_754(FlushForConversion(mips, F(fs)))); break; //round.w.s
			case 13: FsI(fd) = SaturatedFloatToInt(truncf(FlushForConversion(mips, F(fs)))); break; //trunc.w.s
			case 14: FsI(fd) = SaturatedFloatToInt(ceilf(FlushForConversion(mips, F(fs)))); break; //ceil.w.s
			case 15: FsI(fd) = SaturatedFloatToInt(floorf(FlushForConversion(mips, F(fs)))); break; //floor.w.s
			}
			break;
		case 32: F(fd) = (float)FsI(fs); break; //cvt.s.w

		case 36:
			switch (mips->fcr31 & 3)
			{
			case 0: FsI(fd) = SaturatedFloatToInt(round_ieee_754(FlushForConversion(mips, F(fs)))); break;  // RINT_0
			case 1: FsI(fd) = SaturatedFloatToInt(truncf(FlushForConversion(mips, F(fs)))); break;  // CAST_1
			case 2: FsI(fd) = SaturatedFloatToInt(ceilf(FlushForConversion(mips, F(fs)))); break;  // CEIL_2
			case 3: FsI(fd) = SaturatedFloatToInt(floorf(FlushForConversion(mips, F(fs)))); break;  // FLOOR_3
			}
			break; //cvt.w.s
		default:
			_dbg_assert_msg_(false,"Trying to interpret FPU2Op instruction that can't be interpreted");
			break;
		}
		PC += 4;
	}

	void Int_FPUComp(MIPSState *mips, MIPSOpcode op) {
		int fs = _FS;
		int ft = _FT;
		bool cond;
		switch (op & 0xf)
		{
		case 0: //f
		case 8: //sf
			cond = false;
			break;

		case 1: //un
		case 9: //ngle
			cond = my_isnan(F(fs)) || my_isnan(F(ft));
			break;

		case 2: //eq
		case 10: //seq
			cond = !my_isnan(F(fs)) && !my_isnan(F(ft)) && (F(fs) == F(ft));
			break;

		case 3: //ueq
		case 11: //ngl
			cond = (F(fs) == F(ft)) || my_isnan(F(fs)) || my_isnan(F(ft));
			break;

		case 4: //olt
		case 12: //lt
			cond = (F(fs) < F(ft));
			break;

		case 5: //ult
		case 13: //nge
			cond = (F(fs) < F(ft)) || my_isnan(F(fs)) || my_isnan(F(ft));
			break;

		case 6: //ole
		case 14: //le
			cond = (F(fs) <= F(ft));
			break;

		case 7: //ule
		case 15: //ngt
			cond = (F(fs) <= F(ft)) || my_isnan(F(fs)) || my_isnan(F(ft));
			break;

		default:
			_dbg_assert_msg_(false,"Trying to interpret FPUComp instruction that can't be interpreted");
			cond = false;
			break;
		}
		mips->fcr31 = (mips->fcr31 & ~(1u << 23)) | ((cond ? 1u : 0u) << 23);
		PC += 4;
	}

	void Int_FPU3op(MIPSState *mips, MIPSOpcode op) {
		int ft = _FT;
		int fs = _FS;
		int fd = _FD;

		switch (op & 0x3f)
		{
		case 0: F(fd) = F(fs) + F(ft); break; // add.s
		case 1: F(fd) = F(fs) - F(ft); break; // sub.s
		case 2: // mul.s
			if ((my_isinf(F(fs)) && F(ft) == 0.0f) || (my_isinf(F(ft)) && F(fs) == 0.0f)) {
				// Must be positive NAN, see #12519.
				FI(fd) = 0x7fc00000;
			} else if (my_isnan(F(fs)) || my_isnan(F(ft))) {
				// The first NaN operand comes through, quieted (cpu/fpu/roundmode). The host
				// compiler may swap the operands of a commutative multiply, so not left to it.
				FI(fd) = (my_isnan(F(fs)) ? FI(fs) : FI(ft)) | 0x00400000;
			} else {
				F(fd) = F(fs) * F(ft);
			}
			break;
		case 3: // div.s
		{
			// The only FPU exception we implement so far. IEEE 754 raises divide-by-zero only when
			// an exact infinity comes out of finite operands, so 0/0 (invalid operation, which we
			// don't detect) and inf/0 and NaN operands (no exception at all) are all excluded.
			// Cause is per-instruction, so clear it here rather than leaving the last one set.
			// Real hardware clears all five Cause bits in every FPU op, we only manage this one.
			mips->fcr31 &= ~FCR31_CAUSE_DIV_BY_ZERO;
			if (F(ft) == 0.0f && F(fs) != 0.0f && !my_isnanorinf(F(fs))) {
				mips->fcr31 |= FCR31_CAUSE_DIV_BY_ZERO;
				if (mips->fcr31 & FCR31_ENABLE_DIV_BY_ZERO) {
					// Unmasked. Hardware would trap, but PSP threads start with this trap
					// enabled while games divide by zero all the time; like PPSSPP's default,
					// the result is computed and only the cause bit records it.
				} else {
					// Masked: sticky flag, and the default result (a correctly signed infinity).
					mips->fcr31 |= FCR31_FLAG_DIV_BY_ZERO;
				}
			}
			F(fd) = F(fs) / F(ft);
			break;
		}
		default:
			_dbg_assert_msg_(false,"Trying to interpret FPU3Op instruction that can't be interpreted");
			break;
		}
		PC += 4;
	}

	void Int_Interrupt(MIPSState *mips, MIPSOpcode op) {
		switch (op & 1)
		{
		case 0:
			break;
		}
		PC += 4;
	}

}

} // namespace anypsp::cpu
