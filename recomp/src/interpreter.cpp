#include "psprecomp/interpreter.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/decoder.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace psprecomp {
namespace {

using K = OpcodeKind;

constexpr std::uint32_t kBudget = 1u << 20u;
constexpr std::uint32_t kMaxBlockBytes = 4096u;

struct DynamicBlock {
    std::vector<std::uint32_t> words;
    Runtime::RecompiledFunction function{};
};

std::map<std::pair<std::uint32_t, std::uint64_t>, Runtime::RecompiledFunction> &registered_blocks() {
    static std::map<std::pair<std::uint32_t, std::uint64_t>, Runtime::RecompiledFunction> blocks;
    return blocks;
}

std::unordered_map<std::uint32_t, DynamicBlock> g_blocks;
std::set<std::pair<std::uint32_t, std::uint64_t>> g_captured;
DynamicCodeCapture g_capture = nullptr;
constexpr float kHalfPi = 1.57079632679489661923f;

constexpr std::uint32_t kVfpuConstants[32] = {
    0x00000000u, 0x7F7FFFFFu, 0x3FB504F3u, 0x3F3504F3u, 0x3F906EBAu, 0x3F22F983u, 0x3EA2F983u, 0x3F490FDBu,
    0x3FC90FDBu, 0x40490FDBu, 0x402DF854u, 0x3FB8AA3Bu, 0x3EDE5BD9u, 0x3F317218u, 0x40135D8Eu, 0x40C90FDBu,
    0x3F060A92u, 0x3E9A209Bu, 0x40549A78u, 0x3F5DB3D7u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
};

std::uint32_t vector_length(std::uint32_t word) {
    return (((word >> 7u) & 1u) | (((word >> 15u) & 1u) << 1u)) + 1u;
}

bool is_likely(K kind) {
    return kind == K::Beql || kind == K::Bnel || kind == K::Blezl || kind == K::Bgtzl || kind == K::Bltzl ||
           kind == K::Bgezl || kind == K::Bltzall || kind == K::Bgezall || kind == K::Bc1fl || kind == K::Bc1tl ||
           kind == K::Bvfl || kind == K::Bvtl;
}

bool is_link(K kind) {
    return kind == K::Bltzal || kind == K::Bgezal || kind == K::Bltzall || kind == K::Bgezall;
}

bool branch_taken(const AllegrexContext &ctx, const DecodedInstruction &d) {
    const auto rs = static_cast<std::int32_t>(ctx.gpr[d.rs]);
    switch (d.kind) {
    case K::Beq: case K::Beql: return ctx.gpr[d.rs] == ctx.gpr[d.rt];
    case K::Bne: case K::Bnel: return ctx.gpr[d.rs] != ctx.gpr[d.rt];
    case K::Blez: case K::Blezl: return rs <= 0;
    case K::Bgtz: case K::Bgtzl: return rs > 0;
    case K::Bltz: case K::Bltzl: case K::Bltzal: case K::Bltzall: return rs < 0;
    case K::Bgez: case K::Bgezl: case K::Bgezal: case K::Bgezall: return rs >= 0;
    case K::Bc1f: case K::Bc1fl: return !ctx.fpu_condition();
    case K::Bc1t: case K::Bc1tl: return ctx.fpu_condition();
    case K::Bvf: case K::Bvfl: return ((ctx.vfpu_ctrl[3] >> ((d.word >> 18u) & 7u)) & 1u) == 0u;
    case K::Bvt: case K::Bvtl: return ((ctx.vfpu_ctrl[3] >> ((d.word >> 18u) & 7u)) & 1u) != 0u;
    default: return false;
    }
}

float half_to_float(std::uint32_t half) {
    const std::uint32_t sign = (half & 0x8000u) << 16u;
    const std::uint32_t exponent = (half >> 10u) & 0x1Fu;
    std::uint32_t mantissa = half & 0x03FFu;
    std::uint32_t bits = 0u;
    if (exponent == 0u) {
        if (mantissa == 0u) {
            bits = sign;
        } else {
            std::uint32_t shift = 0u;
            while ((mantissa & 0x0400u) == 0u) {
                mantissa <<= 1u;
                ++shift;
            }
            mantissa &= 0x03FFu;
            bits = sign | ((113u - shift) << 23u) | (mantissa << 13u);
        }
    } else if (exponent == 31u) {
        bits = sign | 0x7F800000u | (mantissa << 13u);
    } else {
        bits = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
    }
    return std::bit_cast<float>(bits);
}

float unary(std::uint32_t operation, float s) {
    switch (operation) {
    case 0u: return s;
    case 1u: return std::fabs(s);
    case 2u: return -s;
    case 4u: return s <= 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
    case 5u: return s < -1.0f ? -1.0f : (s > 1.0f ? 1.0f : s);
    case 16u: return 1.0f / s;
    case 17u: return 1.0f / std::sqrt(s);
    case 18u: return std::sin(s * kHalfPi);
    case 19u: return std::cos(s * kHalfPi);
    case 20u: return std::exp2(s);
    case 21u: return std::log2(s);
    case 22u: return std::fabs(std::sqrt(s));
    case 23u: return std::asin(s) * 0.63661977236758134308f;
    case 24u: return -1.0f / s;
    case 26u: return -std::sin(s * kHalfPi);
    default: return 1.0f / std::exp2(s);
    }
}

bool fpu_compare(const AllegrexContext &ctx, const DecodedInstruction &d) {
    const float fs = ctx.fpr[d.rd], ft = ctx.fpr[d.rt];
    const bool unordered = std::isnan(fs) || std::isnan(ft);
    switch (d.word & 0xFu) {
    case 0u: case 8u: return false;
    case 1u: case 9u: return unordered;
    case 2u: case 10u: return !unordered && fs == ft;
    case 3u: case 11u: return unordered || fs == ft;
    case 4u: case 12u: return fs < ft;
    case 5u: case 13u: return unordered || fs < ft;
    case 6u: case 14u: return fs <= ft;
    default: return unordered || fs <= ft;
    }
}

void vf2i(AllegrexContext &ctx, std::uint32_t w) {
    const std::uint32_t length = vector_length(w), destination = w & 0x7Fu, source = (w >> 8u) & 0x7Fu;
    const std::uint32_t mode = (w >> 21u) & 31u;
    float s[4]{};
    std::int32_t d[4]{};
    ctx.read_vfpu_vector(s, source, length);
    ctx.apply_vfpu_source_prefix(s, length, 0u);
    const float scale = std::ldexp(1.0f, static_cast<int>((w >> 16u) & 31u));
    for (std::uint32_t i = 0; i < length; ++i) {
        if (std::isnan(s[i])) {
            d[i] = std::numeric_limits<std::int32_t>::max();
            continue;
        }
        const double scaled = static_cast<double>(s[i] * scale);
        if (scaled > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
            d[i] = std::numeric_limits<std::int32_t>::max();
        } else if (scaled <= static_cast<double>(std::numeric_limits<std::int32_t>::min())) {
            d[i] = std::numeric_limits<std::int32_t>::min();
        } else {
            double rounded = 0.0;
            switch (mode) {
            case 16u: rounded = AllegrexContext::round_ties_to_even(scaled); break;
            case 17u: rounded = std::trunc(scaled); break;
            case 18u: rounded = std::ceil(scaled); break;
            default: rounded = std::floor(scaled); break;
            }
            d[i] = static_cast<std::int32_t>(rounded);
        }
    }
    const std::uint32_t prefix = ctx.vfpu_ctrl[2];
    for (std::uint32_t i = 0; i < length; ++i)
        if (((prefix >> (8u + i)) & 1u) == 0u)
            ctx.vfpu[AllegrexContext::vfpu_vector_lane_index(destination, length, i)] = std::bit_cast<float>(static_cast<std::uint32_t>(d[i]));
    ctx.eat_vfpu_prefixes();
}

void vtfm(AllegrexContext &ctx, std::uint32_t w) {
    const std::uint32_t side = ((w >> 23u) & 3u) + 1u, input_length = vector_length(w);
    const std::uint32_t destination = w & 0x7Fu, source = (w >> 8u) & 0x7Fu, target = (w >> 16u) & 0x7Fu;
    float matrix[16]{}, raw[4]{}, t[4]{}, result[4]{};
    ctx.read_vfpu_matrix(matrix, source, side);
    ctx.read_vfpu_vector(raw, target, side);
    for (std::uint32_t i = 0; i < 4u; ++i) t[i] = i < input_length ? raw[i] : 0.0f;
    if (side - 1u >= input_length) t[side - 1u] = 1.0f;
    for (std::uint32_t row = 0; row + 1u < side; ++row) {
        float sum = 0.0f;
        for (std::uint32_t column = 0; column < side; ++column) sum += matrix[row * 4u + column] * t[column];
        result[row] = sum;
    }
    float last[4]{matrix[(side - 1u) * 4u + 0u], matrix[(side - 1u) * 4u + 1u], matrix[(side - 1u) * 4u + 2u],
                  matrix[(side - 1u) * 4u + 3u]};
    ctx.apply_vfpu_source_prefix(last, 4u, 0u);
    ctx.apply_vfpu_source_prefix(t, 4u, 1u);
    for (std::uint32_t column = 0; column < 4u; ++column) result[side - 1u] += last[column] * t[column];
    const std::uint32_t prefix = ctx.vfpu_ctrl[2], lane = side - 1u;
    ctx.vfpu_ctrl[2] = ((prefix & (1u << 8u)) << lane) | ((prefix & 3u) << (lane * 2u));
    ctx.write_vfpu_vector_with_destination_prefix(result, destination, side);
}

void vmmul(AllegrexContext &ctx, std::uint32_t w) {
    const std::uint32_t side = vector_length(w);
    float s[16]{}, t[16]{}, d[16]{};
    ctx.read_vfpu_matrix(s, (w >> 8u) & 0x7Fu, side);
    ctx.read_vfpu_matrix(t, (w >> 16u) & 0x7Fu, side);
    for (std::uint32_t a = 0; a < side; ++a) {
        for (std::uint32_t b = 0; b < side; ++b) {
            float sum = 0.0f;
            for (std::uint32_t c = 0; c < side; ++c) sum += s[b * 4u + c] * t[a * 4u + c];
            d[a * 4u + b] = sum;
        }
    }
    ctx.write_vfpu_matrix(d, w & 0x7Fu, side);
    ctx.eat_vfpu_prefixes();
}

bool extra_vfpu(Runtime &rt, AllegrexContext &ctx, std::uint32_t w) {
    const std::uint32_t major = w >> 26u, group = (w >> 21u) & 31u, op = (w >> 16u) & 31u;
    const std::uint32_t length = vector_length(w), destination = w & 0x7Fu, source = (w >> 8u) & 0x7Fu;
    if (major == 0x34u && group == 1u && op <= 3u) {
        ctx.execute_vfpu_vrnd(destination, source, length, op);
        return true;
    }
    if (major == 0x34u && group == 1u && op >= 28u) {
        ctx.execute_vfpu_vi2x(destination, source, length, op & 3u);
        return true;
    }
    if (major == 0x34u && group == 2u && op >= 25u && op <= 27u) {
        ctx.execute_vfpu_vt_color(destination, source, length, op & 3u);
        return true;
    }
    if (major == 0x34u && group == 2u) {
        if (op > 10u || op == 4u || op == 6u || op == 7u) return false;
        float s[4]{}, d[4]{};
        std::uint32_t out_length = length;
        ctx.read_vfpu_vector_with_source_prefix(s, source, length, 0u);
        switch (op) {
        case 0u: d[0] = s[0] < s[1] ? s[0] : s[1]; d[1] = s[0] < s[1] ? s[1] : s[0]; d[2] = s[2] < s[3] ? s[2] : s[3]; d[3] = s[2] < s[3] ? s[3] : s[2]; break;
        case 1u: d[0] = s[0] < s[3] ? s[0] : s[3]; d[1] = s[1] < s[2] ? s[1] : s[2]; d[2] = s[1] < s[2] ? s[2] : s[1]; d[3] = s[0] < s[3] ? s[3] : s[0]; break;
        case 2u: d[0] = s[0] + s[1]; d[1] = s[0] - s[1]; d[2] = s[2] + s[3]; d[3] = s[2] - s[3]; break;
        case 3u: d[0] = s[0] + s[2]; d[1] = s[1] + s[3]; d[2] = s[0] - s[2]; d[3] = s[1] - s[3]; break;
        case 5u:
            for (std::uint32_t i = 0; i < length && i < 2u; ++i) {
                const float a = 1.0f - s[i], b = s[i];
                d[i * 2u] = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
                d[i * 2u + 1u] = b < 0.0f ? 0.0f : (b > 1.0f ? 1.0f : b);
            }
            out_length = length * 2u > 4u ? 4u : length * 2u;
            break;
        case 8u: d[0] = s[0] < s[1] ? s[1] : s[0]; d[1] = s[0] < s[1] ? s[0] : s[1]; d[2] = s[2] < s[3] ? s[3] : s[2]; d[3] = s[2] < s[3] ? s[2] : s[3]; break;
        case 9u: d[0] = s[0] < s[3] ? s[3] : s[0]; d[1] = s[1] < s[2] ? s[2] : s[1]; d[2] = s[1] < s[2] ? s[1] : s[2]; d[3] = s[0] < s[3] ? s[0] : s[3]; break;
        default:
            for (std::uint32_t i = 0; i < length; ++i) d[i] = s[i] > 0.0f ? 1.0f : (s[i] < 0.0f ? -1.0f : 0.0f);
            break;
        }
        ctx.write_vfpu_vector_with_destination_prefix(d, destination, out_length);
        return true;
    }
    if (major == 0x35u || major == 0x3Du) {
        auto &m = rt.memory();
        const std::uint32_t address = ctx.gpr[(w >> 21u) & 31u] + static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(w & 0xFFFCu)));
        const std::uint32_t vector = ((w >> 16u) & 0x1Fu) | ((w & 1u) << 5u);
        const std::uint32_t lane = (address >> 2u) & 3u;
        const bool right = (w & 2u) != 0u;
        float q[4]{};
        ctx.read_vfpu_vector(q, vector, 4u);
        if (major == 0x35u) {
            if (!right)
                for (std::uint32_t i = 0; i <= lane; ++i) q[3u - i] = std::bit_cast<float>(m.aot_load32(address - 4u * i));
            else
                for (std::uint32_t i = 0; i <= 3u - lane; ++i) q[i] = std::bit_cast<float>(m.aot_load32(address + 4u * i));
            ctx.write_vfpu_vector(q, vector, 4u);
        } else if (!right) {
            for (std::uint32_t i = 0; i <= lane; ++i) m.aot_store32(address - 4u * i, std::bit_cast<std::uint32_t>(q[3u - i]));
        } else {
            for (std::uint32_t i = 0; i <= 3u - lane; ++i) m.aot_store32(address + 4u * i, std::bit_cast<std::uint32_t>(q[i]));
        }
        return true;
    }
    return false;
}

