// SPDX-License-Identifier: GPL-2.0-or-later

#include "nid_db.hpp"

#include "psprecomp/common.hpp"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#ifndef ANYPSP_DATA_DIR
#error "ANYPSP_DATA_DIR must name the data/ directory"
#endif

namespace anypsp::loader {
namespace {

std::uint32_t rotl(std::uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); }

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

bool valid_identifier(std::string_view text) {
    if (text.empty()) return false;
    for (const char c : text)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

} // namespace

std::array<std::uint8_t, 20> sha1(std::string_view data) {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::string message(data);
    const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8u;
    message.push_back(static_cast<char>(0x80));
    while (message.size() % 64u != 56u) message.push_back('\0');
    for (int i = 7; i >= 0; --i) message.push_back(static_cast<char>((bit_length >> (i * 8)) & 0xFFu));

    for (std::size_t chunk = 0; chunk < message.size(); chunk += 64u) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const auto *p = reinterpret_cast<const unsigned char *>(message.data() + chunk + static_cast<std::size_t>(i) * 4u);
            w[i] = (static_cast<std::uint32_t>(p[0]) << 24u) | (static_cast<std::uint32_t>(p[1]) << 16u) |
                   (static_cast<std::uint32_t>(p[2]) << 8u) | p[3];
        }
        for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const std::uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::array<std::uint8_t, 20> digest{};
    for (int i = 0; i < 20; ++i) digest[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(h[i / 4] >> (24 - (i % 4) * 8));
    return digest;
}

std::uint32_t nid_from_name(std::string_view name) {
    const auto digest = sha1(name);
    return static_cast<std::uint32_t>(digest[0]) | (static_cast<std::uint32_t>(digest[1]) << 8u) |
           (static_cast<std::uint32_t>(digest[2]) << 16u) | (static_cast<std::uint32_t>(digest[3]) << 24u);
}

std::size_t load_nid_toml(const std::filesystem::path &path, psprecomp::NidRegistry &registry) {
    std::ifstream in(path);
    if (!in) throw psprecomp::Error("Cannot open NID database " + path.string());
    std::map<std::pair<std::string, std::uint32_t>, std::string> seen;
    std::string library;
    std::string raw;
    std::size_t added = 0;
    for (std::size_t number = 1; std::getline(in, raw); ++number) {
        const auto fail = [&](const std::string &why) {
            throw psprecomp::Error(path.string() + ":" + std::to_string(number) + ": " + why);
        };
        auto line = trim(raw);
        if (line.empty() || line.front() == '#') continue;
        if (line.front() == '[') {
            if (line.size() < 5u || line.substr(0, 2) != "[\"" || line.substr(line.size() - 2u) != "\"]")
                fail("expected a table header like [\"ThreadManForUser\"]");
            const auto name = line.substr(2, line.size() - 4u);
            if (!valid_identifier(name)) fail("invalid library name");
            library.assign(name);
            continue;
        }
        if (library.empty()) fail("entry before the first library table");
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) fail("expected 0xNNNNNNNN = \"name\"");
        const auto key = trim(line.substr(0, equals));
        const auto value = trim(line.substr(equals + 1u));
        if (key.size() != 10u || key[0] != '0' || (key[1] != 'x' && key[1] != 'X')) fail("NID must be written as 0x and 8 hex digits");
        std::uint32_t nid = 0;
        const auto parsed = std::from_chars(key.data() + 2, key.data() + key.size(), nid, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size()) fail("NID is not hexadecimal");
        if (value.size() < 3u || value.front() != '"' || value.back() != '"') fail("name must be a quoted string");
        const auto name = value.substr(1, value.size() - 2u);
        if (!valid_identifier(name)) fail("invalid function name");
        const auto [it, inserted] = seen.try_emplace({library, nid}, std::string(name));
        if (!inserted) {
            if (it->second != name) fail(library + " NID listed twice with different names");
            continue;
        }
        registry.add(library, nid, std::string(name));
        ++added;
    }
    return added;
}

namespace {

std::filesystem::path executable_dir() {
    std::error_code ec;
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (n == 0 || n >= buffer.size()) return {};
    buffer.resize(n);
    return std::filesystem::path(buffer).parent_path();
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    return std::filesystem::path(buffer.c_str()).parent_path();
#else
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path() : exe.parent_path();
#endif
}

} // namespace

std::filesystem::path default_nid_database() {
    if (const char *path = std::getenv("ANYPSP_NID_DATABASE"); path != nullptr && *path != '\0')
        return std::filesystem::path(reinterpret_cast<const char8_t *>(path));
    const std::filesystem::path built_from = std::filesystem::path(ANYPSP_DATA_DIR) / "nids.toml";
    std::error_code ec;
    if (std::filesystem::exists(built_from, ec)) return built_from;
    // Installed (<prefix>/bin next to <prefix>/share/anypsp) or packaged (next to the program).
    const std::filesystem::path dir = executable_dir();
    if (!dir.empty()) {
        for (const auto &candidate : {dir / "nids.toml", dir.parent_path() / "share" / "anypsp" / "nids.toml"})
            if (std::filesystem::exists(candidate, ec)) return candidate;
    }
    return built_from;
}

} // namespace anypsp::loader
