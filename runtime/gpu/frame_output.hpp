#pragma once

// Frame files for headless runs: PNG images and per-frame content hashes.

#include <cstdint>
#include <string>
#include <vector>

namespace anypsp::gpu {

// FNV-1a over the RGB bytes (alpha is ignored), so equal pictures hash equal
// whatever the renderer leaves in alpha.
[[nodiscard]] std::uint64_t frame_hash(const std::vector<std::uint8_t> &rgba);

// Writes `rgba` (width * height * 4, top row first) as an opaque RGB PNG.
// Throws psprecomp::Error on failure.
void write_png(const std::string &path, const std::vector<std::uint8_t> &rgba, int width, int height);

} // namespace anypsp::gpu
