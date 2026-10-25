// SPDX-License-Identifier: GPL-2.0-or-later
//
// The file parser and the code types are adapted from PPSSPP's Core/CwCheat.cpp
// (CheatFileParser, CWCheatEngine::InterpretNextCwCheat and ExecuteOp; GPL-2.0-or-later,
// Copyright (c) 2012- PPSSPP Project), see THIRD_PARTY.md.

#include "cheats.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/guest_memory.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

namespace anypsp::cheats {
namespace {

enum class Op {
    Invalid,
    Noop,
    Write,
    Add,
    Subtract,
    Or,
    And,
    Xor,
    MultiWrite,
    Copy,
    Assert,
    IfEqual,
    IfNotEqual,
    IfLess,
    IfGreater,
    IfAddrEqual,
    IfAddrNotEqual,
    IfAddrLess,
    IfAddrGreater,
    IfPressed,
    IfNotPressed,
    Pointer,
};

struct Decoded {
    Op op = Op::Invalid;
    std::uint32_t addr = 0;
    unsigned size = 0;
    std::uint32_t val = 0;
    std::uint32_t count = 0, step = 0, add = 0;  // MultiWrite
    std::uint32_t other = 0;                       // Copy: destination; IfAddr*: compared address
    std::uint32_t skip = 0;                        // If*: code lines skipped when the test fails
    std::int32_t offset = 0, base_offset = 0, lines = 0, type = 0;  // Pointer
    const char *why = "";                          // Invalid
};

// CWCheat addresses are offsets into user memory.
std::uint32_t address(std::uint32_t value) { return (value + 0x08800000u) & 0x3FFFFFFFu; }

Decoded invalid(const char *why) {
    Decoded d;
    d.why = why;
    return d;
}

Decoded make(Op op, std::uint32_t addr, unsigned size, std::uint32_t val) {
    Decoded d;
    d.op = op;
    d.addr = addr;
    d.size = size;
    d.val = val;
    return d;
}

// One code, which may take the lines after it; `i` moves past them.
Decoded decode(const std::vector<Line> &lines, std::size_t &i) {
    const Line &l = lines[i++];
    const std::uint32_t arg = l.b;
    const bool more = i < lines.size();
    switch (l.a >> 28) {
    case 0x0:  // 8-bit write; a larger value writes 16 or 32 bits
        return make(Op::Write, address(l.a & 0x0FFFFFFFu), (arg & 0xFFFF0000u) ? 4u : (arg & 0xFF00u) ? 2u : 1u, arg);
    case 0x1: return make(Op::Write, address(l.a & 0x0FFFFFFFu), 2, arg);
    case 0x2: return make(Op::Write, address(l.a & 0x0FFFFFFFu), 4, arg);
    case 0x3: {  // increment / decrement
        const std::uint32_t addr = address(arg & 0x0FFFFFFFu);
        switch ((l.a >> 20) & 0xFu) {
        case 1: return make(Op::Add, addr, 1, l.a & 0xFFu);
        case 2: return make(Op::Subtract, addr, 1, l.a & 0xFFu);
        case 3: return make(Op::Add, addr, 2, l.a & 0xFFFFu);
        case 4: return make(Op::Subtract, addr, 2, l.a & 0xFFFFu);
        case 5:
        case 6:
            if (!more) return invalid("a 32-bit increment needs a second line");
            return make(((l.a >> 20) & 0xFu) == 5 ? Op::Add : Op::Subtract, addr, 4, lines[i++].a);
        default: return invalid("unknown increment type (0x3 codes take 1-6)");
        }
    }
    case 0x4:  // 32-bit serial write
    case 0x8: {  // 8- or 16-bit serial write
        if (!more) return invalid("a serial write needs a second line");
        const Line &l2 = lines[i++];
        const bool wide = (l.a >> 28) == 0x4;
        const bool byte = !wide && (l2.a & 0xFFFF0000u) == 0;
        const unsigned size = wide ? 4u : byte ? 1u : 2u;
        Decoded d = make(Op::MultiWrite, address(l.a & 0x0FFFFFFFu), size, wide ? l2.a : byte ? (l2.a & 0xFFu) : (l2.a & 0xFFFFu));
        d.count = arg >> 16;
        d.step = (arg & 0xFFFFu) * size;
        d.add = l2.b;
        return d;
    }
    case 0x5: {  // copy bytes
        if (!more) return invalid("a copy needs a second line");
        Decoded d = make(Op::Copy, address(l.a & 0x0FFFFFFFu), 0, arg);
        d.other = address(lines[i++].a & 0x0FFFFFFFu);
        return d;
    }
    case 0x6: {  // pointer commands
        if (!more) return invalid("a pointer code needs a second line");
        const Line &l2 = lines[i++];
        Decoded d = make(Op::Pointer, address(l.a & 0x0FFFFFFFu), 0, arg);
        d.offset = static_cast<std::int32_t>(l2.b);
        d.base_offset = (static_cast<std::int32_t>(l2.a) >> 20) * 4;
        d.lines = std::max(static_cast<std::int32_t>(l2.a & 0xFFFFu) - 1, 0);
        d.type = static_cast<std::int32_t>((l2.a >> 16) & 0xFu);
        if (i + static_cast<std::size_t>(d.lines) > lines.size()) return invalid("a pointer code has fewer lines than it says");
        if (d.type > 5) return invalid("unknown pointer write type (0-5)");
        return d;
    }
    case 0x7: {  // boolean operations
        const std::uint32_t addr = address(l.a & 0x0FFFFFFFu);
        switch (arg >> 16) {
        case 0: return make(Op::Or, addr, 1, arg & 0xFFu);
        case 1: return make(Op::Or, addr, 2, arg & 0xFFFFu);
        case 2: return make(Op::And, addr, 1, arg & 0xFFu);
        case 3: return make(Op::And, addr, 2, arg & 0xFFFFu);
        case 4: return make(Op::Xor, addr, 1, arg & 0xFFu);
        case 5: return make(Op::Xor, addr, 2, arg & 0xFFFFu);
        default: return invalid("unknown boolean operation (0x7 codes take 0-5)");
        }
    }
    case 0xA: return invalid("vibration and post-processing codes (0xA) change no memory and are not supported");
    case 0xB: return make(Op::Noop, 0, 0, 0);  // delay: PPSSPP ignores it too
    case 0xC: return make(Op::Assert, address(l.a & 0x0FFFFFFFu), 4, arg);
    case 0xD:
        switch (arg >> 28) {
        case 0x0:
        case 0x2: {  // skip the next line unless
            static constexpr Op kTests[] = {Op::IfEqual, Op::IfNotEqual, Op::IfLess, Op::IfGreater};
            const std::uint32_t test = (arg >> 20) & 0xFu;
            if (test > 3) return invalid("unknown comparison (0-3)");
            const bool byte = (arg >> 28) == 0x2;
            Decoded d = make(kTests[test], address(l.a & 0x0FFFFFFFu), byte ? 1u : 2u, byte ? (arg & 0xFFu) : (arg & 0xFFFFu));
            d.skip = 1;
            return d;
        }
        case 0x1:
        case 0x3: {  // joker: buttons held / not held
            Decoded d = make((arg >> 28) == 0x1 ? Op::IfPressed : Op::IfNotPressed, 0, 0, arg & 0x0FFFFFFFu);
            d.skip = (l.a & 0xFFu) + 1u;
            return d;
        }
        case 0x4:
        case 0x5:
        case 0x6:
        case 0x7: {  // compare two addresses
            if (!more) return invalid("an address comparison needs a second line");
            static constexpr Op kTests[] = {Op::IfAddrEqual, Op::IfAddrNotEqual, Op::IfAddrLess, Op::IfAddrGreater};
            const Line &l2 = lines[i++];
            const std::uint32_t size = 1u << (l2.b & 0xFu);
            if (size > 4) return invalid("unknown comparison size (0-2)");
            Decoded d = make(kTests[(arg >> 28) - 4], address(l.a & 0x0FFFFFFFu), size, 0);
            d.skip = l2.a;
            d.other = address(arg & 0x0FFFFFFFu);
            return d;
        }
        default: return invalid("unknown 0xD condition");
        }
    case 0xE: {  // skip the next N lines unless
        static constexpr Op kTests[] = {Op::IfEqual, Op::IfNotEqual, Op::IfLess, Op::IfGreater};
        if ((arg >> 28) > 3) return invalid("unknown comparison (0-3)");
        const bool byte = (l.a >> 24) == 0xE1u;
        Decoded d = make(kTests[arg >> 28], address(arg & 0x0FFFFFFFu), byte ? 1u : 2u, byte ? (l.a & 0xFFu) : (l.a & 0xFFFFu));
        d.skip = (l.a >> 16) & (byte ? 0xFFu : 0xFFFu);
        return d;
    }
    default: return invalid("unknown code type");
    }
}

// Why a pointer code's own line cannot run, or null.
const char *pointer_line_error(const Line &l) {
    switch (l.a >> 28) {
    case 0x1:
    case 0x9: return nullptr;
    case 0x2:
    case 0x3: {
        const std::uint32_t second = l.b >> 28;
        return second == 0 || second == 2 || second == 3 ? nullptr : "unknown second pointer step (0, 2 or 3)";
    }
    default: return "unknown pointer line (1, 2, 3 or 9)";
    }
}

// Index of the first code line that cannot run and why, else {size, nullptr}.
std::pair<std::size_t, const char *> check(const std::vector<Line> &lines) {
    for (std::size_t i = 0; i < lines.size();) {
        const std::size_t at = i;
        const Decoded d = decode(lines, i);
        if (d.op == Op::Invalid) return {at, d.why};
        if (d.op == Op::Pointer)
            for (std::int32_t k = 0; k < d.lines; ++k, ++i)
                if (const char *why = pointer_line_error(lines[i])) return {i, why};
    }
    return {lines.size(), nullptr};
}

std::uint32_t read(const psprecomp::GuestMemory &m, std::uint32_t addr, unsigned size) {
    return size == 1u ? m.load8(addr) : size == 2u ? m.load16(addr) : m.load32(addr);
}

// Writes when the value differs; returns 1 when memory changed.
std::size_t put(psprecomp::GuestMemory &m, std::uint32_t addr, unsigned size, std::uint32_t value) {
    if (!m.contains(addr, size)) return 0;
    const std::uint32_t mask = size == 4u ? 0xFFFFFFFFu : (1u << (8u * size)) - 1u;
    value &= mask;
    if (read(m, addr, size) == value) return 0;
    if (size == 1u) m.store8(addr, static_cast<std::uint8_t>(value));
    else if (size == 2u) m.store16(addr, static_cast<std::uint16_t>(value));
    else m.store32(addr, value);
    return 1;
}

std::size_t copy(psprecomp::GuestMemory &m, std::uint32_t to, std::uint32_t from, std::uint32_t bytes) {
    if (bytes == 0 || !m.contains(to, bytes) || !m.contains(from, bytes)) return 0;
    std::vector<std::uint8_t> data(bytes);
    m.copy_out(from, data);
    std::size_t changed = 0;
    for (std::uint32_t k = 0; k < bytes; ++k) changed += put(m, to + k, 1, data[k]);
    return changed;
}

bool test(Op op, std::int32_t a, std::int32_t b) {
    switch (op) {
    case Op::IfEqual:
    case Op::IfAddrEqual: return a == b;
    case Op::IfNotEqual:
    case Op::IfAddrNotEqual: return a != b;
    case Op::IfLess:
    case Op::IfAddrLess: return a < b;
    default: return a > b;
    }
}

std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string without_dashes(std::string s) {
    std::erase(s, '-');
    return s;
}

std::string hex(std::uint32_t v) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08X", v);
    return text;
}

// Splits after each '\n', keeping the line endings, so saving changes nothing else.
std::vector<std::string> raw_lines(const std::string &text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::size_t next = end == std::string::npos ? text.size() : end + 1;
        out.push_back(text.substr(start, next - start));
        start = next;
    }
    return out;
}

