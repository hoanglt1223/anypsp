// SPDX-License-Identifier: GPL-2.0-or-later
//
// The HI16 pairing rules and the packed 0x700000A1 decoder follow PPSSPP's
// Core/ELF/ElfReader.cpp (LoadRelocations, LoadRelocations2), GPL-2.0-or-later.

#include "relocs.hpp"

#include "psprecomp/common.hpp"

#include <cstring>

namespace anypsp::loader {
namespace {

using psprecomp::Elf32Image;
using psprecomp::GuestMemory;
using psprecomp::hex32;

constexpr std::uint32_t kPtLoad = 1u;
constexpr std::size_t kMaxProblems = 16u;

struct Rel {
    std::uint32_t offset;
    std::uint32_t info;
};

std::uint32_t read_le32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8u) |
           (static_cast<std::uint32_t>(p[2]) << 16u) | (static_cast<std::uint32_t>(p[3]) << 24u);
}

std::uint16_t hi_for(std::uint32_t address) {
    return static_cast<std::uint16_t>((address >> 16u) + ((address & 0x8000u) != 0u ? 1u : 0u));
}

class Relocator {
public:
    Relocator(const Elf32Image &elf, GuestMemory &memory, std::uint32_t load_base)
        : elf_(elf), memory_(memory), bases_(segment_bases(elf, load_base)) {}

