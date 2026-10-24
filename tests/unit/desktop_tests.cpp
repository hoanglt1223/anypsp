// SPDX-License-Identifier: GPL-2.0-or-later
// Desktop host pieces that need no window or sound device: disc images,
// input bindings, the WAV recorder and the audio backlog limit.

#include "test_harness.hpp"

#include "desktop.hpp"
#include "disc.hpp"
#include "input_map.hpp"
#include "kernel.hpp"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using anypsp::input::Action;

namespace {

fs::path scratch(const std::string &name) {
    const fs::path dir = fs::temp_directory_path() / ("anypsp-desktop-tests-" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void write_file(const fs::path &path, const std::string &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << bytes;
}

std::string read_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void put32(std::vector<std::uint8_t> &v, std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) v[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::vector<std::uint8_t> record(const std::string &name, std::uint32_t extent, std::uint32_t size, bool directory) {
    std::vector<std::uint8_t> r(33u + name.size() + (name.size() % 2u == 0u ? 1u : 0u), 0u);
    r[0] = static_cast<std::uint8_t>(r.size());
    put32(r, 2, extent);
    put32(r, 10, size);
    r[25] = directory ? 2u : 0u;
    r[32] = static_cast<std::uint8_t>(name.size());
    std::memcpy(&r[33], name.data(), name.size());
    return r;
}

// A minimal ISO 9660 image with PSP_GAME/SYSDIR/{BOOT.BIN,EBOOT.BIN} and UMD_DATA.BIN.
void make_iso(const fs::path &path, const std::string &boot, const std::string &eboot) {
    constexpr std::size_t kSector = 2048u;
    std::vector<std::uint8_t> image(24u * kSector, 0u);
    const auto directory = [&](std::uint32_t sector, std::uint32_t self, std::uint32_t parent,
                               const std::vector<std::vector<std::uint8_t>> &entries) {
        std::size_t at = sector * kSector;
        for (const auto &r : {record(std::string(1, '\0'), self, kSector, true), record(std::string(1, '\1'), parent, kSector, true)}) {
            std::memcpy(&image[at], r.data(), r.size());
            at += r.size();
        }
        for (const auto &r : entries) {
            std::memcpy(&image[at], r.data(), r.size());
            at += r.size();
        }
    };
    image[16 * kSector] = 1u;
    std::memcpy(&image[16 * kSector + 1], "CD001", 5);
    const auto root = record(std::string(1, '\0'), 18u, kSector, true);
    std::memcpy(&image[16 * kSector + 156], root.data(), root.size());
    directory(18u, 18u, 18u, {record("PSP_GAME", 19u, kSector, true), record("UMD_DATA.BIN;1", 23u, 4u, false)});
    directory(19u, 19u, 18u, {record("SYSDIR", 20u, kSector, true)});
    directory(20u, 20u, 19u,
              {record("BOOT.BIN;1", 21u, static_cast<std::uint32_t>(boot.size()), false),
               record("EBOOT.BIN;1", 22u, static_cast<std::uint32_t>(eboot.size()), false)});
    std::memcpy(&image[21 * kSector], boot.data(), boot.size());
    std::memcpy(&image[22 * kSector], eboot.data(), eboot.size());
    std::memcpy(&image[23 * kSector], "ULUS", 4);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(image.data()), static_cast<std::streamsize>(image.size()));
}

const std::string kElf = std::string("\x7F" "ELF", 4) + "rest of an elf";
const std::string kEncrypted = "~PSP encrypted";

bool none(int) { return false; }

} // namespace

TEST_CASE("ISO images extract with their directory tree and file contents") {
    const fs::path dir = scratch("extract");
    make_iso(dir / "game.iso", kElf, kEncrypted);
    anypsp::disc::extract_iso(dir / "game.iso", dir / "out");
    CHECK(read_file(dir / "out/PSP_GAME/SYSDIR/BOOT.BIN") == kElf);
    CHECK(read_file(dir / "out/PSP_GAME/SYSDIR/EBOOT.BIN") == kEncrypted);
    CHECK(read_file(dir / "out/UMD_DATA.BIN") == "ULUS");
}

TEST_CASE("A file that is not an ISO image is rejected") {
    const fs::path dir = scratch("not-iso");
    write_file(dir / "x.iso", std::string(40000, 'x'));
    CHECK_THROWS(anypsp::disc::extract_iso(dir / "x.iso", dir / "out"));
}

TEST_CASE("Booting an ISO picks the unencrypted BOOT.BIN and caches the extraction") {
    const fs::path dir = scratch("resolve-iso");
    make_iso(dir / "game.iso", kElf, kEncrypted);
    const auto target = anypsp::disc::resolve(dir / "game.iso", dir / "cache");
    CHECK(target.guest_path == "disc0:/PSP_GAME/SYSDIR/BOOT.BIN");
    CHECK(read_file(target.module) == kElf);
    CHECK(fs::exists(target.disc_root / "UMD_DATA.BIN"));
    // A second run reuses the extraction instead of writing it again.
    write_file(target.disc_root / "marker-left-alone", "x");
    const auto again = anypsp::disc::resolve(dir / "game.iso", dir / "cache");
    CHECK(again.disc_root == target.disc_root);
    CHECK(fs::exists(again.disc_root / "marker-left-alone"));
}

TEST_CASE("A disc folder falls back to a decrypted EBOOT.BIN and refuses encrypted ones") {
    const fs::path dir = scratch("resolve-folder");
    write_file(dir / "PSP_GAME/SYSDIR/BOOT.BIN", std::string(16, '\0'));
    write_file(dir / "PSP_GAME/SYSDIR/EBOOT.BIN", kElf);
    const auto target = anypsp::disc::resolve(dir, dir / "cache");
    CHECK(target.guest_path == "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN");
    CHECK(target.disc_root == dir);
    CHECK(anypsp::disc::resolve(dir / "PSP_GAME", dir / "cache").disc_root == dir);

    write_file(dir / "PSP_GAME/SYSDIR/EBOOT.BIN", kEncrypted);
    bool mentions_encrypted = false;
    try {
        (void)anypsp::disc::find_boot(dir);
    } catch (const std::exception &e) {
        mentions_encrypted = std::string(e.what()).find("encrypted") != std::string::npos;
    }
    CHECK(mentions_encrypted);
    CHECK_THROWS(anypsp::disc::find_boot(scratch("empty")));
}

TEST_CASE("Default bindings match the documented keys") {
    const anypsp::input::InputMap map;
    CHECK(map.keys(Action::Cross) == std::vector<int>{SDL_SCANCODE_Z});
    CHECK(map.keys(Action::Circle) == std::vector<int>{SDL_SCANCODE_X});
    CHECK(map.keys(Action::Square) == std::vector<int>{SDL_SCANCODE_A});
    CHECK(map.keys(Action::Triangle) == std::vector<int>{SDL_SCANCODE_S});
    CHECK(map.keys(Action::L) == std::vector<int>{SDL_SCANCODE_Q});
    CHECK(map.keys(Action::Start) == std::vector<int>{SDL_SCANCODE_RETURN});
    CHECK(map.keys(Action::Select) == std::vector<int>{SDL_SCANCODE_BACKSPACE});
    CHECK(map.keys(Action::AnalogLeft) == std::vector<int>{SDL_SCANCODE_J});
    CHECK(map.buttons(Action::Cross) == std::vector<int>{SDL_GAMEPAD_BUTTON_SOUTH});
}

TEST_CASE("Held keys, gamepad buttons and the stick become the PSP pad") {
    const anypsp::input::InputMap map;
    const std::set<int> keys{SDL_SCANCODE_Z, SDL_SCANCODE_UP, SDL_SCANCODE_L};
    const auto key = [&](int k) { return keys.count(k) != 0u; };
    const auto button = [](int b) { return b == SDL_GAMEPAD_BUTTON_START; };
    auto pad = map.pad(key, button, 0, -32768);
    CHECK_EQ(pad.buttons, pspweb::pad::kCross | pspweb::pad::kUp | pspweb::pad::kStart);
    CHECK_EQ(pad.lx, 255u); // L key pushes the stick right
    CHECK_EQ(pad.ly, 0u);   // stick fully up
    pad = map.pad(none, none, 3000, -3000); // inside the deadzone
    CHECK_EQ(pad.buttons, 0u);
    CHECK_EQ(pad.lx, 128u);
    CHECK_EQ(pad.ly, 128u);
    CHECK_EQ(map.pad(none, none, 32767, 0).lx, 255u);
}

TEST_CASE("An input file overrides only the names it lists") {
    const fs::path dir = scratch("ini");
    write_file(dir / "in.ini", "# comment\n[keyboard]\nCross = Space, K\nSelect =\n[gamepad]\nCircle = RightTrigger\n[player]\nScale = 3\nStickDeadzone = 100\n");
    anypsp::input::InputMap map;
    map.load(dir / "in.ini");
    CHECK((map.keys(Action::Cross) == std::vector<int>{SDL_SCANCODE_SPACE, SDL_SCANCODE_K}));
    CHECK(map.keys(Action::Select).empty());
    CHECK(map.keys(Action::Circle) == std::vector<int>{SDL_SCANCODE_X});
    CHECK(map.buttons(Action::Circle) == std::vector<int>{anypsp::input::kRightTrigger});
    CHECK_EQ(map.scale(), 3);
    CHECK_EQ(map.stick_deadzone(), 100);

    // What to_ini writes loads back to the same bindings.
    map.save(dir / "round.ini");
    anypsp::input::InputMap reloaded;
    reloaded.load(dir / "round.ini");
    for (int a = 0; a < static_cast<int>(Action::Count); ++a) {
        CHECK(reloaded.keys(static_cast<Action>(a)) == map.keys(static_cast<Action>(a)));
        CHECK(reloaded.buttons(static_cast<Action>(a)) == map.buttons(static_cast<Action>(a)));
    }
    CHECK_EQ(reloaded.scale(), 3);
}

TEST_CASE("Mistakes in an input file name the file and line") {
    const fs::path dir = scratch("ini-bad");
    const auto error = [&](const std::string &text) {
        write_file(dir / "bad.ini", text);
        anypsp::input::InputMap map;
        try {
            map.load(dir / "bad.ini");
        } catch (const std::exception &e) {
            return std::string(e.what());
        }
        return std::string();
    };
    CHECK(error("[keyboard]\nCross = NoSuchKey\n").find("bad.ini:2: unknown key") != std::string::npos);
    CHECK(error("[gamepad]\nCross = Bumper\n").find("unknown gamepad button") != std::string::npos);
    CHECK(error("[keybord]\n").find("unknown section") != std::string::npos);
    CHECK(error("[keyboard]\nJump = Z\n").find("unknown name") != std::string::npos);
    CHECK(error("[player]\nScale = 9\n").find("Scale is 1-4") != std::string::npos);
    CHECK(error("Cross = Z\n").find("outside a section") != std::string::npos);
}

TEST_CASE("The WAV recorder writes a valid 44.1 kHz stereo header") {
    const fs::path dir = scratch("wav");
    const std::int16_t samples[] = {1, -1, 2, -2, 3, -3};
    {
        anypsp::desktop::WavWriter wav(dir / "a.wav");
        wav.write(samples, 3);
        CHECK_EQ(wav.frames(), 3u);
    }
    const std::string bytes = read_file(dir / "a.wav");
    CHECK_EQ(bytes.size(), 44u + 12u);
    CHECK(bytes.compare(0, 4, "RIFF") == 0 && bytes.compare(8, 8, "WAVEfmt ") == 0 && bytes.compare(36, 4, "data") == 0);
    std::uint32_t data_size = 0, rate = 0;
    std::memcpy(&data_size, &bytes[40], 4);
    std::memcpy(&rate, &bytes[24], 4);
    CHECK_EQ(data_size, 12u);
    CHECK_EQ(rate, 44100u);
    std::int16_t last = 0;
    std::memcpy(&last, &bytes[54], 2);
    CHECK_EQ(last, -3);
}

TEST_CASE("Audio output stops queueing past 100 ms of backlog") {
    CHECK(anypsp::desktop::accept_audio(0u));
    CHECK(anypsp::desktop::accept_audio(anypsp::desktop::kMaxQueuedBytes));
    CHECK(!anypsp::desktop::accept_audio(anypsp::desktop::kMaxQueuedBytes + 1u));
    CHECK_EQ(anypsp::desktop::kMaxQueuedBytes, 17640u);
}
