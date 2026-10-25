// SPDX-License-Identifier: GPL-2.0-or-later
//
// Entry point of a program written by `anypsp build`: finds the game files packaged next to
// it (or takes the game as the first argument), checks that its executable is the one the
// code was translated from, and starts the desktop runner (runtime/gpu/main_gpu.cpp, compiled
// with its main() renamed to anypsp_runner_main and a window by default).

#include "disc.hpp"
#include "psprecomp/sha256.hpp"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_stdinc.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#ifndef ANYPSP_GAME_SHA256
#error "ANYPSP_GAME_SHA256 must give the SHA-256 of the translated executable"
#endif

int anypsp_runner_main(int argc, char **argv);

namespace {

namespace fs = std::filesystem;

void report(const std::string &message) {
    std::fprintf(stderr, "[anypsp] %s\n", message.c_str());
    // A packaged game usually has no console; headless runs never reach a display.
    const char *driver = SDL_getenv("SDL_VIDEO_DRIVER");
    const bool headless = driver != nullptr && (std::string(driver) == "offscreen" || std::string(driver) == "dummy");
    if (!headless) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "AnyPSP", message.c_str(), nullptr);
}

fs::path program_dir() {
    const char *base = SDL_GetBasePath();  // owned by SDL
    return base != nullptr ? fs::path(reinterpret_cast<const char8_t *>(base)) : fs::path();
}

fs::path user_cache() {
    char *path = SDL_GetPrefPath("anyport", "anypsp");
    if (path == nullptr) return {};
    fs::path result = fs::path(reinterpret_cast<const char8_t *>(path)) / "discs";
    SDL_free(path);
    return result;
}

// The executable the runner will boot for `game`, as main_gpu.cpp resolves it.
fs::path boot_module(const fs::path &game, const fs::path &cache) {
    if (anypsp::disc::is_disc(game)) return anypsp::disc::resolve(game, cache).module;
    return game;
}

} // namespace

int main(int argc, char **argv) {
    try {
        std::vector<std::string> args(argv, argv + argc);
        const bool game_given = argc >= 2 && !std::string(argv[1]).starts_with("--");
        bool wants_help = false;
        fs::path cache = user_cache();
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--help" || a == "-h") wants_help = true;
            if (a == "--cache" && i + 1 < argc) cache = argv[i + 1];
        }
        if (wants_help) {
            std::printf("usage: %s [game] [runner options]\n"
                        "Plays the game files next to the program (game.iso, game/ or EBOOT.BIN/PBP) or the game given.\n"
                        "Runner options (window by default):\n",
                        argc > 0 ? argv[0] : "game");
            char *usage_args[] = {argv[0], const_cast<char *>("--usage")};
            return anypsp_runner_main(2, usage_args) == 2 ? 0 : 1;
        }
        fs::path game;
        if (game_given) {
            game = fs::path(reinterpret_cast<const char8_t *>(argv[1]));
        } else {
            const fs::path dir = program_dir();
            for (const char *name : {"game.iso", "game", "EBOOT.BIN", "EBOOT.PBP"}) {
                std::error_code ec;
                if (fs::exists(dir / name, ec)) {
                    game = dir / name;
                    break;
                }
            }
            if (game.empty()) {
                report("No game files next to the program (" + dir.string() +
                       "): put game.iso, the extracted disc folder as game/, or EBOOT.BIN there, or pass the game as the first argument.");
                return 1;
            }
            const std::u8string u = game.u8string();
            args.insert(args.begin() + 1, std::string(u.begin(), u.end()));
        }
        const fs::path module = boot_module(game, cache);
        const std::string hash = psprecomp::sha256_file(module);
        if (hash != ANYPSP_GAME_SHA256) {
            report("This program was translated from another executable than " + module.string() + " (SHA-256 " + hash +
                   ", expected " ANYPSP_GAME_SHA256 "). Use the same game files it was built from.");
            return 1;
        }
        std::vector<char *> pointers;
        for (auto &a : args) pointers.push_back(a.data());
        pointers.push_back(nullptr);
        return anypsp_runner_main(static_cast<int>(args.size()), pointers.data());
    } catch (const std::exception &e) {
        report(e.what());
        return 1;
    }
}
