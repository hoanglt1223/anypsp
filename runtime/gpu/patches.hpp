#pragma once

// Per-game memory patches (docs/user/desktop.md#game-patches): small value writes into guest
// memory, applied once after loading or on every vblank, such as a frame-rate patch. A patch file
// holds addresses and values only, never game code.
//
// One patch per line ('#' starts a comment):
//
//   <when> <size> <address> = <value> [if <old>]
//
//   when     load (once, after the executable is loaded) or vblank (after every PSP vblank)
//   size     u8, u16 or u32
//   address  0x08812345, or [0x08C20C70]+0x1: the 32-bit pointer stored at 0x08C20C70 plus 1
//            (skipped while that pointer is not in user memory)
//   value    the value written (hex or decimal)
//   if old   write only while the current value is <old>, so values the game sets for other
//            reasons stay alone

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace psprecomp {
class GuestMemory;
}

namespace anypsp::patches {

enum class When { Load, Vblank };

struct Patch {
    When when = When::Load;
    unsigned size = 4;            // bytes: 1, 2 or 4
    std::uint32_t address = 0;    // or the pointer's address when `pointer`
    bool pointer = false;
    std::uint32_t offset = 0;     // added to the pointer
    std::uint32_t value = 0;
    std::optional<std::uint32_t> only_if;
};

// Throws psprecomp::Error naming `source` and the line on any mistake.
[[nodiscard]] std::vector<Patch> parse(const std::string &text, const std::string &source);
[[nodiscard]] std::vector<Patch> load(const std::filesystem::path &path);

// Applies the patches of kind `when`. Returns how many values were written.
std::size_t apply(const std::vector<Patch> &patches, When when, psprecomp::GuestMemory &memory);

// The disc ID in an extracted disc's UMD_DATA.BIN ("ULUS-10299"), or empty when there is none.
[[nodiscard]] std::string disc_id(const std::filesystem::path &disc_root);
// patches/<disc id>.ini next to the program, else in the per-user data folder; empty when neither exists.
[[nodiscard]] std::filesystem::path find_file(const std::string &disc_id, const std::filesystem::path &program_dir,
                                              const std::filesystem::path &user_dir);

} // namespace anypsp::patches
