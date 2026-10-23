// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "psprecomp/elf32.hpp"
#include "psprecomp/guest_memory.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace anypsp::loader {

// Relocation types, numbered as in the classic Elf32_Rel tables. Packed (0x700000A1)
// relocations are converted to these numbers.
enum RelocType : std::uint8_t {
    kRelNone = 0,
    kRel16 = 1,
    kRel32 = 2,
    kRel26 = 4,
    kRelHi16 = 5,
    kRelLo16 = 6,
    kRelGpRel16 = 7,
};

enum class RelocFormat : std::uint8_t { None, Sections, Segments, PackedSegments };

struct RelocSite {
    std::uint32_t address{};
    std::uint8_t type{};
    std::uint32_t target_base{};
};

struct RelocationResult {
    RelocFormat format{RelocFormat::None};
    std::vector<RelocSite> sites; // every relocation that was applied, in table order
    std::uint32_t applied{};
    std::uint32_t ignored{};      // R_MIPS_NONE and R_MIPS_GPREL16, which change nothing
    std::uint32_t unsupported{};
    std::uint32_t invalid{};
    std::uint32_t truncated_tables{}; // relocation tables that lie past the end of the file
    std::vector<std::string> problems;
};

inline constexpr std::uint32_t kSegmentNotLoaded = 0xFFFFFFFFu;
inline constexpr std::uint32_t kProgramTypePspRel = 0x700000A0u;
inline constexpr std::uint32_t kProgramTypePspRel2 = 0x700000A1u;

// Runtime address of every program header, or kSegmentNotLoaded for non-PT_LOAD ones.
[[nodiscard]] std::vector<std::uint32_t> segment_bases(const psprecomp::Elf32Image &elf, std::uint32_t load_base);

// Applies every PSP relocation format to an image already copied to `load_base`:
// SHT_PSPREL sections, or, when the file has no section headers, 0x700000A0 and
// packed 0x700000A1 program headers. ET_EXEC images are returned unchanged.
[[nodiscard]] RelocationResult apply_relocations(const psprecomp::Elf32Image &elf,
                                                 psprecomp::GuestMemory &memory,
                                                 std::uint32_t load_base);

[[nodiscard]] std::vector<psprecomp::PspRelocationSite> analysis_sites(const RelocationResult &result);

[[nodiscard]] const char *format_name(RelocFormat format) noexcept;

} // namespace anypsp::loader
