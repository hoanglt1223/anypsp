// SPDX-License-Identifier: GPL-2.0-or-later
//
// VFPU add, subtract, multiply and divide with a fixed NaN result: when an
// operand is NaN, the first NaN operand, quieted, as an x86 scalar operation
// in operand order gives. Without this, NaN op NaN would depend on the operand
// order the host compiler picks, which differs between the interpreter and
// generated code. Other NaN results (inf - inf, 0 * inf) keep the host's
// default NaN.

#pragma once

#include <bit>
#include <cstdint>

namespace anypsp::cpu {

inline float vfpu_nan_result(float a, float b, float result) {
    const std::uint32_t x = std::bit_cast<std::uint32_t>(a), y = std::bit_cast<std::uint32_t>(b);
    if ((x & 0x7FFFFFFFu) > 0x7F800000u) return std::bit_cast<float>(x | 0x00400000u);
    if ((y & 0x7FFFFFFFu) > 0x7F800000u) return std::bit_cast<float>(y | 0x00400000u);
    return result;
}

inline bool vfpu_is_nan(float f) { return (std::bit_cast<std::uint32_t>(f) & 0x7FFFFFFFu) > 0x7F800000u; }

inline float vfpu_add(float a, float b) {
    const float r = a + b;
    return vfpu_is_nan(r) ? vfpu_nan_result(a, b, r) : r;
}
inline float vfpu_sub(float a, float b) {
    const float r = a - b;
    return vfpu_is_nan(r) ? vfpu_nan_result(a, b, r) : r;
}
inline float vfpu_mul(float a, float b) {
    const float r = a * b;
    return vfpu_is_nan(r) ? vfpu_nan_result(a, b, r) : r;
}
inline float vfpu_div(float a, float b) {
    const float r = a / b;
    return vfpu_is_nan(r) ? vfpu_nan_result(a, b, r) : r;
}

} // namespace anypsp::cpu
