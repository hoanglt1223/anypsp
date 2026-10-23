// SPDX-License-Identifier: GPL-2.0-or-later
//
// Random-state differential test of the code generator: every case is the C++
// psp_recomp emits for one instruction word, run against the reference
// interpreter (anypsp::cpu::execute) from identical register and memory
// states. Any difference in registers, control registers or memory fails.
//
//   anypsp_cpu_difftest [states per case]

#include "difftest_cases.hpp"

#include "cpu/interp.h"
#include "psprecomp/common.hpp"

#include <bit>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kWindow = 0x08A00000u;
constexpr std::uint32_t kWindowBytes = 0x20000u;

constexpr std::uint32_t kFloatSpecials[] = {
    0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u,
    0x7F800001u, 0x7FBFFFFFu, 0x00000001u, 0x807FFFFFu, 0x00800000u, 0x7F7FFFFFu, 0x4F000000u, 0xCF000000u,
    0x4EFFFFFFu, 0x3F000000u, 0x3FC00000u, 0x40200000u, 0xC0200000u, 0x4B800000u, 0xCB400001u,
};
constexpr std::uint32_t kIntSpecials[] = {
    0u, 1u, 2u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0x0000FFFFu, 0xFFFF0000u, 0x00008000u, 31u, 32u, 33u,
};

std::uint32_t random_int(std::mt19937 &rng) {
    switch (rng() % 4u) {
    case 0: return kIntSpecials[rng() % std::size(kIntSpecials)];
    case 1: return rng() % 64u;
    default: return static_cast<std::uint32_t>(rng());
    }
}

std::uint32_t random_float_bits(std::mt19937 &rng) {
    switch (rng() % 4u) {
    case 0: return kFloatSpecials[rng() % std::size(kFloatSpecials)];
    case 1: return std::bit_cast<std::uint32_t>(static_cast<float>(static_cast<int>(rng() % 2001u) - 1000) / 8.0f);
    default: return static_cast<std::uint32_t>(rng());
    }
}

// Natural alignment of a memory access, 0 for non-memory instructions.
std::uint32_t access_alignment(const std::string &m) {
    if (m == "lb" || m == "lbu" || m == "sb" || m == "lwl" || m == "lwr" || m == "swl" || m == "swr" || m == "cache")
        return 1u;
    if (m == "lh" || m == "lhu" || m == "sh") return 2u;
    if (m == "lw" || m == "sw" || m == "lwc1" || m == "swc1" || m == "lv.s" || m == "sv.s") return 4u;
    if (m == "lv.q" || m == "sv.q") return 16u;
    return 0u;
}

void randomize(psprecomp::AllegrexContext &ctx, std::mt19937 &rng) {
    for (auto &r : ctx.gpr) r = random_int(rng);
    ctx.gpr[0] = 0u;
    ctx.hi = random_int(rng);
    ctx.lo = random_int(rng);
    for (auto &f : ctx.fpr) f = std::bit_cast<float>(random_float_bits(rng));
    for (auto &v : ctx.vfpu) v = std::bit_cast<float>(random_float_bits(rng));
    ctx.fcr31 = 0x00000E00u | ((rng() & 1u) << 23u);
    static constexpr std::uint32_t control[16] = {0xE4u, 0xE4u, 0u, 0x3Fu, 0u, 0u, 0u, 0x7772CEABu,
                                                  0x3F800001u, 0x3F800002u, 0x3F800004u, 0x3F800008u,
                                                  0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u};
    for (std::size_t i = 0; i < 16u; ++i) ctx.vfpu_ctrl[i] = control[i];
    if (rng() % 4u == 0u) ctx.vfpu_ctrl[0] = rng() & 0xFFFFFu;
    if (rng() % 4u == 0u) ctx.vfpu_ctrl[2] = rng() & 0xFFFu;
}

