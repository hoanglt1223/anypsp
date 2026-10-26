// SPDX-License-Identifier: GPL-2.0-or-later
//
// Inline versions of interpreter instructions for generated code. Each one is
// a template on the instruction word, so register lanes, vector sizes and
// operation selectors are compile-time constants and the lane loops unroll
// (and vectorise) in the generated unit.
//
// The VFPU ops implement only the case without pending prefixes (S and T
// prefixes 0xE4, no D prefix), which is what nearly all of them run with. Any
// other prefix state runs the reference interpreter for that instruction, so
// the semantics stay exactly the ones in interp_vfpu.cpp (adapted from
// PPSSPP's InterpreterVFPU.cpp, GPL-2.0-or-later); tests/cpu checks each op
// against it from random states.

#pragma once

#include "cpu/interp.h"
#include "cpu/vfpu_arith.h"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/runtime.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace anypsp::cpu {

// Defined by the interpreter (vfpu_utils.cpp, interp.cpp).
float vfpu_dot(const float a[4], const float b[4]);
float vfpu_sin(float);
float vfpu_cos(float);
void vfpu_sincos(float, float &, float &);
float vfpu_asin(float);
float vfpu_sqrt(float);
float vfpu_rsqrt(float);
float vfpu_exp2(float);
float vfpu_rexp2(float);
float vfpu_log2(float);
float vfpu_rcp(float);
float vfpu_h2f_float(std::uint16_t);
extern const float cst_constants[32];
// One lane of vf2i: s * 2^scale converted with the rounding mode (16..19 = n, z, u, d), saturated.
std::int32_t vfpu_f2i(float s, int scale, int mode);
// The ll/sc link (interp.cpp).
extern std::uint32_t g_llBit;

