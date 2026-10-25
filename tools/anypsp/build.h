// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "android.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// `anypsp build`: anypsp-analyze -> psp_recomp --auto -> a CMake build of the generated code
// against the AnyPSP sources -> a packaged program (docs/user/build.md).
namespace anypsp::driver {

enum class Target { Linux, Windows, MacOS, Android };

struct BuildOptions {
    std::filesystem::path input;                    // ISO image, extracted disc folder, EBOOT.BIN/PBP or module
    std::filesystem::path output_dir = ".";
    std::optional<std::filesystem::path> work_dir;  // default: <output>/<input stem>-build
    std::optional<std::string> name;                // default: the disc title, else the module name
    std::optional<std::filesystem::path> feedback;  // trace feedback from earlier runs
    std::optional<std::filesystem::path> source;    // AnyPSP source tree (default: the installed or built-from one)
    std::optional<std::filesystem::path> tools;     // folder with anypsp-analyze and psp_recomp
    std::optional<std::filesystem::path> toolchain; // CMake toolchain file for the game build (cross builds)
    std::string cmake = "cmake";
    std::optional<std::string> generator;
    std::string build_type = "Release";
    std::string opt = "1";                          // generated code: d, 1 or 2
    std::optional<int> jobs;
    bool copy_game = true;
    std::optional<std::string> codesign_identity;   // macOS; default ad-hoc
    Target target;
    android::Options android;

    BuildOptions();
};

// Parses the arguments after `build`. Returns an error message on bad input.
std::optional<std::string> parse_build_args(std::span<const std::string_view> args, BuildOptions& out);
std::string build_usage();
Target host_target();
std::string target_name(Target target);

// Title to file name: ASCII letters, digits, '-', '_' and spaces kept, the rest dropped,
// trimmed; "game" if nothing is left.
std::string program_name(std::string_view title);

// What was given to `anypsp build`, and where its executable is.
struct GameInput {
    enum class Kind { Iso, DiscFolder, Module } kind{Kind::Module};
    std::filesystem::path path;        // as given
    std::filesystem::path module;      // host path of the executable to translate
    std::filesystem::path disc_root;   // extracted disc (empty for a lone module)
    std::string guest_path;            // the executable's path as the game sees it
    std::string title;                 // PARAM.SFO TITLE, else empty
    std::string disc_id;               // PARAM.SFO DISC_ID, else empty
};
// ISO images are extracted into `cache` (once). Throws std::runtime_error on bad input.
GameInput resolve_input(const std::filesystem::path& input, const std::filesystem::path& cache);

// PARAM.SFO string value (TITLE, DISC_ID, ...); empty when absent or unreadable.
std::string sfo_string(const std::vector<std::uint8_t>& sfo, std::string_view key);

// Runs the whole pipeline. Returns the path written: <output>/<Name>/ (a folder with the program,
// its data and the game files), <output>/<Name>.app on macOS, <output>/<Name>.apk for Android.
// Throws std::runtime_error with a user-facing message.
std::filesystem::path build(const BuildOptions& options);

std::optional<std::filesystem::path> executable_path();
// Runs a program with arguments (no shell) and returns its exit code. Throws if it cannot start.
int run_process(const std::vector<std::string>& argv);
// run_process() that prints the command first and throws std::runtime_error("<what> failed ...")
// on a non-zero exit code.
void run_checked(const std::vector<std::string>& argv, const std::string& what);
std::string path_string(const std::filesystem::path& p);

}  // namespace anypsp::driver
