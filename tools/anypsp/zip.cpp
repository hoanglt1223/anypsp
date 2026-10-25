// SPDX-License-Identifier: MIT
// From AnyGBA (tools/anygba/zip.cpp), Copyright (c) 2026 Henry Nguyen; see THIRD_PARTY.md.
#include "zip.h"

#include <array>
#include <span>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>

namespace anypsp::driver::zip {

namespace fs = std::filesystem;

namespace {

std::uint32_t crc32(std::span<const std::uint8_t> data) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const std::uint8_t b : data) crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

constexpr std::uint32_t kLocalSig = 0x04034b50, kCentralSig = 0x02014b50, kEndSig = 0x06054b50,
                        kDescriptorSig = 0x08074b50;
constexpr std::uint16_t kDosDate = (0 << 9) | (1 << 5) | 1;  // 1980-01-01: reproducible archives

std::uint16_t u16(const std::vector<std::uint8_t>& b, std::size_t at) {
    if (at + 2 > b.size()) throw std::runtime_error("truncated ZIP");
    return static_cast<std::uint16_t>(b[at] | b[at + 1] << 8);
}

std::uint32_t u32(const std::vector<std::uint8_t>& b, std::size_t at) {
    if (at + 4 > b.size()) throw std::runtime_error("truncated ZIP");
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
           static_cast<std::uint32_t>(b[at + 2]) << 16 | static_cast<std::uint32_t>(b[at + 3]) << 24;
}

void put16(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}

std::vector<std::uint8_t> read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + p.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct Central {
    std::string name;
    std::vector<std::uint8_t> header;  // the whole central record, offset patched on write
    std::size_t local_offset = 0;
    std::size_t local_length = 0;  // local header + data + descriptor
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint32_t size = 0;
};

std::vector<Central> read_central(const std::vector<std::uint8_t>& b, const fs::path& source) {
    if (b.size() < 22) throw std::runtime_error(source.string() + " is not a ZIP archive");
    std::size_t end = std::string::npos;
    for (std::size_t i = b.size() - 22 + 1; i-- > 0 && b.size() - i <= 22 + 0xFFFF;) {
        if (u32(b, i) == kEndSig) {
            end = i;
            break;
        }
    }
    if (end == std::string::npos) throw std::runtime_error(source.string() + " is not a ZIP archive");
    const std::size_t count = u16(b, end + 10);
    std::size_t at = u32(b, end + 16);
    if (count == 0xFFFF || at == 0xFFFFFFFF) throw std::runtime_error(source.string() + ": ZIP64 is not supported");

    std::vector<Central> out;
    for (std::size_t i = 0; i < count; ++i) {
        if (u32(b, at) != kCentralSig) throw std::runtime_error(source.string() + ": damaged central directory");
        Central c;
        const std::uint16_t flags = u16(b, at + 8);
        c.method = u16(b, at + 10);
        c.crc = u32(b, at + 16);
        const std::uint32_t compressed = u32(b, at + 20);
        c.size = u32(b, at + 24);
        const std::size_t name_len = u16(b, at + 28), extra_len = u16(b, at + 30), comment_len = u16(b, at + 32);
        c.local_offset = u32(b, at + 42);
        const std::size_t record = 46 + name_len + extra_len + comment_len;
        if (at + record > b.size()) throw std::runtime_error(source.string() + ": damaged central directory");
        c.name.assign(reinterpret_cast<const char*>(b.data() + at + 46), name_len);
        c.header.assign(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + record));
        if (u32(b, c.local_offset) != kLocalSig) throw std::runtime_error(source.string() + ": damaged entry " + c.name);
        std::size_t length = 30 + u16(b, c.local_offset + 26) + u16(b, c.local_offset + 28) + compressed;
        if (flags & 0x8) {  // sizes follow the data in a descriptor, with or without its signature
            length += c.local_offset + length + 4 <= b.size() && u32(b, c.local_offset + length) == kDescriptorSig ? 16 : 12;
        }
        if (c.local_offset + length > b.size()) throw std::runtime_error(source.string() + ": damaged entry " + c.name);
        c.local_length = length;
        out.push_back(std::move(c));
        at += record;
    }
    return out;
}

}  // namespace