bool execute(Runtime &rt, AllegrexContext &ctx, const DecodedInstruction &d, std::uint32_t pc) {
    auto &m = rt.memory();
    const auto imm = static_cast<std::int32_t>(d.immediate);
    const auto uimm = static_cast<std::uint32_t>(static_cast<std::uint16_t>(d.immediate));
    const std::uint32_t s = ctx.gpr[d.rs], t = ctx.gpr[d.rt], w = d.word;
    const std::uint32_t address = s + static_cast<std::uint32_t>(imm);
    const std::uint32_t length = vector_length(w), vd = w & 0x7Fu, vs = (w >> 8u) & 0x7Fu, vt = (w >> 16u) & 0x7Fu;
    switch (d.kind) {
    case K::Nop: case K::Cache: break;
    case K::Sync: m.memory_barrier(); break;
    case K::Addiu: ctx.set_gpr(d.rt, address); break;
    case K::Slti: ctx.set_gpr(d.rt, static_cast<std::int32_t>(s) < imm ? 1u : 0u); break;
    case K::Sltiu: ctx.set_gpr(d.rt, s < static_cast<std::uint32_t>(imm) ? 1u : 0u); break;
    case K::Andi: ctx.set_gpr(d.rt, s & uimm); break;
    case K::Ori: ctx.set_gpr(d.rt, s | uimm); break;
    case K::Xori: ctx.set_gpr(d.rt, s ^ uimm); break;
    case K::Lui: ctx.set_gpr(d.rt, uimm << 16u); break;
    case K::Add:
        if (!ctx.execute_signed_add(d.rd, d.rs, d.rt)) {
            rt.arithmetic_overflow(pc, w);
            return false;
        }
        break;
    case K::Sub:
        if (!ctx.execute_signed_sub(d.rd, d.rs, d.rt)) {
            rt.arithmetic_overflow(pc, w);
            return false;
        }
        break;
    case K::Addu: ctx.set_gpr(d.rd, s + t); break;
    case K::Subu: ctx.set_gpr(d.rd, s - t); break;
    case K::And: ctx.set_gpr(d.rd, s & t); break;
    case K::Or: ctx.set_gpr(d.rd, s | t); break;
    case K::Xor: ctx.set_gpr(d.rd, s ^ t); break;
    case K::Nor: ctx.set_gpr(d.rd, ~(s | t)); break;
    case K::Slt: ctx.set_gpr(d.rd, static_cast<std::int32_t>(s) < static_cast<std::int32_t>(t) ? 1u : 0u); break;
    case K::Sltu: ctx.set_gpr(d.rd, s < t ? 1u : 0u); break;
    case K::Max: ctx.set_gpr(d.rd, static_cast<std::int32_t>(s) > static_cast<std::int32_t>(t) ? s : t); break;
    case K::Min: ctx.set_gpr(d.rd, static_cast<std::int32_t>(s) < static_cast<std::int32_t>(t) ? s : t); break;
    case K::Movz: if (t == 0u) ctx.set_gpr(d.rd, s); break;
    case K::Movn: if (t != 0u) ctx.set_gpr(d.rd, s); break;
    case K::Sll: ctx.set_gpr(d.rd, t << d.sa); break;
    case K::Srl: ctx.set_gpr(d.rd, t >> d.sa); break;
    case K::Sra: ctx.set_gpr(d.rd, static_cast<std::uint32_t>(static_cast<std::int32_t>(t) >> d.sa)); break;
    case K::Rotr: ctx.set_gpr(d.rd, std::rotr(t, static_cast<int>(d.sa))); break;
    case K::Sllv: ctx.set_gpr(d.rd, t << (s & 31u)); break;
    case K::Srlv: ctx.set_gpr(d.rd, t >> (s & 31u)); break;
    case K::Srav: ctx.set_gpr(d.rd, static_cast<std::uint32_t>(static_cast<std::int32_t>(t) >> (s & 31u))); break;
    case K::Rotrv: ctx.set_gpr(d.rd, std::rotr(t, static_cast<int>(s & 31u))); break;
    case K::Clz: ctx.set_gpr(d.rd, static_cast<std::uint32_t>(std::countl_zero(s))); break;
    case K::Clo: ctx.set_gpr(d.rd, static_cast<std::uint32_t>(std::countl_one(s))); break;
    case K::Ext: {
        const std::uint32_t size = d.rd + 1u;
        const std::uint32_t mask = size == 32u ? 0xFFFFFFFFu : ((1u << size) - 1u);
        ctx.set_gpr(d.rt, (s >> d.sa) & mask);
        break;
    }
    case K::Ins: {
        const std::uint32_t size = d.rd >= d.sa ? d.rd - d.sa + 1u : 0u;
        const std::uint32_t mask = size == 32u ? 0xFFFFFFFFu : (size == 0u ? 0u : ((1u << size) - 1u));
        ctx.set_gpr(d.rt, (t & ~(mask << d.sa)) | ((s & mask) << d.sa));
        break;
    }
    case K::Seb: ctx.set_gpr(d.rd, static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(t)))); break;
    case K::Seh: ctx.set_gpr(d.rd, static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(t)))); break;
    case K::Bitrev: {
        std::uint32_t value = t;
        value = ((value >> 1u) & 0x55555555u) | ((value & 0x55555555u) << 1u);
        value = ((value >> 2u) & 0x33333333u) | ((value & 0x33333333u) << 2u);
        value = ((value >> 4u) & 0x0F0F0F0Fu) | ((value & 0x0F0F0F0Fu) << 4u);
        value = ((value >> 8u) & 0x00FF00FFu) | ((value & 0x00FF00FFu) << 8u);
        ctx.set_gpr(d.rd, (value >> 16u) | (value << 16u));
        break;
    }
    case K::Wsbh: ctx.set_gpr(d.rd, ((t & 0x00FF00FFu) << 8u) | ((t & 0xFF00FF00u) >> 8u)); break;
    case K::Wsbw: ctx.set_gpr(d.rd, (t << 24u) | ((t & 0x0000FF00u) << 8u) | ((t & 0x00FF0000u) >> 8u) | (t >> 24u)); break;
    case K::Lw: ctx.set_gpr(d.rt, m.aot_load32(address)); break;
    case K::Lwl: ctx.set_gpr(d.rt, m.aot_load_word_left(address, t)); break;
    case K::Lwr: ctx.set_gpr(d.rt, m.aot_load_word_right(address, t)); break;
    case K::Sw: m.aot_store32(address, t); break;
    case K::Swl: m.aot_store_word_left(address, t); break;
    case K::Swr: m.aot_store_word_right(address, t); break;
    case K::Lh: ctx.set_gpr(d.rt, static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(m.aot_load16(address))))); break;
    case K::Lhu: ctx.set_gpr(d.rt, m.aot_load16(address)); break;
    case K::Sh: m.aot_store16(address, static_cast<std::uint16_t>(t)); break;
    case K::Lb: ctx.set_gpr(d.rt, static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(m.aot_load8(address))))); break;
    case K::Lbu: ctx.set_gpr(d.rt, m.aot_load8(address)); break;
    case K::Sb: m.aot_store8(address, static_cast<std::uint8_t>(t)); break;
    case K::Lwc1: ctx.fpr[d.rt] = std::bit_cast<float>(m.aot_load32(address)); break;
    case K::Swc1: m.aot_store32(address, std::bit_cast<std::uint32_t>(ctx.fpr[d.rt])); break;
    case K::Mfhi: ctx.set_gpr(d.rd, ctx.hi); break;
    case K::Mflo: ctx.set_gpr(d.rd, ctx.lo); break;
    case K::Mthi: ctx.hi = s; break;
    case K::Mtlo: ctx.lo = s; break;
    case K::Mult: {
        const std::int64_t product = static_cast<std::int64_t>(static_cast<std::int32_t>(s)) * static_cast<std::int64_t>(static_cast<std::int32_t>(t));
        ctx.lo = static_cast<std::uint32_t>(product);
        ctx.hi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(product) >> 32u);
        break;
    }
    case K::Multu: {
        const std::uint64_t product = static_cast<std::uint64_t>(s) * static_cast<std::uint64_t>(t);
        ctx.lo = static_cast<std::uint32_t>(product);
        ctx.hi = static_cast<std::uint32_t>(product >> 32u);
        break;
    }
    case K::Div: {
        const auto dividend = static_cast<std::int32_t>(s), divisor = static_cast<std::int32_t>(t);
        if (divisor == 0) {
            ctx.lo = dividend >= 0 ? 0xFFFFFFFFu : 1u;
            ctx.hi = static_cast<std::uint32_t>(dividend);
        } else if (dividend == static_cast<std::int32_t>(0x80000000u) && divisor == -1) {
            ctx.lo = 0x80000000u;
            ctx.hi = 0u;
        } else {
            ctx.lo = static_cast<std::uint32_t>(dividend / divisor);
            ctx.hi = static_cast<std::uint32_t>(dividend % divisor);
        }
        break;
    }
    case K::Divu:
        if (t == 0u) {
            ctx.lo = 0xFFFFFFFFu;
            ctx.hi = s;
        } else {
            ctx.lo = s / t;
            ctx.hi = s % t;
        }
        break;
    case K::Mfc1: ctx.set_gpr(d.rt, std::bit_cast<std::uint32_t>(ctx.fpr[d.rd])); break;
    case K::Mtc1: ctx.fpr[d.rd] = std::bit_cast<float>(t); break;
    case K::Cfc1:
        if (d.rd != 31u) {
            rt.unsupported(pc, w, "unsupported CFC1 control register");
            return false;
        }
        ctx.set_gpr(d.rt, ctx.fcr31);
        break;
    case K::Ctc1:
        if (d.rd != 31u) {
            rt.unsupported(pc, w, "unsupported CTC1 control register");
            return false;
        }
        ctx.fcr31 = t & 0x0181FFFFu;
        break;
    case K::AddS: ctx.fpr[d.sa] = ctx.fpr[d.rd] + ctx.fpr[d.rt]; break;
    case K::SubS: ctx.fpr[d.sa] = ctx.fpr[d.rd] - ctx.fpr[d.rt]; break;
    case K::MulS: {
        const float fs = ctx.fpr[d.rd], ft = ctx.fpr[d.rt];
        if ((std::isinf(fs) && ft == 0.0f) || (std::isinf(ft) && fs == 0.0f)) ctx.set_fpr_bits(d.sa, 0x7FC00000u);
        else ctx.fpr[d.sa] = fs * ft;
        break;
    }
    case K::DivS: ctx.fpr[d.sa] = ctx.fpr[d.rd] / ctx.fpr[d.rt]; break;
    case K::SqrtS: ctx.fpr[d.sa] = std::sqrt(ctx.fpr[d.rd]); break;
    case K::AbsS: ctx.set_fpr_bits(d.sa, ctx.fpr_bits(d.rd) & 0x7FFFFFFFu); break;
    case K::MovS: ctx.set_fpr_bits(d.sa, ctx.fpr_bits(d.rd)); break;
    case K::NegS: ctx.set_fpr_bits(d.sa, ctx.fpr_bits(d.rd) ^ 0x80000000u); break;
    case K::RoundWS: ctx.set_fpr_bits(d.sa, ctx.fpu_float_to_word(ctx.fpr[d.rd], 0u)); break;
    case K::TruncWS: ctx.set_fpr_bits(d.sa, ctx.fpu_float_to_word(ctx.fpr[d.rd], 1u)); break;
    case K::CeilWS: ctx.set_fpr_bits(d.sa, ctx.fpu_float_to_word(ctx.fpr[d.rd], 2u)); break;
    case K::FloorWS: ctx.set_fpr_bits(d.sa, ctx.fpu_float_to_word(ctx.fpr[d.rd], 3u)); break;
    case K::CvtWS: ctx.set_fpr_bits(d.sa, ctx.fpu_float_to_word(ctx.fpr[d.rd], 4u)); break;
    case K::CvtSW: ctx.fpr[d.sa] = static_cast<float>(static_cast<std::int32_t>(ctx.fpr_bits(d.rd))); break;
    case K::FpuCompare: ctx.set_fpu_condition(fpu_compare(ctx, d)); break;
    case K::Vflush:
        if ((w & 0xFFFF0000u) != 0xFFFF0000u) ctx.eat_vfpu_prefixes();
        break;
    case K::Vpfx: {
        const std::uint32_t control = (w >> 24u) & 3u;
        ctx.vfpu_ctrl[control] = control == 2u ? (w & 0x00000FFFu) : (w & 0x000FFFFFu);
        break;
    }
    case K::Viim: {
        const float value[1]{static_cast<float>(static_cast<std::int16_t>(w & 0xFFFFu))};
        ctx.write_vfpu_vector_with_destination_prefix(value, vt, 1u);
        break;
    }
    case K::Vfim: {
        const float value[1]{half_to_float(w & 0xFFFFu)};
        ctx.write_vfpu_vector_with_destination_prefix(value, vt, 1u);
        break;
    }
    case K::Vf2h: ctx.execute_vfpu_vf2h(vd, vs, length); break;
    case K::Vh2f: ctx.execute_vfpu_vh2f(vd, vs, length); break;
    case K::Vf2i: vf2i(ctx, w); break;
    case K::Vi2f: {
        float source[4]{}, result[4]{};
        ctx.read_vfpu_vector(source, vs, length);
        ctx.apply_vfpu_source_prefix(source, length, 0u);
        const float scale = std::ldexp(1.0f, -static_cast<int>((w >> 16u) & 31u));
        for (std::uint32_t i = 0; i < length; ++i)
            result[i] = static_cast<float>(static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(source[i]))) * scale;
        ctx.write_vfpu_vector_with_destination_prefix(result, vd, length);
        break;
    }
    case K::Vx2i: ctx.execute_vfpu_vx2i(vd, vs, length, (w >> 16u) & 3u); break;
    case K::Mtv: ctx.set_vfpu_scalar_bits(w & 0xFFu, t); break;
    case K::Mfv: ctx.set_gpr(d.rt, ctx.vfpu_scalar_bits(w & 0xFFu)); break;
    case K::VmidT: ctx.execute_vfpu_matrix_init(vd, length, 3u); break;
    case K::Vmmov: ctx.execute_vfpu_vmmov(vd, vs, length); break;
    case K::VfpuMatrixInit: ctx.execute_vfpu_matrix_init(vd, length, (w >> 16u) & 15u); break;
    case K::Vidt: {
        float value[4]{};
        value[vd & (length >= 3u ? 3u : 1u)] = 1.0f;
        ctx.write_vfpu_vector_with_destination_prefix(value, vd, length);
        break;
    }
    case K::Vtfm: vtfm(ctx, w); break;
    case K::VfpuVectorInit: {
        const float fill = ((w >> 16u) & 31u) == 7u ? 1.0f : 0.0f;
        const float value[4]{fill, fill, fill, fill};
        ctx.write_vfpu_vector_with_destination_prefix(value, vd, length);
        break;
    }
    case K::Vmmul: vmmul(ctx, w); break;
    case K::Vmscl: ctx.execute_vfpu_vmscl(vd, vs, vt, length); break;
    case K::Vrot: ctx.execute_vfpu_vrot(vd, vs, length, (w >> 16u) & 31u); break;
    case K::Vocp: ctx.execute_vfpu_vocp(vd, vs, length); break;
    case K::VfpuHorizontal: ctx.execute_vfpu_horizontal(vd, vs, length, ((w >> 16u) & 31u) == 7u); break;
    case K::VfpuVec3: {
        const std::uint32_t operation = (w >> 26u) == 0x19u ? 2u : ((w >> 23u) & 7u);
        float a[4]{}, b[4]{}, result[4]{};
        ctx.read_vfpu_vector_with_source_prefix(a, vs, length, 0u);
        ctx.read_vfpu_vector_with_source_prefix(b, vt, length, 1u);
        for (std::uint32_t i = 0; i < length; ++i)
            result[i] = operation == 0u ? a[i] + b[i] : operation == 1u ? a[i] - b[i] : operation == 2u ? a[i] * b[i] : a[i] / b[i];
        ctx.write_vfpu_vector_with_destination_prefix(result, vd, length);
        break;
    }
    case K::Vdot: ctx.execute_vfpu_vdot(vd, vs, vt, length); break;
    case K::Vhdp: ctx.execute_vfpu_vhdp(vd, vs, vt, length); break;
    case K::VcrossQuat: ctx.execute_vfpu_cross_quat(vd, vs, vt, length); break;
    case K::Vminmax: ctx.execute_vfpu_vminmax(vd, vs, vt, length, ((w >> 23u) & 7u) == 3u); break;
    case K::VfpuCompare3: ctx.execute_vfpu_compare3(vd, vs, vt, length, (w >> 23u) & 7u); break;
    case K::Vcmp: ctx.execute_vfpu_vcmp(vs, vt, length, w & 15u); break;
    case K::Vcmov: ctx.execute_vfpu_vcmov(vd, vs, length, (w >> 16u) & 7u, ((w >> 19u) & 1u) != 0u); break;
    case K::Vscl: ctx.execute_vfpu_vscl(vd, vs, vt, length); break;
    case K::VfpuUnary: {
        float source[4]{}, result[4]{};
        ctx.read_vfpu_vector_with_source_prefix(source, vs, length, 0u);
        for (std::uint32_t i = 0; i < length; ++i) result[i] = unary((w >> 16u) & 31u, source[i]);
        ctx.write_vfpu_vector_with_destination_prefix(result, vd, length);
        break;
    }
    case K::Vcst: {
        const float constant = std::bit_cast<float>(kVfpuConstants[(w >> 16u) & 31u]);
        const float value[4]{constant, constant, constant, constant};
        ctx.write_vfpu_vector_with_destination_prefix(value, vd, length);
        break;
    }
    case K::Lvs: {
        const std::uint32_t at = s + static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(w & 0xFFFCu)));
        ctx.set_vfpu_scalar_bits(((w >> 16u) & 0x1Fu) | ((w & 3u) << 5u), m.aot_load32(at));
        break;
    }
    case K::Svs: {
        const std::uint32_t at = s + static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(w & 0xFFFCu)));
        m.aot_store32(at, ctx.vfpu_scalar_bits(((w >> 16u) & 0x1Fu) | ((w & 3u) << 5u)));
        break;
    }
    case K::Lvq: {
        const std::uint32_t at = s + static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(w & 0xFFFCu)));
        float value[4]{};
        for (std::uint32_t i = 0; i < 4u; ++i) value[i] = std::bit_cast<float>(m.aot_load32(at + i * 4u));
        ctx.write_vfpu_vector(value, ((w >> 16u) & 0x1Fu) | ((w & 1u) << 5u), 4u);
        break;
    }
    case K::Svq: {
        const std::uint32_t at = s + static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(w & 0xFFFCu)));
        float value[4]{};
        ctx.read_vfpu_vector(value, ((w >> 16u) & 0x1Fu) | ((w & 1u) << 5u), 4u);
        for (std::uint32_t i = 0; i < 4u; ++i) m.aot_store32(at + i * 4u, std::bit_cast<std::uint32_t>(value[i]));
        break;
    }
    case K::Vfpu:
        if (extra_vfpu(rt, ctx, w)) break;
        rt.unsupported(pc, w, d.mnemonic + " not interpreted");
        return false;
    default:
        rt.unsupported(pc, w, d.mnemonic + " not interpreted");
        return false;
    }
    return true;
}

