// SPDX-License-Identifier: GPL-2.0-or-later
// Builds small synthetic ELF/PRX images for tests.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace anypsp_test {

struct Segment {
    std::uint32_t type{1};
    std::uint32_t vaddr{};
    std::vector<std::uint8_t> data;
    std::uint32_t memsz{}; // 0: same as data size
    std::uint32_t flags{5};
    // When set, p_paddr becomes the file offset of data[paddr_data_offset] (module info).
    std::int64_t paddr_data_offset{-1};
};

struct Section {
    std::string name;
    std::uint32_t type{1};
    std::vector<std::uint8_t> data;
    std::uint32_t addr{};
    std::uint32_t link{};
    std::uint32_t info{};
    std::uint32_t entsize{};
    bool past_end{}; // header points past the end of the file
};

inline void put16(std::vector<std::uint8_t> &b, std::size_t at, std::uint32_t v) {
    if (b.size() < at + 2) b.resize(at + 2);
    b[at] = static_cast<std::uint8_t>(v);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}
inline void put32(std::vector<std::uint8_t> &b, std::size_t at, std::uint32_t v) {
    if (b.size() < at + 4) b.resize(at + 4);
    for (int i = 0; i < 4; ++i) b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline void append32(std::vector<std::uint8_t> &b, std::uint32_t v) { put32(b, b.size(), v); }
inline std::vector<std::uint8_t> words(std::initializer_list<std::uint32_t> list) {
    std::vector<std::uint8_t> out;
    for (const auto w : list) append32(out, w);
    return out;
}

inline std::vector<std::uint8_t> build_elf(std::uint16_t type, std::uint32_t entry, const std::vector<Segment> &segments,
                                           const std::vector<Section> &sections = {}) {
    std::vector<std::uint8_t> b(52, 0);
    b[0] = 0x7F; b[1] = 'E'; b[2] = 'L'; b[3] = 'F'; b[4] = 1; b[5] = 1; b[6] = 1;
    put16(b, 16, type);
    put16(b, 18, 8);
    put32(b, 20, 1);
    put32(b, 24, entry);
    put32(b, 28, 52);
    put16(b, 40, 52);
    put16(b, 42, 32);
    put16(b, 44, static_cast<std::uint32_t>(segments.size()));
    put16(b, 46, 40);
    b.resize(52 + 32 * segments.size(), 0);
    const auto align16 = [&] { while (b.size() % 16 != 0) b.push_back(0); };
    for (std::size_t i = 0; i < segments.size(); ++i) {
        align16();
        const auto &s = segments[i];
        const auto offset = static_cast<std::uint32_t>(b.size());
        b.insert(b.end(), s.data.begin(), s.data.end());
        const std::size_t ph = 52 + i * 32;
        put32(b, ph, s.type);
        put32(b, ph + 4, offset);
        put32(b, ph + 8, s.vaddr);
        put32(b, ph + 12, s.paddr_data_offset >= 0 ? offset + static_cast<std::uint32_t>(s.paddr_data_offset) : s.vaddr);
        put32(b, ph + 16, static_cast<std::uint32_t>(s.data.size()));
        put32(b, ph + 20, s.memsz != 0 ? s.memsz : static_cast<std::uint32_t>(s.data.size()));
        put32(b, ph + 24, s.flags);
        put32(b, ph + 28, 16);
    }
    if (sections.empty()) return b;

    std::vector<std::uint8_t> names(1, 0);
    std::vector<std::uint32_t> name_offsets, data_offsets;
    for (const auto &s : sections) {
        name_offsets.push_back(static_cast<std::uint32_t>(names.size()));
        names.insert(names.end(), s.name.begin(), s.name.end());
        names.push_back(0);
        align16();
        data_offsets.push_back(static_cast<std::uint32_t>(b.size()));
        if (!s.past_end) b.insert(b.end(), s.data.begin(), s.data.end());
    }
    align16();
    const auto names_offset = static_cast<std::uint32_t>(b.size());
    b.insert(b.end(), names.begin(), names.end());
    const auto shstr_name = static_cast<std::uint32_t>(names.size());
    // The string table names itself last.
    const std::string shstr = ".shstrtab";
    b.insert(b.end(), shstr.begin(), shstr.end());
    b.push_back(0);
    const auto names_size = static_cast<std::uint32_t>(names.size() + shstr.size() + 1);
    align16();
    const auto shoff = static_cast<std::uint32_t>(b.size());
    const std::size_t count = sections.size() + 2; // null + sections + shstrtab
    b.resize(b.size() + 40 * count, 0);
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const auto &s = sections[i];
        const std::size_t sh = shoff + (i + 1) * 40;
        put32(b, sh, name_offsets[i]);
        put32(b, sh + 4, s.type);
        put32(b, sh + 8, 2);
        put32(b, sh + 12, s.addr);
        put32(b, sh + 16, s.past_end ? static_cast<std::uint32_t>(shoff + 40 * count + 64) : data_offsets[i]);
        put32(b, sh + 20, static_cast<std::uint32_t>(s.data.size()));
        put32(b, sh + 24, s.link);
        put32(b, sh + 28, s.info);
        put32(b, sh + 32, 4);
        put32(b, sh + 36, s.entsize);
    }
    const std::size_t sh = shoff + (count - 1) * 40;
    put32(b, sh, shstr_name);
    put32(b, sh + 4, 3);
    put32(b, sh + 16, names_offset);
    put32(b, sh + 20, names_size);
    put32(b, 32, shoff);
    put16(b, 48, static_cast<std::uint32_t>(count));
    put16(b, 50, static_cast<std::uint32_t>(count - 1));
    return b;
}

} // namespace anypsp_test