std::string describe_difference(const psprecomp::AllegrexContext &a, const psprecomp::AllegrexContext &b) {
    using psprecomp::hex32;
    for (std::size_t i = 0; i < 32u; ++i)
        if (a.gpr[i] != b.gpr[i]) return "gpr" + std::to_string(i) + " " + hex32(a.gpr[i]) + " vs " + hex32(b.gpr[i]);
    if (a.hi != b.hi) return "hi " + hex32(a.hi) + " vs " + hex32(b.hi);
    if (a.lo != b.lo) return "lo " + hex32(a.lo) + " vs " + hex32(b.lo);
    for (std::size_t i = 0; i < 32u; ++i)
        if (a.fpr_bits(static_cast<std::uint32_t>(i)) != b.fpr_bits(static_cast<std::uint32_t>(i)))
            return "f" + std::to_string(i) + " " + hex32(a.fpr_bits(static_cast<std::uint32_t>(i))) + " vs " +
                   hex32(b.fpr_bits(static_cast<std::uint32_t>(i)));
    if (a.fcr31 != b.fcr31) return "fcr31 " + hex32(a.fcr31) + " vs " + hex32(b.fcr31);
    for (std::size_t i = 0; i < 128u; ++i)
        if (std::bit_cast<std::uint32_t>(a.vfpu[i]) != std::bit_cast<std::uint32_t>(b.vfpu[i]))
            return "vfpu[" + std::to_string(i) + "] " + hex32(std::bit_cast<std::uint32_t>(a.vfpu[i])) + " vs " +
                   hex32(std::bit_cast<std::uint32_t>(b.vfpu[i]));
    for (std::size_t i = 0; i < 16u; ++i)
        if (a.vfpu_ctrl[i] != b.vfpu_ctrl[i])
            return "vfpu_ctrl[" + std::to_string(i) + "] " + hex32(a.vfpu_ctrl[i]) + " vs " + hex32(b.vfpu_ctrl[i]);
    return {};
}

} // namespace

int main(int argc, char **argv) {
    const int states = argc > 1 ? std::stoi(argv[1]) : 64;
    auto runtime = std::make_unique<psprecomp::Runtime>();
    auto &rt = *runtime;
    auto &memory = rt.memory();
    std::mt19937 rng(0xA11E64u);
    std::vector<std::uint8_t> initial(kWindowBytes), generated(kWindowBytes), reference(kWindowBytes);
    std::map<std::string, std::pair<int, std::string>> failures;  // mnemonic -> count, first example
    std::size_t runs = 0;

    for (std::size_t c = 0; c < kDiffCaseCount; ++c) {
        const DiffCase &test = kDiffCases[c];
        const std::uint32_t alignment = access_alignment(test.mnemonic);
        const std::uint32_t rs = (test.word >> 21u) & 31u;
        if (alignment != 0u && rs == 0u) continue;
        for (int s = 0; s < states; ++s) {
            psprecomp::AllegrexContext start{};
            randomize(start, rng);
            if (alignment != 0u) {
                const bool vfpu = alignment == 16u || test.mnemonic[0] == 'l' && test.mnemonic[1] == 'v' ||
                                  test.mnemonic[0] == 's' && test.mnemonic[1] == 'v';
                const auto offset = vfpu ? static_cast<std::int32_t>(static_cast<std::int16_t>(test.word & 0xFFFCu))
                                         : static_cast<std::int32_t>(static_cast<std::int16_t>(test.word & 0xFFFFu));
                const std::uint32_t target = kWindow + 0x8000u + ((static_cast<std::uint32_t>(rng()) % 0x8000u) & ~(alignment - 1u));
                start.gpr[rs] = target - static_cast<std::uint32_t>(offset);
            }
            for (auto &byte : initial) byte = static_cast<std::uint8_t>(rng());

            psprecomp::AllegrexContext a = start, b = start;
            memory.copy_in(kWindow, initial);
            test.run(rt, a);
            memory.copy_out(kWindow, generated);

            memory.copy_in(kWindow, initial);
            (void)anypsp::cpu::execute(rt, b, 0x08804000u, test.word);
            memory.copy_out(kWindow, reference);
            ++runs;

            std::string difference = describe_difference(a, b);
            if (difference.empty() && generated != reference) {
                for (std::size_t i = 0; i < kWindowBytes; ++i)
                    if (generated[i] != reference[i]) {
                        difference = "memory " + psprecomp::hex32(kWindow + static_cast<std::uint32_t>(i));
                        break;
                    }
            }
            if (!difference.empty()) {
                auto &entry = failures[test.mnemonic];
                if (entry.first++ == 0)
                    entry.second = psprecomp::hex32(test.word) + ": " + difference + " (generated vs reference)";
            }
        }
    }

    std::cout << kDiffCaseCount << " encodings, " << runs << " runs\n";
    for (const auto &[mnemonic, failure] : failures)
        std::cout << "FAIL " << mnemonic << " (" << failure.first << " runs), e.g. " << failure.second << "\n";
    return failures.empty() ? 0 : 1;
}