std::size_t block_extent(const GuestMemory &m, std::uint32_t address) {
    std::uint32_t furthest = address;
    for (std::uint32_t pc = address; pc - address < kMaxBlockBytes; pc += 4u) {
        if (!m.contains(pc, 8u)) return (pc - address) / 4u;
        const DecodedInstruction d = decode_allegrex(m.load32(pc));
        std::uint32_t target = 0u;
        if (d.kind == K::J || d.kind == K::Jal) target = ((pc + 4u) & 0xF0000000u) | (d.target << 2u);
        else if (d.has_delay_slot() && d.kind != K::Jr && d.kind != K::Jalr)
            target = pc + 4u + static_cast<std::uint32_t>(static_cast<std::int32_t>(d.immediate) * 4);
        if (d.kind != K::Jal && target > furthest && target - address < kMaxBlockBytes) furthest = target;
        if (d.kind == K::Jr && d.rs == 31u && pc >= furthest) return (pc + 8u - address) / 4u;
    }
    return kMaxBlockBytes / 4u;
}

} // namespace

std::uint64_t dynamic_code_hash(const std::uint32_t *words, std::size_t count) noexcept {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < count; ++i) {
        for (std::uint32_t b = 0; b < 4u; ++b) {
            hash ^= (words[i] >> (b * 8u)) & 0xFFu;
            hash *= 0x100000001B3ull;
        }
    }
    return hash;
}

