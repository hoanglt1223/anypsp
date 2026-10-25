// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// CWCheat cheat files as PPSSPP reads them (docs/user/desktop.md "Cheats"), applied after every
// PSP vblank. A file holds one or more games:
//
//   _S ULUS-10299            disc ID; cheats under other games' IDs are skipped
//   _G Naruto: Ultimate...   title (ignored)
//   _C1 Infinite health      a cheat, on (_C0: off); the menu saves the switch back here
//   _L 0x20123456 0x000003E7 code lines: addresses relative to 0x08800000
//
// Code types follow PPSSPP's Core/CwCheat.cpp (InterpretNextCwCheat, ExecuteOp): writes of 8, 16
// and 32 bits, increment/decrement, OR/AND/XOR, serial (multi) writes, copy bytes, pointer
// writes, the 0xC/0xD/0xE conditions and the joker (button) conditions. A cheat with a code type
// that is not a memory write or condition (PPSSPP's 0xA vibration and shader codes, PSPAR _M and
// TempAR _N lines) or a malformed line is refused with the reason and never runs.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace psprecomp {
class GuestMemory;
}

namespace anypsp::cheats {

struct Line {
    std::uint32_t a = 0, b = 0;
};

struct Cheat {
    std::string name;
    bool enabled = false;
    std::vector<Line> lines;
    std::string error;  // non-empty: refused, never runs ("<why> (line 12: _L ...)")
    int file_line = 0;  // 1-based line of its _C line, where the switch is saved
};

class CheatFile {
public:
    // `game_id` ("ULUS10299" or "ULUS-10299"; empty accepts every game) picks the cheats of
    // this game in a file for several. As in PPSSPP, a file with a single _S line is used
    // even when its ID differs. Lines the format does not know become warnings.
    static CheatFile parse(const std::string &text, const std::string &game_id, const std::filesystem::path &path = {});
    // Throws psprecomp::Error when the file cannot be read.
    static CheatFile load(const std::filesystem::path &path, const std::string &game_id);

    [[nodiscard]] const std::vector<Cheat> &cheats() const { return cheats_; }
    [[nodiscard]] const std::vector<std::string> &warnings() const { return warnings_; }
    [[nodiscard]] const std::filesystem::path &path() const { return path_; }
    // Returns false (and changes nothing) for a refused cheat.
    bool set_enabled(std::size_t index, bool on);
    // Writes the switches back into the file (_C1 / _C0), keeping every other byte. Throws
    // psprecomp::Error when the file changed underneath or cannot be written.
    void save() const;

    // Runs the enabled cheats once. `buttons` are the PSP buttons held (joker codes).
    // Returns how many values changed in memory.
    std::size_t apply(psprecomp::GuestMemory &memory, std::uint32_t buttons) const;

private:
    std::vector<Cheat> cheats_;
    std::vector<std::string> warnings_;
    std::filesystem::path path_;
};

// Runs one cheat's lines (enabled or not). Returns how many values changed.
std::size_t run(const Cheat &cheat, psprecomp::GuestMemory &memory, std::uint32_t buttons);

// The disc ID without its dash ("ULUS10299"), as PPSSPP names cheat files.
[[nodiscard]] std::string file_id(const std::string &disc_id);
// cheats/<ID>.ini (PPSSPP's name, ULUS10299.ini, or ULUS-10299.ini) next to the program, else in
// the per-user data folder; empty when there is none.
[[nodiscard]] std::filesystem::path find_file(const std::string &disc_id, const std::filesystem::path &program_dir,
                                              const std::filesystem::path &user_dir);

} // namespace anypsp::cheats
