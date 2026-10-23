// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "psprecomp/runtime.hpp"

#include <cstddef>
#include <cstdint>

struct DiffCase {
    std::uint32_t word;
    const char *mnemonic;
    void (*run)(psprecomp::Runtime &, psprecomp::AllegrexContext &);
};

extern const DiffCase kDiffCases[];
extern const std::size_t kDiffCaseCount;