void register_dynamic_function(std::uint32_t address, std::uint64_t hash, Runtime::RecompiledFunction function) {
    registered_blocks()[{address, hash}] = function;
}

void set_dynamic_code_capture(DynamicCodeCapture capture) noexcept { g_capture = capture; }

void run_dynamic_code(Runtime &rt, AllegrexContext &ctx) {
    auto &m = rt.memory();
    const std::uint32_t address = ctx.pc;
    DynamicBlock &block = g_blocks[address];
    const std::size_t bytes = block.words.size() * 4u;
    const std::uint8_t *live = bytes != 0u ? m.raw_pointer(address, bytes) : nullptr;
    if (live == nullptr || std::memcmp(live, block.words.data(), bytes) != 0) {
        const std::size_t count = block_extent(m, address);
        block.words.resize(count);
        for (std::size_t i = 0; i < count; ++i) block.words[i] = m.load32(address + static_cast<std::uint32_t>(i * 4u));
        const std::uint64_t hash = dynamic_code_hash(block.words.data(), count);
        const auto found = registered_blocks().find({address, hash});
        block.function = found != registered_blocks().end() ? found->second : nullptr;
        if (block.function == nullptr && g_capture != nullptr && count != 0u && g_captured.insert({address, hash}).second)
            g_capture(address, block.words.data(), count);
    }
    if (block.function != nullptr) block.function(rt, ctx);
    else interpret(rt, ctx);
}

