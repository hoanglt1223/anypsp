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

inline float vfpu_flush(float f) {
    const std::uint32_t u = std::bit_cast<std::uint32_t>(f);
    return (u & 0x7F800000u) == 0u ? std::bit_cast<float>(u & 0x80000000u) : f;
}

// For a result r at most the smallest normal in magnitude. Tininess is decided before rounding:
// a result that only rounds up to the smallest normal was denormal, so it is zero too. exact is
// the operation in double precision.
inline float vfpu_tiny_result(float r, double exact) {
    const std::uint32_t u = std::bit_cast<std::uint32_t>(r);
    if ((u & 0x7FFFFFFFu) == 0x00800000u && !(exact < 0x1p-126 && exact > -0x1p-126)) return r;
    return std::bit_cast<float>(u & 0x80000000u);
}

inline bool vfpu_is_tiny(float r) { return (std::bit_cast<std::uint32_t>(r) & 0x7FFFFFFFu) <= 0x00800000u; }

inline bool vfpu_is_nan(float f) { return (std::bit_cast<std::uint32_t>(f) & 0x7FFFFFFFu) > 0x7F800000u; }

inline float vfpu_add(float a, float b) {
    const float x = vfpu_flush(a), y = vfpu_flush(b), r = x + y;
    if (vfpu_is_nan(r)) return std::bit_cast<float>(0x7F800001u);
    if (vfpu_is_tiny(r)) [[unlikely]]
        return vfpu_tiny_result(r, static_cast<double>(x) + static_cast<double>(y));
    return r;
}

inline float vfpu_sub(float a, float b) {
    const float x = vfpu_flush(a), y = vfpu_flush(b), r = x - y;
    if (vfpu_is_nan(r)) return std::bit_cast<float>(0x7F800001u);
    if (vfpu_is_tiny(r)) [[unlikely]]
        return vfpu_tiny_result(r, static_cast<double>(x) - static_cast<double>(y));
    return r;
}

inline float vfpu_signed_nan(float a, float b) {
    return std::bit_cast<float>(0x7F800001u |
                                ((std::bit_cast<std::uint32_t>(a) ^ std::bit_cast<std::uint32_t>(b)) & 0x80000000u));
}

inline float vfpu_mul(float a, float b) {
    const float x = vfpu_flush(a), y = vfpu_flush(b), r = x * y;
    if (vfpu_is_nan(r)) return vfpu_signed_nan(a, b);
    if (vfpu_is_tiny(r)) [[unlikely]]
        return vfpu_tiny_result(r, static_cast<double>(x) * static_cast<double>(y));
    return r;
}

inline float vfpu_div(float a, float b) {
    const float x = vfpu_flush(a), y = vfpu_flush(b), r = x / y;
    if (vfpu_is_nan(r)) return vfpu_signed_nan(a, b);
    if (vfpu_is_tiny(r)) [[unlikely]]
        return vfpu_tiny_result(r, static_cast<double>(x) / static_cast<double>(y));
    return r;
}

} // namespace anypsp::cpu
