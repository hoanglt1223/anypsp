// SPDX-License-Identifier: GPL-2.0-or-later

#include "patches.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/guest_memory.hpp"

#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace anypsp::patches {
namespace {

// Pointers are only followed into user memory, where a game's own objects live.
constexpr std::uint32_t kUserFirst = 0x08800000u, kUserEnd = 0x0A000000u;

std::uint32_t number(const std::string &text, const std::string &where) {
    try {
        std::size_t used = 0;
        const unsigned long v = std::stoul(text, &used, 0);
        if (used == text.size() && v <= 0xFFFFFFFFul) return static_cast<std::uint32_t>(v);
    } catch (const std::logic_error &) {
    }
    throw psprecomp::Error(where + "'" + text + "' is not a number");
}

std::uint32_t read(psprecomp::GuestMemory &m, std::uint32_t address, unsigned size) {
    return size == 1u ? m.load8(address) : size == 2u ? m.load16(address) : m.load32(address);
}

bool in_user_memory(std::uint32_t address, unsigned size) { return address >= kUserFirst && address <= kUserEnd - size; }

} // namespace

std::vector<Patch> parse(const std::string &text, const std::string &source) {
    std::vector<Patch> patches;
    std::istringstream in(text);
    std::string line;
    for (int number_line = 1; std::getline(in, line); ++number_line) {
        const std::string where = source + ":" + std::to_string(number_line) + ": ";
        line = line.substr(0, line.find('#'));
        std::istringstream words(line);
        std::string when, size, address, equals, value, word, old;
        if (!(words >> when)) continue;
        if (!(words >> size >> address >> equals >> value) || equals != "=")
            throw psprecomp::Error(where + "expected <load|vblank> <u8|u16|u32> <address> = <value> [if <old>]");
        Patch p;
        if (when == "load") p.when = When::Load;
        else if (when == "vblank") p.when = When::Vblank;
        else throw psprecomp::Error(where + "'" + when + "' is not load or vblank");
        if (size == "u8") p.size = 1;
        else if (size == "u16") p.size = 2;
        else if (size == "u32") p.size = 4;
        else throw psprecomp::Error(where + "'" + size + "' is not u8, u16 or u32");
        if (address.front() == '[') {
            const auto close = address.find(']');
            if (close == std::string::npos) throw psprecomp::Error(where + "unterminated [pointer]");
            p.pointer = true;
            p.address = number(address.substr(1, close - 1), where);
            const std::string rest = address.substr(close + 1);
            if (!rest.empty()) {
                if (rest.front() != '+') throw psprecomp::Error(where + "expected [pointer]+offset");
                p.offset = number(rest.substr(1), where);
            }
        } else {
            p.address = number(address, where);
        }
        if (!in_user_memory(p.address, p.pointer ? 4u : p.size))
            throw psprecomp::Error(where + "address outside user memory (0x08800000-0x09FFFFFF)");
        if (p.address % (p.pointer ? 4u : p.size) != 0u) throw psprecomp::Error(where + "misaligned address");
        p.value = number(value, where);
        if (p.size < 4u && p.value >> (8u * p.size) != 0u) throw psprecomp::Error(where + "value too large for " + size);
        if (words >> word) {
            if (word != "if" || !(words >> old)) throw psprecomp::Error(where + "expected 'if <old value>'");
            p.only_if = number(old, where);
        }
        if (words >> word) throw psprecomp::Error(where + "unexpected '" + word + "'");
        patches.push_back(p);
    }
    return patches;
}

std::vector<Patch> load(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw psprecomp::Error("Cannot open " + path.string());
    return parse(std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}, path.string());
}

std::size_t apply(const std::vector<Patch> &patches, When when, psprecomp::GuestMemory &memory) {
    std::size_t written = 0;
    for (const Patch &p : patches) {
        if (p.when != when) continue;
        std::uint32_t address = p.address;
        if (p.pointer) {
            address = memory.load32(p.address) + p.offset;
            if (!in_user_memory(address, p.size) || address % p.size != 0u) continue; // not set up yet
        }
        const std::uint32_t current = read(memory, address, p.size);
        if (current == p.value || (p.only_if && current != *p.only_if)) continue;
        if (p.size == 1u) memory.store8(address, static_cast<std::uint8_t>(p.value));
        else if (p.size == 2u) memory.store16(address, static_cast<std::uint16_t>(p.value));
        else memory.store32(address, p.value);
        ++written;
    }
    return written;
}

std::string disc_id(const std::filesystem::path &disc_root) {
    std::ifstream in(disc_root / "UMD_DATA.BIN", std::ios::binary);
    std::string id;
    if (!in || !std::getline(in, id, '|')) return {};
    for (const char c : id)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') return {};
    return id;
}

std::filesystem::path find_file(const std::string &disc_id, const std::filesystem::path &program_dir,
                                const std::filesystem::path &user_dir) {
    if (disc_id.empty()) return {};
    std::error_code ec;
    for (const auto &dir : {program_dir, user_dir}) {
        if (dir.empty()) continue;
        const auto path = dir / "patches" / (disc_id + ".ini");
        if (std::filesystem::exists(path, ec)) return path;
    }
    return {};
}

} // namespace anypsp::patches
