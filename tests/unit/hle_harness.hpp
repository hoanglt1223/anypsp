// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// A kernel with every HLE function installed and no guest program: tests call functions
// through the runtime's import table as a game would.

#include "test_harness.hpp"

#include "kernel.hpp"

#include "psprecomp/runtime.hpp"

#include <memory>
#include <string>

namespace anypsp_test {

inline constexpr std::uint32_t kScratch = 0x08800000u;
inline constexpr std::uint32_t kReturn = 0x08900000u;

struct Hle {
    psprecomp::Runtime rt;
    std::unique_ptr<pspweb::Kernel> kernel;

    Hle() : kernel(std::make_unique<pspweb::Kernel>(rt)) { kernel->install({}); }

    std::uint32_t call(const char *library, std::uint32_t nid, std::uint32_t a0 = 0u, std::uint32_t a1 = 0u,
                       std::uint32_t a2 = 0u, std::uint32_t a3 = 0u, std::uint32_t t0 = 0u, std::uint32_t t1 = 0u,
                       std::uint32_t t2 = 0u) {
        psprecomp::AllegrexContext ctx{};
        ctx.gpr[4] = a0;
        ctx.gpr[5] = a1;
        ctx.gpr[6] = a2;
        ctx.gpr[7] = a3;
        ctx.gpr[8] = t0;
        ctx.gpr[9] = t1;
        ctx.gpr[10] = t2;
        ctx.gpr[31] = kReturn;
        rt.invoke_import(library, nid, ctx);
        CHECK(ctx.pc == kReturn);
        last = ctx;
        return ctx.gpr[2];
    }

    void store_string(std::uint32_t address, const std::string &text) {
        for (std::size_t i = 0; i <= text.size(); ++i)
            rt.memory().store8(address + static_cast<std::uint32_t>(i), i < text.size() ? static_cast<std::uint8_t>(text[i]) : 0u);
    }

    // The registers after the last call (v1, f0).
    psprecomp::AllegrexContext last{};
};

} // namespace anypsp_test