namespace gen {

using u32 = std::uint32_t;
using Ctx = psprecomp::AllegrexContext;
using Runtime = psprecomp::Runtime;

// Forced inline when optimising, where stack slots of the inlined ops are shared. Without
// optimisation (the -O0 / "d" runner builds) every inlined op would keep its own arrays in the
// generated function's frame, which reached a megabyte and overflowed the stack across chained
// calls; there the ops stay ordinary calls.
#if (defined(__GNUC__) || defined(__clang__)) && !defined(__OPTIMIZE__)
#define ANYPSP_GEN_INLINE inline
#else
#define ANYPSP_GEN_INLINE PSPRECOMP_CONTEXT_FORCEINLINE
#endif

// Lanes of a vector register, as in GetVectorRegs / ReadVector.
constexpr u32 vec_size(u32 op) { return ((op >> 7) & 1u) + ((op >> 14) & 2u) + 1u; }

constexpr std::array<std::uint8_t, 4> vector_lanes(u32 reg, u32 n) {
    std::array<std::uint8_t, 4> lanes{};
    if (n == 1u) {
        lanes[0] = static_cast<std::uint8_t>(Ctx::vfpu_scalar_index(reg & 0x7Fu));
        return lanes;
    }
    const u32 row = n == 3u ? (reg >> 6) & 1u : (reg >> 5) & 2u;
    const u32 mtx = (reg << 2) & 0x70u, col = reg & 3u;
    for (u32 i = 0; i < n; ++i)
        lanes[i] = static_cast<std::uint8_t>(((reg >> 5) & 1u) != 0u ? mtx + col + ((row + i) & 3u) * 4u
                                                                       : mtx + col * 4u + ((row + i) & 3u));
    return lanes;
}

// Elements of a matrix register in ReadMatrix order: [j * 4 + i] for column j, row i.
constexpr std::array<std::uint8_t, 16> matrix_lanes(u32 reg, u32 side) {
    std::array<std::uint8_t, 16> lanes{};
    u32 transpose = (reg >> 5) & 1u, row = 0;
    if (side == 1u) transpose = 0u, row = (reg >> 5) & 3u;
    else if (side == 3u) row = (reg >> 6) & 1u;
    else row = (reg >> 5) & 2u;
    const u32 mtx = (reg >> 2) & 7u, col = reg & 3u;
    for (u32 j = 0; j < side; ++j)
        for (u32 i = 0; i < side; ++i)
            lanes[j * 4u + i] = static_cast<std::uint8_t>(
                mtx * 16u + (transpose != 0u ? ((row + i) & 3u) * 4u + ((col + j) & 3u)
                                             : ((col + j) & 3u) * 4u + ((row + i) & 3u)));
    return lanes;
}

template <u32 Reg, u32 N>
ANYPSP_GEN_INLINE void read_vector(const Ctx &ctx, float *d) {
    constexpr auto lanes = vector_lanes(Reg, N);
    for (u32 i = 0; i < N; ++i) d[i] = ctx.vfpu[lanes[i]];
}

template <u32 Reg, u32 N>
ANYPSP_GEN_INLINE void write_vector(Ctx &ctx, const float *s) {
    constexpr auto lanes = vector_lanes(Reg, N);
    for (u32 i = 0; i < N; ++i) ctx.vfpu[lanes[i]] = s[i];
}

// Lanes as bits, for results that can be the VFPU NaN 0x7F800001 (see vfpu_arith.h).
template <u32 Reg, u32 N>
ANYPSP_GEN_INLINE void read_vector_bits(const Ctx &ctx, u32 *d) {
    constexpr auto lanes = vector_lanes(Reg, N);
    for (u32 i = 0; i < N; ++i) std::memcpy(&d[i], &ctx.vfpu[lanes[i]], sizeof(u32));
}

template <u32 Reg, u32 N>
ANYPSP_GEN_INLINE void write_vector_bits(Ctx &ctx, const u32 *s) {
    constexpr auto lanes = vector_lanes(Reg, N);
    for (u32 i = 0; i < N; ++i) std::memcpy(&ctx.vfpu[lanes[i]], &s[i], sizeof(u32));
}

template <u32 Reg, u32 Side>
ANYPSP_GEN_INLINE void read_matrix(const Ctx &ctx, float *d) {
    constexpr auto lanes = matrix_lanes(Reg, Side);
    for (u32 j = 0; j < Side; ++j)
        for (u32 i = 0; i < Side; ++i) d[j * 4u + i] = ctx.vfpu[lanes[j * 4u + i]];
}

template <u32 Reg, u32 Side>
ANYPSP_GEN_INLINE void write_matrix(Ctx &ctx, const float *s) {
    constexpr auto lanes = matrix_lanes(Reg, Side);
    for (u32 j = 0; j < Side; ++j)
        for (u32 i = 0; i < Side; ++i) ctx.vfpu[lanes[j * 4u + i]] = s[j * 4u + i];
}

ANYPSP_GEN_INLINE bool no_prefixes(const Ctx &ctx) {
    return ctx.vfpu_ctrl[0] == 0xE4u && ctx.vfpu_ctrl[1] == 0xE4u && ctx.vfpu_ctrl[2] == 0u;
}

ANYPSP_GEN_INLINE u32 bits(float f) { return std::bit_cast<u32>(f); }
ANYPSP_GEN_INLINE float from_bits(u32 u) { return std::bit_cast<float>(u); }

ANYPSP_GEN_INLINE bool is_nan(float f) { return (bits(f) & 0x7FFFFFFFu) > 0x7F800000u; }

// The result rule of vdot, vmmul and vtfm: one NaN encoding, denormals flushed to signed zero.
ANYPSP_GEN_INLINE float dot_result(float f) {
    u32 u = bits(f);
    if (is_nan(f)) u = 0x7F800001u;
    else if ((u & 0x7F800000u) == 0u) u &= 0xFF800000u;
    return from_bits(u);
}

// Runs the instruction in the interpreter; false when it stopped the guest.
#define ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, op)              \
    if (!no_prefixes(ctx)) [[unlikely]]                              \
        return ::anypsp::cpu::execute(rt, ctx, pc, op)

// vadd, vsub, vdiv, vmul.
template <u32 Op>
ANYPSP_GEN_INLINE bool vec3(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 n = vec_size(Op);
    constexpr u32 kind = (Op >> 26) == 25u ? 8u : (Op >> 23) & 7u;
    static_assert(kind == 0u || kind == 1u || kind == 7u || kind == 8u);
    u32 s[4], t[4], d[4];
    read_vector_bits<(Op >> 8) & 0x7Fu, n>(ctx, s);
    read_vector_bits<(Op >> 16) & 0x7Fu, n>(ctx, t);
    for (u32 i = 0; i < n; ++i) {
        if constexpr (kind == 0u) d[i] = vfpu_add_bits(s[i], t[i]);
        else if constexpr (kind == 1u) d[i] = vfpu_sub_bits(s[i], t[i]);
        else if constexpr (kind == 7u) d[i] = vfpu_div_bits(s[i], t[i]);
        else d[i] = vfpu_mul_bits(s[i], t[i]);
    }
    write_vector_bits<Op & 0x7Fu, n>(ctx, d);
    return true;
}

// vmov, vabs, vneg, vsat0, vsat1, vrcp, vrsq, vsin, vcos, vexp2, vlog2, vsqrt, vasin, vnrcp, vnsin, vrexp2.
template <u32 Op>
ANYPSP_GEN_INLINE bool vv2(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 n = vec_size(Op);
    constexpr u32 kind = (Op >> 16) & 31u;
    float s[4], d[4];
    read_vector<(Op >> 8) & 0x7Fu, n>(ctx, s);
    for (u32 i = 0; i < n; ++i) {
        if constexpr (kind == 0u) d[i] = s[i];
        else if constexpr (kind == 1u) d[i] = from_bits(bits(s[i]) & 0x7FFFFFFFu);
        else if constexpr (kind == 2u) d[i] = from_bits(bits(s[i]) ^ 0x80000000u);
        else if constexpr (kind == 4u) d[i] = s[i] <= 0 ? 0.0f : (s[i] > 1.0f ? 1.0f : s[i]);
        else if constexpr (kind == 5u) d[i] = s[i] < -1.0f ? -1.0f : (s[i] > 1.0f ? 1.0f : s[i]);
        else if constexpr (kind == 16u) d[i] = vfpu_rcp(s[i]);
        else if constexpr (kind == 17u) d[i] = vfpu_rsqrt(s[i]);
        else if constexpr (kind == 18u) d[i] = vfpu_sin(s[i]);
        else if constexpr (kind == 19u) d[i] = vfpu_cos(s[i]);
        else if constexpr (kind == 20u) d[i] = vfpu_exp2(s[i]);
        else if constexpr (kind == 21u) d[i] = vfpu_log2(s[i]);
        else if constexpr (kind == 22u) d[i] = vfpu_sqrt(s[i]);
        else if constexpr (kind == 23u) d[i] = vfpu_asin(s[i]);
        else if constexpr (kind == 24u) d[i] = -vfpu_rcp(s[i]);
        else if constexpr (kind == 26u) d[i] = -vfpu_sin(s[i]);
        else {
            static_assert(kind == 28u);
            d[i] = vfpu_rexp2(s[i]);
        }
    }
    write_vector<Op & 0x7Fu, n>(ctx, d);
    return true;
}

// vdot, vhdp, vfad, vavg: a dot product written to one lane.
template <u32 Op>
ANYPSP_GEN_INLINE bool dot(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 n = vec_size(Op);
    constexpr u32 major = Op >> 26;
    float s[4]{}, t[4]{};
    read_vector<(Op >> 8) & 0x7Fu, n>(ctx, s);
    float d;
    if constexpr (major == 0x19u) { // vdot, vhdp
        read_vector<(Op >> 16) & 0x7Fu, n>(ctx, t);
        if constexpr (((Op >> 23) & 7u) == 1u) { // vdot
            d = dot_result(vfpu_dot(s, t));
        } else { // vhdp: the last S lane is the constant 1
            s[n - 1u] = 1.0f;
            const float sum = vfpu_dot(s, t);
            d = is_nan(sum) ? std::fabs(sum) : sum;
        }
    } else { // vfad, vavg: T is all 1, or 1/n
        constexpr float c = ((Op >> 16) & 31u) == 6u ? 1.0f
                          : n == 1u                  ? 0.0f
                          : n == 2u                  ? 0.5f
                          : n == 3u                  ? 1.0f / 3.0f
                                                     : 0.25f;
        for (float &lane : t) lane = c;
        d = vfpu_dot(s, t);
    }
    write_vector<Op & 0x7Fu, 1u>(ctx, &d);
    return true;
}

// vscl: S times the scalar T.
template <u32 Op>
ANYPSP_GEN_INLINE bool vscl(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 n = vec_size(Op);
    float s[4], d[4], t;
    read_vector<(Op >> 8) & 0x7Fu, n>(ctx, s);
    read_vector<(Op >> 16) & 0x7Fu, 1u>(ctx, &t);
    for (u32 i = 0; i < n; ++i) d[i] = vfpu_mul(s[i], t);
    write_vector<Op & 0x7Fu, n>(ctx, d);
    return true;
}

// vmin, vmax (vfpu_minmax_key order), vsge, vslt, vscmp.
template <u32 Op>
ANYPSP_GEN_INLINE bool compare3(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 n = vec_size(Op);
    constexpr u32 kind = (Op >> 23) & 7u;
    float s[4], t[4], d[4];
    read_vector<(Op >> 8) & 0x7Fu, n>(ctx, s);
    read_vector<(Op >> 16) & 0x7Fu, n>(ctx, t);
    const auto key = [](u32 x) -> std::int32_t {
        if ((x & 0x7F800000u) == 0u) return 0;
        return static_cast<std::int32_t>(x ^ (static_cast<u32>(static_cast<std::int32_t>(x) >> 31) >> 1));
    };
    for (u32 i = 0; i < n; ++i) {
        const u32 a = bits(s[i]), b = bits(t[i]);
        if constexpr (kind == 2u) d[i] = from_bits(key(a) < key(b) ? a : b);       // vmin
        else if constexpr (kind == 3u) d[i] = from_bits(key(a) > key(b) ? a : b);  // vmax
        else if constexpr (kind == 6u) d[i] = is_nan(s[i]) || is_nan(t[i]) ? 0.0f : (s[i] >= t[i] ? 1.0f : 0.0f);
        else if constexpr (kind == 7u) d[i] = is_nan(s[i]) || is_nan(t[i]) ? 0.0f : (s[i] < t[i] ? 1.0f : 0.0f);
        else {
            static_assert(kind == 5u);  // vscmp
            const float diff = s[i] - t[i];
            if (is_nan(diff)) {
                const int sm = static_cast<int>(a & 0x7FFFFFFFu), tm = static_cast<int>(b & 0x7FFFFFFFu);
                const int c = (static_cast<std::int32_t>(a) < 0 ? -sm : sm) - (static_cast<std::int32_t>(b) < 0 ? -tm : tm);
                d[i] = static_cast<float>((0 < c) - (c < 0));
            } else {
                d[i] = static_cast<float>((0.0f < diff) - (diff < 0.0f));
            }
        }
    }
    write_vector<Op & 0x7Fu, n>(ctx, d);
    return true;
}

// vcmp: sets CC bits 0..n-1, 4 (any) and 5 (all).
template <u32 Op>
ANYPSP_GEN_INLINE bool vcmp(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 n = vec_size(Op);
    constexpr u32 cond = Op & 15u;
    float s[4], t[4];
    read_vector<(Op >> 8) & 0x7Fu, n>(ctx, s);
    read_vector<(Op >> 16) & 0x7Fu, n>(ctx, t);
    u32 cc = 0, any = 0, all = 1;
    for (u32 i = 0; i < n; ++i) {
        const u32 e = bits(s[i]) & 0x7F800000u, m = bits(s[i]) & 0x7FFFFFu;
        bool c;
        if constexpr (cond == 0u) c = false;
        else if constexpr (cond == 1u) c = s[i] == t[i];
        else if constexpr (cond == 2u) c = s[i] < t[i];
        else if constexpr (cond == 3u) c = s[i] <= t[i];
        else if constexpr (cond == 4u) c = true;
        else if constexpr (cond == 5u) c = s[i] != t[i];
        else if constexpr (cond == 6u) c = s[i] >= t[i];
        else if constexpr (cond == 7u) c = s[i] > t[i];
        else if constexpr (cond == 8u) c = s[i] == 0.0f || s[i] == -0.0f;
        else if constexpr (cond == 9u) c = e == 0x7F800000u && m != 0u;
        else if constexpr (cond == 10u) c = e == 0x7F800000u && m == 0u;
        else if constexpr (cond == 11u) c = e == 0x7F800000u;
        else if constexpr (cond == 12u) c = s[i] != 0;
        else if constexpr (cond == 13u) c = !(e == 0x7F800000u && m != 0u);
        else if constexpr (cond == 14u) c = !(e == 0x7F800000u && m == 0u);
        else c = e != 0x7F800000u;
        cc |= static_cast<u32>(c) << i;
        any |= static_cast<u32>(c);
        all &= static_cast<u32>(c);
    }
    constexpr u32 affected = 0x30u | ((1u << n) - 1u);
    ctx.vfpu_ctrl[3] = (ctx.vfpu_ctrl[3] & ~affected) | ((cc | (any << 4) | (all << 5)) & affected);
    return true;
}

// vcmov: D keeps its value or takes S, by one CC bit or (imm 6) per lane.
template <u32 Op>
ANYPSP_GEN_INLINE bool vcmov(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 n = vec_size(Op);
    constexpr u32 tf = (Op >> 19) & 1u, imm3 = (Op >> 16) & 7u;
    float s[4], d[4];
    read_vector<(Op >> 8) & 0x7Fu, n>(ctx, s);
    read_vector<Op & 0x7Fu, n>(ctx, d);
    const u32 cc = ctx.vfpu_ctrl[3];
    if constexpr (imm3 == 6u) {
        for (u32 i = 0; i < n; ++i)
            if (((cc >> i) & 1u) == (tf == 0u ? 1u : 0u)) d[i] = s[i];
    } else {
        if (((cc >> imm3) & 1u) == (tf == 0u ? 1u : 0u))
            for (u32 i = 0; i < n; ++i) d[i] = s[i];
    }
    write_vector<Op & 0x7Fu, n>(ctx, d);
    return true;
}

// vcst, vidt, vzero, vone: constant vectors.
template <u32 Op>
ANYPSP_GEN_INLINE bool vcst(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 n = vec_size(Op);
    float d[4];
    const float c = cst_constants[(Op >> 16) & 31u];
    for (u32 i = 0; i < n; ++i) d[i] = c;
    write_vector<Op & 0x7Fu, n>(ctx, d);
    return true;
}

template <u32 Op>
ANYPSP_GEN_INLINE bool vidt(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 n = vec_size(Op), mask = n >= 3u ? 3u : 1u, off = Op & mask;
    float d[4];
    for (u32 i = 0; i < n; ++i) d[i] = off == (i & mask) ? 1.0f : 0.0f;
    write_vector<Op & 0x7Fu, n>(ctx, d);
    return true;
}

template <u32 Op, bool One>
ANYPSP_GEN_INLINE bool vector_init(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 n = vec_size(Op);
    float d[4];
    for (u32 i = 0; i < n; ++i) d[i] = One ? 1.0f : 0.0f;
    write_vector<Op & 0x7Fu, n>(ctx, d);
    return true;
}

// vmidt, vmzero, vmone.
template <u32 Op>
ANYPSP_GEN_INLINE bool matrix_init(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 side = vec_size(Op), kind = (Op >> 16) & 0xFu;
    float m[16];
    for (u32 j = 0; j < side; ++j)
        for (u32 i = 0; i < side; ++i) m[j * 4u + i] = kind == 7u || (kind == 3u && i == j) ? 1.0f : 0.0f;
    write_matrix<Op & 0x7Fu, side>(ctx, m);
    return true;
}

// vmmov.
template <u32 Op>
ANYPSP_GEN_INLINE bool vmmov(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 side = vec_size(Op);
    float m[16];
    read_matrix<(Op >> 8) & 0x7Fu, side>(ctx, m);
    write_matrix<Op & 0x7Fu, side>(ctx, m);
    return true;
}

// vmmul: D[a][b] = dot(row b of S, row a of T), with the vdot result rule.
template <u32 Op>
ANYPSP_GEN_INLINE bool vmmul(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 side = vec_size(Op);
    float s[16]{}, t[16]{}, d[16];
    read_matrix<(Op >> 8) & 0x7Fu, side>(ctx, s);
    read_matrix<(Op >> 16) & 0x7Fu, side>(ctx, t);
    for (u32 a = 0; a < side; ++a)
        for (u32 b = 0; b < side; ++b) d[a * 4u + b] = dot_result(vfpu_dot(&s[b * 4u], &t[a * 4u]));
    write_matrix<Op & 0x7Fu, side>(ctx, d);
    return true;
}

// vmscl: every element of S times the scalar T.
template <u32 Op>
ANYPSP_GEN_INLINE bool vmscl(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 side = vec_size(Op);
    float s[16], d[16], t;
    read_matrix<(Op >> 8) & 0x7Fu, side>(ctx, s);
    read_vector<(Op >> 16) & 0x7Fu, 1u>(ctx, &t);
    for (u32 j = 0; j < side; ++j)
        for (u32 i = 0; i < side; ++i) d[j * 4u + i] = vfpu_mul(s[j * 4u + i], t);
    write_matrix<Op & 0x7Fu, side>(ctx, d);
    return true;
}

// vtfm2/3/4 and vhtfm2/3/4: matrix times vector, the homogeneous forms with an implicit 1.
template <u32 Op>
ANYPSP_GEN_INLINE bool vtfm(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 ins = (Op >> 23) & 3u, n = vec_size(Op), tn = n < ins + 1u ? n : ins + 1u;
    float s[16]{}, t[4]{}, t2[4], d[4];
    read_matrix<(Op >> 8) & 0x7Fu, ins + 1u>(ctx, s);
    read_vector<(Op >> 16) & 0x7Fu, ins + 1u>(ctx, t);
    for (u32 i = 0; i < 4u; ++i) t2[i] = i < tn ? t[i] : (i == ins ? 1.0f : 0.0f);
    for (u32 i = 0; i <= ins; ++i) d[i] = dot_result(vfpu_dot(&s[i * 4u], t2));
    write_vector<Op & 0x7Fu, ins + 1u>(ctx, d);
    return true;
}

// vrot: one row of a rotation matrix, sine and cosine of S in VFPU angle units.
template <u32 Op>
ANYPSP_GEN_INLINE bool vrot(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 n = vec_size(Op), vd = Op & 0x7Fu, vs = (Op >> 8) & 0x7Fu, imm = (Op >> 16) & 31u;
    constexpr u32 sine_lane = (imm >> 2) & 3u, cosine_lane = imm & 3u;
    // When D shares S's matrix and one of its lanes is S, the cosine is taken of that lane's new value.
    constexpr int source_in_d = [] {
        if (((vd >> 2) & 7u) != ((vs >> 2) & 7u)) return -1;
        // Lanes as register numbers, the way GetVectorRegs names them.
        const u32 mtx = (vd >> 2) & 7u, col = vd & 3u;
        u32 row = 0, length = n, transpose = (vd >> 5) & 1u;
        if (n == 1u) transpose = 0u, row = (vd >> 5) & 3u;
        else if (n == 3u) row = (vd >> 6) & 1u;
        else row = (vd >> 5) & 2u;
        for (u32 i = 0; i < length; ++i) {
            const u32 index = mtx * 4u + (transpose != 0u ? ((row + i) & 3u) + col * 32u : col + ((row + i) & 3u) * 32u);
            if (index == vs) return static_cast<int>(i);
        }
        return -1;
    }();
    float source, sine, cosine;
    read_vector<vs, 1u>(ctx, &source);
    vfpu_sincos(source, sine, cosine);
    if constexpr ((imm & 0x10u) != 0u) sine = -sine;
    float d[4]{};
    if constexpr (sine_lane == cosine_lane) {
        for (float &lane : d) lane = sine;
    } else {
        d[sine_lane] = sine;
    }
    if constexpr (source_in_d >= 0) d[cosine_lane] = vfpu_cos(d[source_in_d]);
    else d[cosine_lane] = cosine;
    write_vector<vd, n>(ctx, d);
    return true;
}

// vcrsp.t (cross product) and vqmul.q (quaternion product); the pair and single forms give zeros.
template <u32 Op>
ANYPSP_GEN_INLINE bool cross_quat(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 n = vec_size(Op);
    float s[4]{}, t[4]{}, d[4]{};
    read_vector<(Op >> 8) & 0x7Fu, n>(ctx, s);
    read_vector<(Op >> 16) & 0x7Fu, n>(ctx, t);
    if constexpr (n == 3u) {
        const float t0[4] = {0.0f, t[2], -t[1], 0.0f}, t1[4] = {-t[2], 0.0f, t[0], 0.0f};
        const float t2[4] = {t[1], -t[0], t[3], t[2]};
        d[0] = vfpu_dot(s, t0);
        d[1] = vfpu_dot(s, t1);
        d[2] = vfpu_dot(s, t2);
    } else if constexpr (n == 4u) {
        const float t0[4] = {t[3], t[2], -t[1], t[0]}, t1[4] = {-t[2], t[3], t[0], t[1]};
        const float t2[4] = {t[1], -t[0], t[3], t[2]}, t3[4] = {-t[0], -t[1], -t[2], t[3]};
        d[0] = vfpu_dot(s, t0);
        d[1] = vfpu_dot(s, t1);
        d[2] = vfpu_dot(s, t2);
        d[3] = vfpu_dot(s, t3);
    } else if constexpr (n == 2u) {
        d[1] = s[2] * t[0];  // s[2] is zero: no lane of a pair reaches it without a prefix
    }
    write_vector<Op & 0x7Fu, n>(ctx, d);
    return true;
}

// vocp: 1 - S, a NaN made positive.
template <u32 Op>
ANYPSP_GEN_INLINE bool vocp(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 n = vec_size(Op);
    float s[4], d[4];
    read_vector<(Op >> 8) & 0x7Fu, n>(ctx, s);
    for (u32 i = 0; i < n; ++i) {
        const float negated = from_bits(bits(s[i]) ^ 0x80000000u);
        d[i] = is_nan(negated) ? std::fabs(negated) : 1.0f + negated;
    }
    write_vector<Op & 0x7Fu, n>(ctx, d);
    return true;
}

// vsgn: -1, 0 or 1 by sign; NaN by its sign bit, denormals as zero.
template <u32 Op>
ANYPSP_GEN_INLINE bool vsgn(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 n = vec_size(Op);
    float s[4], d[4];
    read_vector<(Op >> 8) & 0x7Fu, n>(ctx, s);
    for (u32 i = 0; i < n; ++i) {
        const u32 value = bits(is_nan(s[i]) ? s[i] : s[i] - 0.0f);
        d[i] = (value & 0x7F800000u) == 0u ? 0.0f : ((value >> 31) == 0u ? 1.0f : -1.0f);
    }
    write_vector<Op & 0x7Fu, n>(ctx, d);
    return true;
}

// vdet: s.x * t.y - s.y * t.x, through the VFPU dot product.
template <u32 Op>
ANYPSP_GEN_INLINE bool vdet(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 n = vec_size(Op);
    float s[4]{}, t[4]{};
    read_vector<(Op >> 8) & 0x7Fu, n>(ctx, s);
    read_vector<(Op >> 16) & 0x7Fu, n>(ctx, t);
    const float swapped[4] = {t[1], t[0], t[2], t[3]};
    s[1] = -s[1];
    const float d = vfpu_dot(s, swapped);
    write_vector<Op & 0x7Fu, 1u>(ctx, &d);
    return true;
}

// vf2in, vf2iz, vf2iu, vf2id.
template <u32 Op>
ANYPSP_GEN_INLINE bool vf2i(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 n = vec_size(Op);
    float s[4], d[4];
    read_vector<(Op >> 8) & 0x7Fu, n>(ctx, s);
    for (u32 i = 0; i < n; ++i)
        d[i] = from_bits(static_cast<u32>(vfpu_f2i(s[i], static_cast<int>((Op >> 16) & 31u), static_cast<int>((Op >> 21) & 31u))));
    write_vector<Op & 0x7Fu, n>(ctx, d);
    return true;
}

// vi2f: integers to floats, scaled by 2^-imm.
template <u32 Op>
ANYPSP_GEN_INLINE bool vi2f(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 n = vec_size(Op);
    const float mult = 1.0f / static_cast<float>(1UL << ((Op >> 16) & 31u));
    float s[4], d[4];
    read_vector<(Op >> 8) & 0x7Fu, n>(ctx, s);
    for (u32 i = 0; i < n; ++i) d[i] = static_cast<float>(static_cast<std::int32_t>(bits(s[i]))) * mult;
    write_vector<Op & 0x7Fu, n>(ctx, d);
    return true;
}

// vcrs: half a cross product, S.yzx * T.zxy (lanes past the vector's size read as zero).
template <u32 Op>
ANYPSP_GEN_INLINE bool vcrs(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    constexpr u32 n = vec_size(Op);
    float s[4]{}, t[4]{}, sw[4]{}, tw[4]{}, d[4];
    read_vector<(Op >> 8) & 0x7Fu, n>(ctx, s);
    read_vector<(Op >> 16) & 0x7Fu, n>(ctx, t);
    constexpr u32 s_lane[4] = {1u, 2u, 0u, 3u}, t_lane[4] = {2u, 0u, 1u, 3u};
    for (u32 i = 0; i < 4u; ++i) {
        sw[i] = i < n ? (s_lane[i] < n ? s[s_lane[i]] : 0.0f) : s[i];
        tw[i] = i < n ? (t_lane[i] < n ? t[t_lane[i]] : 0.0f) : t[i];
        d[i] = vfpu_mul(sw[i], tw[i]);
    }
    write_vector<Op & 0x7Fu, n>(ctx, d);
    return true;
}

// lvl.q, lvr.q, svl.q, svr.q: the part of a quad on one side of an unaligned address. They take
// no prefixes; a bad address runs the interpreter, which reports it.
template <u32 Op>
ANYPSP_GEN_INLINE bool vquad_partial(Runtime &rt, Ctx &ctx, u32 pc) {
    constexpr u32 vt = ((Op >> 16) & 0x1Fu) | ((Op & 1u) << 5);
    const u32 addr = ctx.gpr[(Op >> 21) & 31u] + static_cast<u32>(static_cast<std::int32_t>(static_cast<std::int16_t>(Op & 0xFFFCu)));
    auto &memory = rt.memory();
    if ((addr & 3u) != 0u || !memory.contains(addr, 4u)) [[unlikely]]
        return ::anypsp::cpu::execute(rt, ctx, pc, Op);
    const u32 offset = (addr >> 2) & 3u;
    float q[4];
    read_vector<vt, 4u>(ctx, q);
    if constexpr ((Op >> 26) == 53u) {
        if constexpr ((Op & 2u) == 0u)
            for (u32 i = 0; i < offset + 1u; ++i) q[3u - i] = from_bits(memory.aot_load32(addr - 4u * i));
        else
            for (u32 i = 0; i < 4u - offset; ++i) q[i] = from_bits(memory.aot_load32(addr + 4u * i));
        write_vector<vt, 4u>(ctx, q);
    } else {
        if constexpr ((Op & 2u) == 0u)
            for (u32 i = 0; i < offset + 1u; ++i) memory.aot_store32(addr - 4u * i, bits(q[3u - i]));
        else
            for (u32 i = 0; i < 4u - offset; ++i) memory.aot_store32(addr + 4u * i, bits(q[i]));
    }
    return true;
}

// viim.s, vfim.s: an integer or half-float immediate to one lane.
template <u32 Op>
ANYPSP_GEN_INLINE bool viim(Runtime &rt, Ctx &ctx, u32 pc) {
    ANYPSP_GEN_PLAIN_OR_INTERPRET(rt, ctx, pc, Op);
    float f;
    if constexpr (((Op >> 23) & 7u) == 6u) f = static_cast<float>(static_cast<std::int16_t>(Op & 0xFFFFu));
    else f = vfpu_h2f_float(static_cast<std::uint16_t>(Op & 0xFFFFu));
    write_vector<(Op >> 16) & 0x7Fu, 1u>(ctx, &f);
    return true;
}

#undef ANYPSP_GEN_PLAIN_OR_INTERPRET

// ll and sc. The link is the interpreter's, which leaving the guest (syscalls, interrupts) clears.
template <u32 Op>
ANYPSP_GEN_INLINE void load_linked(Runtime &rt, Ctx &ctx, u32 pc) {
    constexpr u32 rs = (Op >> 21) & 31u, rt_reg = (Op >> 16) & 31u;
    const u32 addr = ctx.gpr[rs] + static_cast<u32>(static_cast<std::int32_t>(static_cast<std::int16_t>(Op & 0xFFFFu)));
    auto &memory = rt.memory();
    const bool valid = (addr & 3u) == 0u && memory.contains(addr, 4u);
    if constexpr ((Op >> 26) == 48u) {  // ll
        if (!valid) [[unlikely]] {
            (void)::anypsp::cpu::execute(rt, ctx, pc, Op);  // reports the bad address and does the rest
            return;
        }
        if constexpr (rt_reg != 0u) ctx.gpr[rt_reg] = memory.aot_load32(addr);
        g_llBit = 1u;
    } else {  // sc
        if (!valid) [[unlikely]] {
            (void)::anypsp::cpu::execute(rt, ctx, pc, Op);
            return;
        }
        const u32 linked = g_llBit;
        if (linked != 0u) memory.aot_store32(addr, ctx.gpr[rt_reg]);
        if constexpr (rt_reg != 0u) ctx.gpr[rt_reg] = linked != 0u ? 1u : 0u;
    }
}

// madd, maddu, msub, msubu: HI:LO plus or minus the product.
template <u32 Op>
ANYPSP_GEN_INLINE void multiply_accumulate(Ctx &ctx) {
    constexpr u32 rs = (Op >> 21) & 31u, rt = (Op >> 16) & 31u, function = Op & 63u;
    const std::uint64_t acc = static_cast<std::uint64_t>(ctx.lo) | (static_cast<std::uint64_t>(ctx.hi) << 32);
    std::uint64_t product;
    if constexpr (function == 28u || function == 46u)
        product = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(ctx.gpr[rs])) *
                                             static_cast<std::int64_t>(static_cast<std::int32_t>(ctx.gpr[rt])));
    else
        product = static_cast<std::uint64_t>(ctx.gpr[rs]) * static_cast<std::uint64_t>(ctx.gpr[rt]);
    const std::uint64_t result = function == 28u || function == 29u ? acc + product : acc - product;
    ctx.lo = static_cast<u32>(result);
    ctx.hi = static_cast<u32>(result >> 32);
}

#undef ANYPSP_GEN_INLINE

} // namespace gen
} // namespace anypsp::cpu
