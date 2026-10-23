// SPDX-License-Identifier: GPL-2.0-or-later
//
// Stand-ins for the PPSSPP utilities (Common/, Core/Reporting.h) that the
// adapted interpreter sources in this directory use.

#pragma once

#include "mips_state.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(__aarch64__) || defined(_M_ARM64)
#define ANYPSP_ARCH_ARM64_NEON 1
#include <arm_neon.h>
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define ANYPSP_ARCH_SSE2 1
#include <emmintrin.h>
#endif

#if defined(_MSC_VER)
#define NO_INLINE __declspec(noinline)
#else
#define NO_INLINE __attribute__((noinline))
#endif

// PPSSPP's logging and assertion macros. The interpreter only uses them for
// conditions it then handles, so they compile to nothing here.
#define ERROR_LOG(...) ((void)0)
#define ERROR_LOG_REPORT(...) ((void)0)
#define ERROR_LOG_REPORT_ONCE(...) ((void)0)
#define WARN_LOG(...) ((void)0)
#define WARN_LOG_REPORT(...) ((void)0)
#define WARN_LOG_REPORT_ONCE(...) ((void)0)
#define DEBUG_LOG(...) ((void)0)
#define VERBOSE_LOG(...) ((void)0)
#define _assert_msg_(...) ((void)0)
#define _dbg_assert_msg_(...) ((void)0)
#define _dbg_assert_(...) ((void)0)
#define _assert_(...) ((void)0)

namespace anypsp::cpu {

inline u32 clz32_nonzero(u32 value) { return static_cast<u32>(std::countl_zero(value)); }
inline u32 clz32(u32 value) { return static_cast<u32>(std::countl_zero(value)); }
inline u32 __rotr(u32 value, int shift) { return std::rotr(value, shift & 31); }
inline u32 ReverseBits32(u32 value) {
    value = ((value >> 1u) & 0x55555555u) | ((value & 0x55555555u) << 1u);
    value = ((value >> 2u) & 0x33333333u) | ((value & 0x33333333u) << 2u);
    value = ((value >> 4u) & 0x0F0F0F0Fu) | ((value & 0x0F0F0F0Fu) << 4u);
    value = ((value >> 8u) & 0x00FF00FFu) | ((value & 0x00FF00FFu) << 8u);
    return (value >> 16u) | (value << 16u);
}
inline u32 swap32(u32 value) {
    return (value >> 24u) | ((value >> 8u) & 0xFF00u) | ((value << 8u) & 0xFF0000u) | (value << 24u);
}
// The SPECIAL3 BSHFL encodings decode only some sa bits (PPSSPP Core/MIPS/MIPSCodeUtils.h):
// wsbh/wsbw (2, 3), seb (16), bitrev (20), seh (24).
inline int AllegrexBitOp(MIPSOpcode op) {
    const int sa = static_cast<int>((op >> 6) & 31);
    if (!(sa & 0x10)) return 2 | (sa & 1);
    if (sa & 4) return 20;
    return (sa & 8) ? 24 : 16;
}

inline s32 SignExtend16ToS32(u32 value) { return static_cast<s32>(static_cast<s16>(value & 0xFFFFu)); }
inline u32 SignExtend16ToU32(u32 value) { return static_cast<u32>(SignExtend16ToS32(value)); }
inline u32 SignExtend8ToU32(u32 value) { return static_cast<u32>(static_cast<s32>(static_cast<s8>(value & 0xFFu))); }

inline bool my_isnan(float f) {
    const u32 u = std::bit_cast<u32>(f);
    return (u & 0x7F800000u) == 0x7F800000u && (u & 0x7FFFFFu) != 0u;
}
inline bool my_isinf(float f) {
    const u32 u = std::bit_cast<u32>(f);
    return u == 0x7F800000u || u == 0xFF800000u;
}
inline bool my_isnanorinf(float f) { return (std::bit_cast<u32>(f) & 0x7F800000u) == 0x7F800000u; }

inline int is_even(float d) {
    float int_part;
    std::modf(d / 2.0f, &int_part);
    return 2.0f * int_part == d;
}

// Rounds .5 to the closest even number (PPSSPP Common/Math/math_util.h).
inline double round_ieee_754(double d) {
    const float i = static_cast<float>(std::floor(d));
    d -= i;
    if (d < 0.5f) return i;
    if (d > 0.5f) return i + 1.0f;
    if (is_even(i)) return i;
    return i + 1.0f;
}

inline s32 SaturatedFloatToInt(double d) {
    if (d >= 2147483648.0) return 0x7FFFFFFF;
    if (d <= -2147483648.0) return static_cast<s32>(0x80000000u);
    if (d != d) return 0x7FFFFFFF;
    return static_cast<s32>(d);
}

// With fcr31.FS set, a denormal input to a float->int conversion counts as zero
// (cpu/fpu/denormal). Done explicitly instead of relying on the host's DAZ.
inline float FlushForConversion(const MIPSState *mips, float f) {
    u32 bits = std::bit_cast<u32>(f);
    if ((mips->fcr31 & 0x01000000u) != 0u && (bits & 0x7F800000u) == 0u) {
        bits &= 0x80000000u;
        f = std::bit_cast<float>(bits);
    }
    return f;
}

} // namespace anypsp::cpu