void write(const fs::path& out, const std::optional<fs::path>& base, const std::vector<Entry>& entries) {
    std::set<std::string> replaced;
    for (const auto& e : entries) {
        if (e.name.empty() || e.name.size() > 0xFFFF || e.data.size() >= 0xFFFFFFFFull) {
            throw std::runtime_error("cannot store '" + e.name + "' in a ZIP archive");
        }
        replaced.insert(e.name);
    }

    std::vector<std::uint8_t> body;
    std::vector<std::uint8_t> central;
    std::size_t count = 0;
    const auto check_offset = [&body]() {
        if (body.size() >= 0xFFFFFFFFull) throw std::runtime_error("archive too large (ZIP64 is not supported)");
        return static_cast<std::uint32_t>(body.size());
    };

    if (base) {
        const std::vector<std::uint8_t> b = read_all(*base);
        for (Central& c : read_central(b, *base)) {
            if (replaced.contains(c.name)) continue;
            const std::uint32_t offset = check_offset();
            body.insert(body.end(), b.begin() + static_cast<std::ptrdiff_t>(c.local_offset),
                        b.begin() + static_cast<std::ptrdiff_t>(c.local_offset + c.local_length));
            for (int i = 0; i < 4; ++i) c.header[42 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(offset >> (8 * i));
            central.insert(central.end(), c.header.begin(), c.header.end());
            ++count;
        }
    }

    for (const auto& e : entries) {
        const std::uint32_t offset = check_offset();
        const std::uint32_t crc = crc32(e.data);
        const auto size = static_cast<std::uint32_t>(e.data.size());
        const auto name_len = static_cast<std::uint32_t>(e.name.size());

        put32(body, kLocalSig);
        put16(body, 10);  // version needed: 1.0
        put16(body, 0x0800);  // UTF-8 names
        put16(body, 0);  // stored
        put16(body, 0);
        put16(body, kDosDate);
        put32(body, crc);
        put32(body, size);
        put32(body, size);
        put16(body, name_len);
        put16(body, 0);
        body.insert(body.end(), e.name.begin(), e.name.end());
        body.insert(body.end(), e.data.begin(), e.data.end());

        put32(central, kCentralSig);
        put16(central, 20);  // made by: MS-DOS / 2.0
        put16(central, 10);
        put16(central, 0x0800);
        put16(central, 0);
        put16(central, 0);
        put16(central, kDosDate);
        put32(central, crc);
        put32(central, size);
        put32(central, size);
        put16(central, name_len);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put32(central, 0);
        put32(central, offset);
        central.insert(central.end(), e.name.begin(), e.name.end());
        ++count;
    }
    if (count > 0xFFFE) throw std::runtime_error("too many ZIP entries (ZIP64 is not supported)");

    const std::uint32_t central_offset = check_offset();
    std::vector<std::uint8_t> end;
    put32(end, kEndSig);
    put16(end, 0);
    put16(end, 0);
    put16(end, static_cast<std::uint32_t>(count));
    put16(end, static_cast<std::uint32_t>(count));
    put32(end, static_cast<std::uint32_t>(central.size()));
    put32(end, central_offset);
    put16(end, 0);

    if (out.has_parent_path()) fs::create_directories(out.parent_path());
    std::ofstream file(out, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
    file.write(reinterpret_cast<const char*>(central.data()), static_cast<std::streamsize>(central.size()));
    file.write(reinterpret_cast<const char*>(end.data()), static_cast<std::streamsize>(end.size()));
    file.close();
    if (!file) throw std::runtime_error("cannot write " + out.string());
}

std::vector<Listed> list(const fs::path& archive) {
    std::vector<Listed> out;
    for (const Central& c : read_central(read_all(archive), archive)) {
        out.push_back({c.name, c.method, c.crc, c.size});
    }
    return out;
}

std::vector<Entry> directory_entries(const fs::path& dir, const std::string& prefix) {
    std::vector<Entry> out;
    for (const auto& item : fs::recursive_directory_iterator(dir)) {
        if (!item.is_regular_file()) continue;
        const std::u8string rel = item.path().lexically_relative(dir).generic_u8string();
        out.push_back({prefix + std::string(rel.begin(), rel.end()), read_all(item.path())});
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
    return out;
}

}  // namespace anypsp::driver::zip
