#pragma once

// Display settings of the desktop host (docs/user/desktop.md): the anypsp-display.ini file,
// its command-line overrides, and the decisions of smoothing. Independent of SDL and GL so it
// can be unit tested.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace anypsp::display {

struct Settings {
    // A 50/50 blend on the vblank between two frames of a game drawing at 30 fps.
    bool smoothing = false;

    bool operator==(const Settings &) const = default;
};

inline constexpr const char *kFileName = "anypsp-display.ini";

// Applies `text` (ini format, section [display]) over `settings`. Throws psprecomp::Error
// naming `source` and the line on any mistake.
void apply_ini(Settings &settings, const std::string &text, const std::string &source);
[[nodiscard]] std::string to_ini(const Settings &settings);
// One setting by its ini name (case-insensitive), as the command line passes it.
void apply_setting(Settings &settings, const std::string &name, const std::string &value, const std::string &where);

// The settings file next to the program, else the one in the per-user data folder (which is
// also where changes made while playing are saved).
[[nodiscard]] std::filesystem::path find_file(const std::filesystem::path &program_dir, const std::filesystem::path &user_dir);
[[nodiscard]] Settings load(const std::filesystem::path &path); // defaults when it does not exist
void save(const std::filesystem::path &path, const Settings &settings);

// Mean absolute difference of two equally sized RGBA pictures over R, G and B: 0 same, 1 opposite.
[[nodiscard]] double mean_difference(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b);
// Pictures differing by more than this are a scene cut: no blend across it.
inline constexpr double kSceneCut = 0.10;
// Whether the vblank where a new game frame appears shows a blend of it with the frame before:
// only when the frame before was on screen for exactly two vblanks (the game draws at 30 fps)
// and the two are not a scene cut.
[[nodiscard]] bool blend_on_new_frame(bool smoothing, unsigned previous_frame_vblanks, double difference);

} // namespace anypsp::display