    void table(std::span<const std::uint8_t> raw) {
        std::vector<Rel> rels(raw.size() / sizeof(Rel));
        for (std::size_t i = 0; i < rels.size(); ++i)
            rels[i] = {read_le32(raw.data() + i * 8u), read_le32(raw.data() + i * 8u + 4u)};

        std::vector<std::uint32_t> addresses(rels.size(), 0u);
        std::vector<std::uint32_t> original(rels.size(), 0u);
        std::vector<bool> valid(rels.size(), false);
        for (std::size_t r = 0; r < rels.size(); ++r) {
            const std::uint32_t type = rels[r].info & 0x0Fu;
            const std::uint32_t segment = (rels[r].info >> 8u) & 0xFFu;
            if (!loaded(segment)) {
                ++result_.invalid;
                problem("relocation " + std::to_string(r) + " patches unloaded segment " + std::to_string(segment));
                continue;
            }
            const std::uint32_t address = bases_[segment] + rels[r].offset;
            if (((address & 3u) != 0u && type != kRel32) || !memory_.contains(address, 4u)) {
                ++result_.invalid;
                problem("relocation " + std::to_string(r) + " has a bad address " + hex32(address));
                continue;
            }
            addresses[r] = address;
            original[r] = memory_.load32(address);
            valid[r] = true;
        }

        for (std::size_t r = 0; r < rels.size(); ++r) {
            if (!valid[r]) continue;
            const auto type = static_cast<std::uint8_t>(rels[r].info & 0x0Fu);
            const std::uint32_t target_segment = (rels[r].info >> 16u) & 0xFFu;
            const std::uint32_t target = loaded(target_segment) ? bases_[target_segment] : 0u;
            std::uint32_t op = original[r];
            switch (type) {
            case kRel32: op += target; break;
            case kRel26: op = (op & 0xFC000000u) | (((op & 0x03FFFFFFu) + (target >> 2u)) & 0x03FFFFFFu); break;
            case kRelHi16: {
                bool found = false;
                for (std::size_t t = r + 1u; t < rels.size(); ++t) {
                    const std::uint32_t t_type = rels[t].info & 0x0Fu;
                    if (t_type == kRelHi16) continue;
                    // The PSP takes R_MIPS_16 as the low half too; anything else is skipped.
                    if (t_type != kRelLo16 && t_type != kRel16) continue;
                    if (!valid[t]) continue;
                    const std::uint32_t value = ((op & 0xFFFFu) << 16u) +
                        static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(original[t] & 0xFFFFu))) +
                        target;
                    op = (op & 0xFFFF0000u) | hi_for(value);
                    found = true;
                    break;
                }
                if (!found) {
                    ++result_.invalid;
                    problem("R_MIPS_HI16 at " + hex32(addresses[r]) + " has no matching R_MIPS_LO16");
                    continue;
                }
                break;
            }
            case kRelLo16:
            case kRel16: op = (op & 0xFFFF0000u) | ((op + target) & 0xFFFFu); break;
            case kRelNone:
            case kRelGpRel16: ++result_.ignored; continue;
            default:
                ++result_.unsupported;
                problem("unsupported relocation type " + std::to_string(type) + " at " + hex32(addresses[r]));
                continue;
            }
            store(addresses[r], op, type, target);
        }
    }

    void packed(std::size_t rel_segment) {
        const auto &ph = elf_.segments()[rel_segment];
        const auto &bytes = elf_.bytes();
        if (static_cast<std::uint64_t>(ph.offset) + ph.file_size > bytes.size()) {
            ++result_.truncated_tables;
            problem("packed relocation segment " + std::to_string(rel_segment) + " extends past the end of the file");
            return;
        }
        const std::uint8_t *buf = bytes.data() + ph.offset;
        const std::uint8_t *const end = buf + ph.file_size;
        auto have = [&](std::ptrdiff_t n) {
            if (end - buf >= n) return true;
            ++result_.invalid;
            problem("packed relocations are truncated");
            return false;
        };
        if (!have(4)) return;
        const int flag_bits = buf[2];
        const int type_bits = buf[3];
        int seg_bits = 1;
        while ((1 << seg_bits) < static_cast<int>(rel_segment)) ++seg_bits;
        buf += 4;
        if (!have(1)) return;
        const std::uint8_t *flag_table = buf;
        const int flag_table_size = flag_table[0];
        if (!have(flag_table_size)) return;
        buf += flag_table_size;
        if (!have(1)) return;
        const std::uint8_t *type_table = buf;
        const int type_table_size = type_table[0];
        if (!have(type_table_size)) return;
        buf += type_table_size;
        if (flag_bits + seg_bits + type_bits > 16) {
            fail("packed relocation header has more than 16 bits per command");
            return;
        }

        std::int32_t rel_base = 0;
        int off_segment = 0;
        int last_type = -1;
        std::int32_t lo16 = 0;
        const int shift = type_bits + seg_bits + flag_bits;
        while (buf < end) {
            if (!have(2)) return;
            const int cmd = buf[0] | (buf[1] << 8);
            buf += 2;
            int flag = ((cmd << (16 - flag_bits)) & 0xFFFF) >> (16 - flag_bits);
            if (flag >= flag_table_size) {
                fail("packed relocation flag index out of range");
                return;
            }
            flag = flag_table[flag];
            const int segment = ((cmd << (16 - seg_bits - flag_bits)) & 0xFFFF) >> (16 - seg_bits);
            int type = ((cmd << (16 - type_bits - seg_bits - flag_bits)) & 0xFFFF) >> (16 - type_bits);
            if (type >= type_table_size) {
                fail("packed relocation type index out of range");
                return;
            }
            type = type_table[type];

            if ((flag & 0x01) == 0) {
                off_segment = segment;
                if ((flag & 0x06) == 0) {
                    rel_base = cmd >> (seg_bits + flag_bits);
                } else if ((flag & 0x06) == 4) {
                    if (!have(4)) return;
                    rel_base = static_cast<std::int32_t>(read_le32(buf));
                    buf += 4;
                } else {
                    fail("packed relocation has an invalid base size flag");
                    rel_base = 0;
                }
                continue;
            }

            if ((flag & 0x06) == 0x00) {
                std::int32_t offset = cmd;
                if ((cmd & 0x8000) != 0) {
                    offset |= static_cast<std::int32_t>(0xFFFF0000u);
                    offset >>= shift;
                    offset |= static_cast<std::int32_t>(0xFFFF0000u);
                } else {
                    offset >>= shift;
                }
                rel_base += offset;
            } else if ((flag & 0x06) == 0x02) {
                std::int32_t offset = cmd;
                if ((cmd & 0x8000) != 0) offset |= static_cast<std::int32_t>(0xFFFF0000u);
                offset >>= shift;
                if (!have(2)) return;
                offset = static_cast<std::int32_t>((static_cast<std::uint32_t>(offset) << 16u) | buf[0] | (buf[1] << 8));
                buf += 2;
                rel_base += offset;
            } else if ((flag & 0x06) == 0x04) {
                if (!have(4)) return;
                rel_base = static_cast<std::int32_t>(read_le32(buf));
                buf += 4;
            } else {
                fail("packed relocation has an invalid offset size flag");
            }

            if ((flag & 0x38) == 0x00) {
                lo16 = 0;
            } else if ((flag & 0x38) == 0x08) {
                if (last_type != 4) lo16 = 0;
            } else if ((flag & 0x38) == 0x10) {
                if (!have(2)) return;
                lo16 = static_cast<std::int16_t>(buf[0] | (buf[1] << 8));
                buf += 2;
            } else {
                fail("packed relocation has an invalid lo16 flag");
            }

            if (!loaded(static_cast<std::uint32_t>(segment)) || !loaded(static_cast<std::uint32_t>(off_segment))) {
                ++result_.invalid;
                problem("packed relocation refers to an unloaded segment");
                continue;
            }
            const std::uint32_t target = bases_[static_cast<std::size_t>(segment)];
            const std::uint32_t address = bases_[static_cast<std::size_t>(off_segment)] + static_cast<std::uint32_t>(rel_base);
            if (!memory_.contains(address, 4u)) {
                ++result_.invalid;
                problem("packed relocation has a bad address " + hex32(address));
                continue;
            }
            std::uint32_t op = memory_.load32(address);
            std::uint8_t normalized = kRelNone;
            switch (type) {
            case 0: break;
            case 2: op += target; normalized = kRel32; break;
            case 3: case 6: case 7:
                op = (op & 0xFC000000u) | (((op & 0x03FFFFFFu) + (target >> 2u)) & 0x03FFFFFFu);
                if (type == 6) op = (op & ~0xFC000000u) | 0x08000000u;
                else if (type == 7) op = (op & ~0xFC000000u) | 0x0C000000u;
                normalized = kRel26;
                break;
            case 4: {
                std::uint32_t value = (op << 16u) + static_cast<std::uint32_t>(lo16) + target;
                if ((value & 0x8000u) != 0u) value += 0x00010000u;
                op = (op & 0xFFFF0000u) | (value >> 16u);
                normalized = kRelHi16;
                break;
            }
            case 1: case 5: op = (op & 0xFFFF0000u) | (((op & 0xFFFFu) + target) & 0xFFFFu); normalized = kRelLo16; break;
            default:
                ++result_.unsupported;
                problem("unsupported packed relocation type " + std::to_string(type) + " at " + hex32(address));
                last_type = type;
                continue;
            }
            last_type = type;
            if (normalized == kRelNone) {
                ++result_.ignored;
                continue;
            }
            store(address, op, normalized, target);
        }
    }

    RelocationResult take() { return std::move(result_); }
    RelocationResult &result() { return result_; }

