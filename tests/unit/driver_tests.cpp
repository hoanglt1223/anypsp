// SPDX-License-Identifier: GPL-2.0-or-later
// `anypsp build` front end: option parsing, input detection, PARAM.SFO, packaging helpers.
#include "test_harness.hpp"

#include "android.h"
#include "build.h"
#include "zip.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace anypsp::driver;

void put32(std::vector<std::uint8_t> &b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// A PARAM.SFO with UTF-8 string entries, laid out as the PSP writes it.
std::vector<std::uint8_t> make_sfo(const std::vector<std::pair<std::string, std::string>> &entries) {
    std::string keys, data;
    std::vector<std::uint8_t> table(entries.size() * 16);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto &[key, value] = entries[i];
        const std::size_t e = i * 16;
        table[e] = static_cast<std::uint8_t>(keys.size());
        table[e + 1] = static_cast<std::uint8_t>(keys.size() >> 8);
        table[e + 2] = 0x04;
        table[e + 3] = 0x02;
        put32(table, e + 4, static_cast<std::uint32_t>(value.size() + 1));
        put32(table, e + 8, static_cast<std::uint32_t>((value.size() + 4) & ~3u));
        put32(table, e + 12, static_cast<std::uint32_t>(data.size()));
        keys += key + '\0';
        data += value;
        data.append(((value.size() + 4) & ~3u) - value.size(), '\0');
    }
    while (keys.size() % 4) keys += '\0';
    std::vector<std::uint8_t> sfo(0x14);
    std::memcpy(sfo.data(), "\0PSF", 4);
    put32(sfo, 4, 0x101);
    put32(sfo, 8, static_cast<std::uint32_t>(0x14 + table.size()));
    put32(sfo, 12, static_cast<std::uint32_t>(0x14 + table.size() + keys.size()));
    put32(sfo, 16, static_cast<std::uint32_t>(entries.size()));
    sfo.insert(sfo.end(), table.begin(), table.end());
    sfo.insert(sfo.end(), keys.begin(), keys.end());
    sfo.insert(sfo.end(), data.begin(), data.end());
    return sfo;
}

