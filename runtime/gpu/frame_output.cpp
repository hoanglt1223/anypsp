// SPDX-License-Identifier: GPL-2.0-or-later

#include "frame_output.hpp"

#include "psprecomp/common.hpp"

#include <SDL3/SDL.h>

namespace anypsp::gpu {

std::uint64_t frame_hash(const std::vector<std::uint8_t> &rgba) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i + 3u < rgba.size(); i += 4u)
        for (std::size_t c = 0; c < 3u; ++c) h = (h ^ rgba[i + c]) * 0x100000001B3ull;
    return h;
}

void write_png(const std::string &path, const std::vector<std::uint8_t> &rgba, int width, int height) {
    if (width <= 0 || height <= 0 || rgba.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u)
        throw psprecomp::Error("write_png: " + path + ": pixel buffer does not match " + std::to_string(width) + "x" +
                               std::to_string(height));
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u);
    for (std::size_t i = 0, j = 0; j < rgb.size(); i += 4u, j += 3u) {
        rgb[j] = rgba[i];
        rgb[j + 1u] = rgba[i + 1u];
        rgb[j + 2u] = rgba[i + 2u];
    }
    SDL_Surface *surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGB24, rgb.data(), width * 3);
    if (surface == nullptr) throw psprecomp::Error("write_png: " + path + ": " + SDL_GetError());
    const bool saved = SDL_SavePNG(surface, path.c_str());
    SDL_DestroySurface(surface);
    if (!saved) throw psprecomp::Error("cannot write " + path + ": " + SDL_GetError());
}

} // namespace anypsp::gpu