std::string read_file(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw psprecomp::Error("Cannot open " + path.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

constexpr std::string_view kBom = "\xEF\xBB\xBF";

} // namespace

std::size_t run(const Cheat &cheat, psprecomp::GuestMemory &m, std::uint32_t buttons) {
    const std::vector<Line> &lines = cheat.lines;
    std::size_t changed = 0;
    for (std::size_t i = 0; i < lines.size();) {
        const Decoded d = decode(lines, i);
        switch (d.op) {
        case Op::Invalid: return changed;  // refused cheats never get here
        case Op::Noop: break;
        case Op::Write: changed += put(m, d.addr, d.size, d.val); break;
        case Op::Add:
        case Op::Subtract:
        case Op::Or:
        case Op::And:
        case Op::Xor: {
            if (!m.contains(d.addr, d.size)) break;
            const std::uint32_t v = read(m, d.addr, d.size);
            const std::uint32_t r = d.op == Op::Add ? v + d.val : d.op == Op::Subtract ? v - d.val : d.op == Op::Or ? (v | d.val)
                                  : d.op == Op::And ? (v & d.val) : (v ^ d.val);
            changed += put(m, d.addr, d.size, r);
            break;
        }
        case Op::MultiWrite: {
            const std::uint64_t span = static_cast<std::uint64_t>(d.count) * d.step + d.size;
            if (span >= 24u * 1024u * 1024u || !m.contains(d.addr, static_cast<std::size_t>(span))) break;
            std::uint32_t addr = d.addr, value = d.val;
            for (std::uint32_t k = 0; k < d.count; ++k, addr += d.step, value += d.add) changed += put(m, addr, d.size, value);
            break;
        }
        case Op::Copy: changed += copy(m, d.other, d.addr, d.val); break;
        case Op::Assert:
            if (m.contains(d.addr, 4) && m.load32(d.addr) != d.val) return changed;
            break;
        case Op::IfEqual:
        case Op::IfNotEqual:
        case Op::IfLess:
        case Op::IfGreater:
            if (!m.contains(d.addr, d.size) ||
                !test(d.op, static_cast<std::int32_t>(read(m, d.addr, d.size)), static_cast<std::int32_t>(d.val)))
                i += d.skip;
            break;
        case Op::IfAddrEqual:
        case Op::IfAddrNotEqual:
        case Op::IfAddrLess:
        case Op::IfAddrGreater:
            if (!m.contains(d.addr, d.size) || !m.contains(d.other, d.size) ||
                !test(d.op, static_cast<std::int32_t>(read(m, d.addr, d.size)), static_cast<std::int32_t>(read(m, d.other, d.size))))
                i += d.skip;
            break;
        case Op::IfPressed:
            if ((buttons & d.val) != d.val) i += d.skip;
            break;
        case Op::IfNotPressed:
            if ((buttons & d.val) == d.val) i += d.skip;
            break;
        case Op::Pointer: {
            // PPSSPP leaves the pointer lines to run as codes when the base is unreadable; they
            // are skipped here, since they are not codes.
            const std::size_t first = i;
            i += static_cast<std::size_t>(d.lines);
            const std::uint32_t base_at = d.addr + static_cast<std::uint32_t>(d.base_offset);
            if (!m.contains(base_at, 4)) break;
            std::uint32_t base = m.load32(base_at), val = d.val;
            std::int32_t type = d.type;
            for (std::size_t k = first; k < i; ++k) {
                const Line &pl = lines[k];
                switch (pl.a >> 28) {
                case 0x1:  // copy bytes from the first pointer to the second
                    if (m.contains(d.addr, 4)) {
                        const std::uint32_t from = m.load32(d.addr) + static_cast<std::uint32_t>(d.offset);
                        const std::uint32_t to = m.load32(base_at) + (pl.a & 0x0FFFFFFFu);
                        changed += copy(m, to, from, val);
                        type = -1;
                    }
                    break;
                case 0x2:
                case 0x3: {  // walk: follow the pointer at base +/- offset, once or twice
                    const auto walk = [&](std::uint32_t word) {
                        const std::uint32_t off = word & 0x0FFFFFFFu;
                        const std::uint32_t at = (word >> 28) == 0x3 ? base - off : base + off;
                        if (!m.contains(at, 4)) return false;
                        base = m.load32(at);
                        return true;
                    };
                    if (walk(pl.a) && ((pl.b >> 28) == 0x2 || (pl.b >> 28) == 0x3)) walk(pl.b);
                    break;
                }
                case 0x9:  // next address and value
                    base += pl.a & 0x0FFFFFFFu;
                    val += pl.b;
                    break;
                default: break;
                }
            }
            if (type >= 0) {
                static constexpr unsigned kSizes[] = {1, 2, 4};
                const std::uint32_t off = static_cast<std::uint32_t>(d.offset);
                const std::uint32_t at = type < 3 ? base + off : base - off;
                changed += put(m, at, kSizes[type % 3], val);
            }
            break;
        }
        }
    }
    return changed;
}

CheatFile CheatFile::parse(const std::string &text, const std::string &game_id, const std::filesystem::path &path) {
    CheatFile file;
    file.path_ = path;
    const std::string want = without_dashes(trim(game_id));
    const std::vector<std::string> raw = raw_lines(text);
    std::vector<std::vector<int>> code_lines;  // file line of each code line, per cheat
    int games = 0;
    bool game_on = true, risky = false;
    Cheat *current = nullptr;
    const auto warn = [&](int n, const std::string &what) { file.warnings_.push_back("line " + std::to_string(n) + ": " + what); };
    const auto refuse = [](Cheat &c, int n, const std::string &why) {
        if (c.error.empty()) c.error = why + " (line " + std::to_string(n) + ")";
    };
    for (std::size_t index = 0; index < raw.size(); ++index) {
        const int n = static_cast<int>(index) + 1;
        std::string line = raw[index];
        if (index == 0 && line.starts_with(kBom)) line.erase(0, kBom.size());
        line = trim(line);
        if (line.empty() || line.starts_with("//") || line.starts_with("#")) continue;
        if (line.size() < 2 || line[0] != '_') {
            warn(n, "expected a line starting with _ (_S, _G, _C0/_C1 or _L)");
            continue;
        }
        switch (line[1]) {
        case 'S': {
            current = nullptr;
            ++games;
            const bool match = want.empty() || without_dashes(trim(line.substr(2))) == want;
            if (match) {
                if (risky) {  // the right game after all: drop the guess
                    file.cheats_.clear();
                    code_lines.clear();
                    risky = false;
                }
                game_on = true;
            } else if (games == 1) {
                // A file for a single game is used even when its ID differs, as in PPSSPP.
                risky = game_on = true;
            } else {
                if (risky) {
                    file.cheats_.clear();
                    code_lines.clear();
                    risky = false;
                }
                game_on = false;
            }
            break;
        }
        case 'G': break;
        case 'C': {
            current = nullptr;
            if (!game_on) break;
            if (line.size() < 3 || !std::isdigit(static_cast<unsigned char>(line[2]))) {
                warn(n, "expected _C0 or _C1 and a name");
                break;
            }
            Cheat c;
            c.enabled = line[2] != '0';
            c.name = trim(line.substr(3));
            c.file_line = n;
            file.cheats_.push_back(std::move(c));
            code_lines.emplace_back();
            current = &file.cheats_.back();
            break;
        }
        case 'L': {
            if (!game_on) break;
            if (current == nullptr) {
                warn(n, "code line outside a cheat (_C0/_C1 first)");
                break;
            }
            unsigned a = 0, b = 0;
            int used = 0;
            const std::string data = line.substr(2);
            if (std::sscanf(data.c_str(), "%x %x %n", &a, &b, &used) != 2) {
                refuse(*current, n, "expected two hex numbers after _L");
                break;
            }
            const std::string rest = trim(data.substr(static_cast<std::size_t>(used)));
            if (!rest.empty() && !rest.starts_with("#") && !rest.starts_with("//")) refuse(*current, n, "unexpected '" + rest + "'");
            current->lines.push_back({a, b});
            code_lines.back().push_back(n);
            break;
        }
        case 'M':
            if (game_on && current != nullptr) refuse(*current, n, "PSP Action Replay (_M) codes are not supported");
            break;
        case 'N':
            if (game_on && current != nullptr) refuse(*current, n, "TempAR (_N) codes are not supported");
            break;
        default: warn(n, "unknown line type " + line.substr(0, 2)); break;
        }
    }
    for (std::size_t c = 0; c < file.cheats_.size(); ++c) {
        Cheat &cheat = file.cheats_[c];
        if (!cheat.error.empty()) continue;
        const auto [at, why] = check(cheat.lines);
        if (why == nullptr) continue;
        const Line &l = cheat.lines[at];
        cheat.error = std::string(why) + " (line " + std::to_string(code_lines[c][at]) + ": _L " + hex(l.a) + " " + hex(l.b) + ")";
    }
    return file;
}

CheatFile CheatFile::load(const std::filesystem::path &path, const std::string &game_id) {
    return parse(read_file(path), game_id, path);
}

bool CheatFile::set_enabled(std::size_t index, bool on) {
    if (index >= cheats_.size() || (on && !cheats_[index].error.empty())) return false;
    cheats_[index].enabled = on;
    return true;
}

void CheatFile::save() const {
    if (path_.empty()) throw psprecomp::Error("The cheats have no file to save to");
    std::vector<std::string> raw = raw_lines(read_file(path_));
    for (const Cheat &c : cheats_) {
        const auto index = static_cast<std::size_t>(c.file_line - 1);
        if (index >= raw.size()) throw psprecomp::Error(path_.string() + " changed since it was loaded");
        std::string &line = raw[index];
        std::size_t at = index == 0 && line.starts_with(kBom) ? kBom.size() : 0;
        at = line.find_first_not_of(" \t", at);
        if (at == std::string::npos || line.compare(at, 2, "_C") != 0 || at + 2 >= line.size() ||
            !std::isdigit(static_cast<unsigned char>(line[at + 2])) || trim(line.substr(at + 3)) != c.name)
            throw psprecomp::Error(path_.string() + " changed since it was loaded (line " + std::to_string(c.file_line) + ")");
        if ((line[at + 2] != '0') != c.enabled) line[at + 2] = c.enabled ? '1' : '0';
    }
    std::string text;
    for (const std::string &line : raw) text += line;
    std::filesystem::path temp = path_;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(text.data(), static_cast<std::streamsize>(text.size())) || !out.flush())
            throw psprecomp::Error("Cannot write " + temp.string());
    }
    std::error_code ec;
    std::filesystem::rename(temp, path_, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        throw psprecomp::Error("Cannot write " + path_.string());
    }
}

std::size_t CheatFile::apply(psprecomp::GuestMemory &memory, std::uint32_t buttons) const {
    std::size_t changed = 0;
    for (const Cheat &c : cheats_)
        if (c.enabled && c.error.empty()) changed += run(c, memory, buttons);
    return changed;
}

std::string file_id(const std::string &disc_id) { return without_dashes(disc_id); }

std::filesystem::path find_file(const std::string &disc_id, const std::filesystem::path &program_dir,
                                const std::filesystem::path &user_dir) {
    if (disc_id.empty()) return {};
    std::error_code ec;
    for (const auto &dir : {program_dir, user_dir}) {
        if (dir.empty()) continue;
        for (const std::string &name : {file_id(disc_id), disc_id}) {
            const auto path = dir / "cheats" / (name + ".ini");
            if (std::filesystem::is_regular_file(path, ec)) return path;
        }
    }
    return {};
}

} // namespace anypsp::cheats
