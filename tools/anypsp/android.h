// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// `anypsp build --target android`: an NDK build of the generated code with the runtime and the
// Android host (platform/android), and an APK assembled straight from the SDK build tools
// (aapt2, javac, d8, zipalign, apksigner) without Gradle, as AnyGBA does.
// Layout and decisions: docs/user/android.md.
namespace anypsp::driver::android {

struct Options {
    std::vector<std::string> abis = {"arm64-v8a"};
    std::optional<std::filesystem::path> sdk;   // default: ANDROID_HOME, ANDROID_SDK_ROOT, ~/Android/Sdk, ~/android-sdk
    std::optional<std::filesystem::path> ndk;   // default: ANDROID_NDK_HOME, ANDROID_NDK_ROOT, newest <sdk>/ndk/*
    std::optional<std::string> package_id;      // default: io.anyport.anypsp.g<first 12 hex digits of the module SHA-256>
    std::optional<std::filesystem::path> icon;  // PNG; default: the template icon
    std::optional<std::filesystem::path> keystore;  // default: the Android debug keystore (created if missing)
    std::string key_alias = "androiddebugkey";
    std::optional<std::string> keystore_pass_env;
    std::optional<int> version_code;            // default: minutes since 2024-01-01, so rebuilds update
    bool embed_game = false;                    // put the disc files into the APK (lone modules always are)
};

inline constexpr std::string_view kAbis[] = {"arm64-v8a", "armeabi-v7a", "x86_64", "x86"};

// Parses one Android option at args[i] (advancing i past its value). Returns nullopt when
// args[i] is not an Android option, else an error message ("" on success).
std::optional<std::string> parse_option(std::span<const std::string_view> args, std::size_t& i, Options& out);
std::string usage();

bool valid_package_id(std::string_view id);
std::string default_package_id(std::string_view module_sha256);
std::string xml_escape(std::string_view text);
// Replaces each @KEY@ in `text`; throws std::runtime_error naming a placeholder left over.
std::string fill_template(std::string text, const std::vector<std::pair<std::string, std::string>>& values);

struct Game {
    std::filesystem::path gen_dir;      // psp_recomp --auto output
    std::string name;                   // app label and file name
    std::string title;
    std::string disc_id;
    std::string module_sha256;          // of the executable the code was translated from
    std::string guest_path;             // the executable's path as the game sees it
    // Files the APK carries in assets/game/ (a lone module, or the disc with --embed-game):
    // pairs of host path and name inside assets/game/.
    std::vector<std::pair<std::filesystem::path, std::string>> files;
    bool needs_disc = false;            // the player picks the disc on first start
};

struct Paths {
    std::filesystem::path source;       // AnyPSP source tree
    std::optional<std::filesystem::path> sdl3_source;  // FETCHCONTENT_SOURCE_DIR_SDL3
    std::filesystem::path work;
    std::filesystem::path output_dir;
    std::string cmake = "cmake";
    std::optional<std::string> generator;  // default: Ninja
    std::string build_type = "Release";
    std::string opt = "1";
    std::optional<int> jobs;
};

// Writes <output>/<name>.apk, signed. Returns the path.
std::filesystem::path build_apk(const Game& game, const Paths& paths, const Options& options);

}  // namespace anypsp::driver::android