void interpret(Runtime &rt, AllegrexContext &ctx) {
    auto &m = rt.memory();
    for (std::uint32_t step = 0; step < kBudget; ++step) {
        const std::uint32_t pc = ctx.pc;
        if (step != 0u && rt.has_function(pc)) return;
        if ((pc & 3u) != 0u || !m.contains(pc, 8u)) {
            rt.stop("No recompiled function registered at " + hex32(pc));
            return;
        }
        const DecodedInstruction d = decode_allegrex(m.load32(pc));
        if (!d.has_delay_slot()) {
            if (!execute(rt, ctx, d, pc)) return;
            ctx.pc = pc + 4u;
            continue;
        }
        const DecodedInstruction slot = decode_allegrex(m.load32(pc + 4u));
        if (slot.is_control_flow()) {
            rt.unsupported(pc + 4u, slot.word, "control flow in delay slot");
            return;
        }
        std::uint32_t next = pc + 8u;
        if (d.kind == K::J || d.kind == K::Jal) {
            if (d.kind == K::Jal) ctx.set_gpr(31u, pc + 8u);
            if (!execute(rt, ctx, slot, pc + 4u)) return;
            next = ((pc + 4u) & 0xF0000000u) | (d.target << 2u);
        } else if (d.kind == K::Jr || d.kind == K::Jalr) {
            const std::uint32_t target = ctx.gpr[d.rs];
            if (d.kind == K::Jalr) ctx.set_gpr(d.rd == 0u ? 31u : d.rd, pc + 8u);
            if (!execute(rt, ctx, slot, pc + 4u)) return;
            next = target;
        } else {
            if (is_link(d.kind)) ctx.set_gpr(31u, pc + 8u);
            const bool taken = branch_taken(ctx, d);
            if (taken || !is_likely(d.kind)) {
                if (!execute(rt, ctx, slot, pc + 4u)) return;
            }
            if (taken) next = pc + 4u + static_cast<std::uint32_t>(static_cast<std::int32_t>(d.immediate) * 4);
        }
        ctx.pc = next;
        if ((d.kind == K::J || d.kind == K::Jal || d.kind == K::Jr || d.kind == K::Jalr) && !rt.has_function(next))
            record_missing_code(next);
    }
}

} // namespace psprecomp