void write_bytes(const fs::path &p, const std::vector<std::uint8_t> &bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::uint8_t> fake_elf() {
    std::vector<std::uint8_t> elf(64, 0);
    std::memcpy(elf.data(), "\x7f" "ELF", 4);
    elf[4] = 1;  // 32-bit
    elf[5] = 1;  // little endian
    return elf;
}

fs::path scratch(const char *name) {
    const fs::path dir = fs::temp_directory_path() / "anypsp-driver-tests" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::optional<std::string> parse(std::vector<std::string_view> args, BuildOptions &out) {
    return parse_build_args(std::span<const std::string_view>(args.data(), args.size()), out);
}

} // namespace

TEST_CASE("driver: program names keep ASCII letters, digits, '-', '_' and single spaces") {
    CHECK_EQ(program_name("NARUTO: Ultimate Ninja Heroes"), std::string("NARUTO Ultimate Ninja Heroes"));
    CHECK_EQ(program_name("  a/b\\c  "), std::string("abc"));
    CHECK_EQ(program_name(":::"), std::string("game"));
}

TEST_CASE("driver: PARAM.SFO strings by key") {
    const auto sfo = make_sfo({{"CATEGORY", "UG"}, {"DISC_ID", "ULUS99999"}, {"TITLE", "Test Game"}});
    CHECK_EQ(sfo_string(sfo, "TITLE"), std::string("Test Game"));
    CHECK_EQ(sfo_string(sfo, "DISC_ID"), std::string("ULUS99999"));
    CHECK_EQ(sfo_string(sfo, "MISSING"), std::string());
    CHECK_EQ(sfo_string({1, 2, 3}, "TITLE"), std::string());
}

TEST_CASE("driver: build options") {
    BuildOptions o;
    CHECK(!parse({"game.iso", "-o", "out", "--target", "android", "--abi", "arm64-v8a,x86_64", "--opt", "d", "--jobs", "4",
                  "--no-game", "--embed-game"},
                 o));
    CHECK(o.target == Target::Android);
    CHECK_EQ(o.android.abis.size(), std::size_t{2});
    CHECK_EQ(o.opt, std::string("d"));
    CHECK(o.jobs && *o.jobs == 4);
    CHECK(!o.copy_game);
    CHECK(o.android.embed_game);
    const auto rejects = [](std::vector<std::string_view> args) {
        BuildOptions fresh;
        return parse(std::move(args), fresh).has_value();
    };
    CHECK(rejects({"game.iso", "--opt", "3"}));
    CHECK(rejects({"--target", "android"}));          // no game
    CHECK(rejects({"game.iso", "--target", "ps3"}));  // unknown target
    CHECK(rejects({"game.iso", "--abi", "mips"}));
    // Desktop programs are built on their own OS, except with a cross toolchain.
    const Target other = host_target() == Target::Windows ? Target::Linux : Target::Windows;
    BuildOptions cross;
    CHECK(parse({"game.iso", "--target", target_name(other)}, cross).has_value());
    BuildOptions with_toolchain;
    CHECK(!parse({"game.iso", "--target", target_name(other), "--toolchain", "t.cmake"}, with_toolchain).has_value());
}

TEST_CASE("driver: a lone executable is a module with the default guest path") {
    const fs::path dir = scratch("module");
    write_bytes(dir / "EBOOT.BIN", fake_elf());
    const GameInput g = resolve_input(dir / "EBOOT.BIN", dir / "cache");
    CHECK(g.kind == GameInput::Kind::Module);
    CHECK_EQ(g.guest_path, std::string("ms0:/PSP/GAME/ANYPSP/EBOOT.PBP"));
    std::vector<std::uint8_t> encrypted(0x200, 0);
    std::memcpy(encrypted.data(), "~PSP", 4);
    write_bytes(dir / "ENC.BIN", encrypted);
    CHECK_THROWS(resolve_input(dir / "ENC.BIN", dir / "cache"));
    write_bytes(dir / "notes.txt", {'h', 'i'});
    CHECK_THROWS(resolve_input(dir / "notes.txt", dir / "cache"));
    CHECK_THROWS(resolve_input(dir / "missing.iso", dir / "cache"));
}

TEST_CASE("driver: a disc folder boots its executable and names the game from PARAM.SFO") {
    const fs::path dir = scratch("disc");
    write_bytes(dir / "PSP_GAME" / "SYSDIR" / "EBOOT.BIN", fake_elf());
    write_bytes(dir / "PSP_GAME" / "PARAM.SFO", make_sfo({{"DISC_ID", "ULUS99999"}, {"TITLE", "Disc Game"}}));
    const GameInput g = resolve_input(dir, dir / "cache");
    CHECK(g.kind == GameInput::Kind::DiscFolder);
    CHECK_EQ(g.title, std::string("Disc Game"));
    CHECK_EQ(g.disc_id, std::string("ULUS99999"));
    CHECK_EQ(g.module.filename().string(), std::string("EBOOT.BIN"));
    CHECK_EQ(g.guest_path, std::string("disc0:/PSP_GAME/SYSDIR/EBOOT.BIN"));
}

TEST_CASE("driver: Android package ids and templates") {
    CHECK(android::valid_package_id("io.anyport.anypsp.gabc123"));
    CHECK(!android::valid_package_id("io"));
    CHECK(!android::valid_package_id("io.1abc"));
    CHECK(!android::valid_package_id("io..x"));
    CHECK_EQ(android::default_package_id("0123456789abcdef"), std::string("io.anyport.anypsp.g0123456789ab"));
    CHECK_EQ(android::fill_template("<a p=\"@PACKAGE@\">@LABEL@</a>", {{"PACKAGE", "x.y"}, {"LABEL", android::xml_escape("A&B")}}),
             std::string("<a p=\"x.y\">A&amp;B</a>"));
    CHECK_THROWS(android::fill_template("@MISSING@", {}));
    // The shipped template has exactly the placeholders build_apk fills.
    std::ifstream in(fs::path(ANYPSP_TEST_SOURCE_DIR) / "platform" / "android" / "AndroidManifest.xml");
    const std::string manifest{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    CHECK(!manifest.empty());
    const std::string filled = android::fill_template(manifest, {{"PACKAGE", "x.y"}, {"LABEL", "Game"}});
    CHECK(filled.find("x.y") != std::string::npos);
}

TEST_CASE("driver: ZIP entries survive a rewrite and new ones are stored") {
    const fs::path dir = scratch("zip");
    zip::write(dir / "a.zip", std::nullopt, {{"one.txt", {'1'}}, {"lib/x/libmain.so", {1, 2, 3}}});
    zip::write(dir / "b.zip", dir / "a.zip", {{"two.txt", {'2', '2'}}});
    const auto listed = zip::list(dir / "b.zip");
    CHECK_EQ(listed.size(), std::size_t{3});
    for (const auto &e : listed) CHECK_EQ(e.method, std::uint16_t{0});
}
