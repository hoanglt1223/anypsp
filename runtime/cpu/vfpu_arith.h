// SPDX-License-Identifier: GPL-2.0-or-later
//
// VFPU add, subtract, multiply and divide as the hardware rounds their special
// cases (pspautotests cpu/vfpu/nan_gen and fcr31):
// - denormal operands count as zero of the same sign, and a denormal result
//   is flushed to zero of its sign;
// - any NaN result is 0x7F800001, with the sign of s xor t for multiply and
//   divide and positive for add and subtract, whatever NaN went in.
// Shared by the interpreter and generated code, so both give the same bits
// whatever operand order the host compiler picks.

#pragma once

#include <bit>
#include <cstdint>

namespace anypsp::cpu {

// For a result r at most the smallest normal in magnitude. Tininess is decided before rounding:
// a result that only rounds up to the smallest normal was denormal, so it is zero too. exact is
// the operation in double precision.
inline float vfpu_tiny_result(float r, double exact) {
    const std::uint32_t u = std::bit_cast<std::uint32_t>(r);
    if ((u & 0x7FFFFFFFu) == 0x00800000u && !(exact < 0x1p-126 && exact > -0x1p-126)) return r;
    return std::bit_cast<float>(u & 0x80000000u);
}

inline bool vfpu_is_tiny(float r) { return (std::bit_cast<std::uint32_t>(r) & 0x7FFFFFFFu) <= 0x00800000u; }

// The operations decide a NaN result from the operand bits and return it as bits, so the
// signalling NaN 0x7F800001 never exists as a float value: MSVC quiets a float constant holding one
// (it stored 0x7FC00001). Their float math only runs when its result cannot be a NaN.
namespace vfpu_bits {
inline bool is_nan(std::uint32_t u) { return (u & 0x7FFFFFFFu) > 0x7F800000u; }
inline bool is_inf(std::uint32_t u) { return (u & 0x7FFFFFFFu) == 0x7F800000u; }
inline bool is_zero(std::uint32_t u) { return (u & 0x7F800000u) == 0u; }  // zero or denormal
inline float flushed(std::uint32_t u) { return std::bit_cast<float>(is_zero(u) ? u & 0x80000000u : u); }
inline std::uint32_t finish(float r, double exact) {
    if (vfpu_is_tiny(r)) [[unlikely]] return std::bit_cast<std::uint32_t>(vfpu_tiny_result(r, exact));
    return std::bit_cast<std::uint32_t>(r);
}
} // namespace vfpu_bits

inline std::uint32_t vfpu_add_bits(std::uint32_t a, std::uint32_t b) {
    using namespace vfpu_bits;
    if (is_nan(a) || is_nan(b) || (is_inf(a) && is_inf(b) && ((a ^ b) >> 31) != 0u)) return 0x7F800001u;
    const float x = flushed(a), y = flushed(b);
    return finish(x + y, static_cast<double>(x) + static_cast<double>(y));
}

inline std::uint32_t vfpu_sub_bits(std::uint32_t a, std::uint32_t b) {
    using namespace vfpu_bits;
    if (is_nan(a) || is_nan(b) || (is_inf(a) && is_inf(b) && ((a ^ b) >> 31) == 0u)) return 0x7F800001u;
    const float x = flushed(a), y = flushed(b);
    return finish(x - y, static_cast<double>(x) - static_cast<double>(y));
}

inline std::uint32_t vfpu_mul_bits(std::uint32_t a, std::uint32_t b) {
    using namespace vfpu_bits;
    if (is_nan(a) || is_nan(b) || (is_inf(a) && is_zero(b)) || (is_zero(a) && is_inf(b)))
        return 0x7F800001u | ((a ^ b) & 0x80000000u);
    const float x = flushed(a), y = flushed(b);
    return finish(x * y, static_cast<double>(x) * static_cast<double>(y));
}

inline std::uint32_t vfpu_div_bits(std::uint32_t a, std::uint32_t b) {
    using namespace vfpu_bits;
    if (is_nan(a) || is_nan(b) || (is_inf(a) && is_inf(b)) || (is_zero(a) && is_zero(b)))
        return 0x7F800001u | ((a ^ b) & 0x80000000u);
    const float x = flushed(a), y = flushed(b);
    return finish(x / y, static_cast<double>(x) / static_cast<double>(y));
}

// Float forms for callers that keep lanes as floats; store the _bits forms where the result can be
// the positive NaN.
inline float vfpu_add(float a, float b) {
    return std::bit_cast<float>(vfpu_add_bits(std::bit_cast<std::uint32_t>(a), std::bit_cast<std::uint32_t>(b)));
}
inline float vfpu_sub(float a, float b) {
    return std::bit_cast<float>(vfpu_sub_bits(std::bit_cast<std::uint32_t>(a), std::bit_cast<std::uint32_t>(b)));
}
inline float vfpu_mul(float a, float b) {
    return std::bit_cast<float>(vfpu_mul_bits(std::bit_cast<std::uint32_t>(a), std::bit_cast<std::uint32_t>(b)));
}
inline float vfpu_div(float a, float b) {
    return std::bit_cast<float>(vfpu_div_bits(std::bit_cast<std::uint32_t>(a), std::bit_cast<std::uint32_t>(b)));
}

} // namespace anypsp::cpu