private:
    bool loaded(std::uint32_t segment) const {
        return segment < bases_.size() && bases_[segment] != kSegmentNotLoaded;
    }
    void store(std::uint32_t address, std::uint32_t op, std::uint8_t type, std::uint32_t target) {
        memory_.store32(address, op);
        result_.sites.push_back({address, type, target});
        ++result_.applied;
    }
    void fail(std::string message) {
        ++result_.invalid;
        problem(std::move(message));
    }
    void problem(std::string message) {
        if (result_.problems.size() < kMaxProblems) result_.problems.push_back(std::move(message));
    }

    const Elf32Image &elf_;
    GuestMemory &memory_;
    std::vector<std::uint32_t> bases_;
    RelocationResult result_;
};

} // namespace

std::vector<std::uint32_t> segment_bases(const Elf32Image &elf, std::uint32_t load_base) {
    std::vector<std::uint32_t> bases;
    bases.reserve(elf.segments().size());
    for (std::size_t i = 0; i < elf.segments().size(); ++i)
        bases.push_back(elf.segments()[i].type == kPtLoad ? elf.segment_runtime_address(i, load_base) : kSegmentNotLoaded);
    return bases;
}

RelocationResult apply_relocations(const Elf32Image &elf, GuestMemory &memory, std::uint32_t load_base) {
    if (!elf.is_psp_prx()) return {};
    Relocator relocator(elf, memory, load_base);
    const auto &bytes = elf.bytes();
    if (!elf.sections().empty()) {
        relocator.result().format = RelocFormat::Sections;
        for (const auto &section : elf.sections()) {
            if (section.type != psprecomp::kSectionTypePspRel || section.size == 0u) continue;
            if (static_cast<std::uint64_t>(section.offset) + section.size > bytes.size()) {
                ++relocator.result().truncated_tables;
                continue;
            }
            relocator.table(std::span<const std::uint8_t>(bytes).subspan(section.offset, section.size & ~7u));
        }
        return relocator.take();
    }
    for (std::size_t i = 0; i < elf.segments().size(); ++i) {
        const auto &ph = elf.segments()[i];
        if (ph.type == kProgramTypePspRel) {
            relocator.result().format = RelocFormat::Segments;
            if (static_cast<std::uint64_t>(ph.offset) + ph.file_size > bytes.size()) {
                ++relocator.result().truncated_tables;
                continue;
            }
            relocator.table(std::span<const std::uint8_t>(bytes).subspan(ph.offset, ph.file_size & ~7u));
        } else if (ph.type == kProgramTypePspRel2) {
            relocator.result().format = RelocFormat::PackedSegments;
            relocator.packed(i);
        }
    }
    return relocator.take();
}

std::vector<psprecomp::PspRelocationSite> analysis_sites(const RelocationResult &result) {
    std::vector<psprecomp::PspRelocationSite> sites;
    sites.reserve(result.sites.size());
    for (const auto &site : result.sites) sites.push_back({site.address, site.type, 0u, 0u});
    return sites;
}

const char *format_name(RelocFormat format) noexcept {
    switch (format) {
    case RelocFormat::None: return "none";
    case RelocFormat::Sections: return "sections";
    case RelocFormat::Segments: return "segments";
    case RelocFormat::PackedSegments: return "packed_segments";
    }
    return "unknown";
}

} // namespace anypsp::loader
