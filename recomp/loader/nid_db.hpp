// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "psprecomp/nid_registry.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace anypsp::loader {

[[nodiscard]] std::array<std::uint8_t, 20> sha1(std::string_view data);

// A NID is the first four bytes of SHA-1(name), read little-endian. Firmwares
// from 6.x on randomise many NIDs, so a mismatch does not make a name wrong.
[[nodiscard]] std::uint32_t nid_from_name(std::string_view name);

// Loads data/nids.toml: `["Library"]` tables of `0xNNNNNNNN = "name"` entries.
// Throws with the line number on anything else. Returns the entries added.
std::size_t load_nid_toml(const std::filesystem::path &path, psprecomp::NidRegistry &registry);

// The ANYPSP_NID_DATABASE environment variable when set; else data/nids.toml of the source tree
// this binary was built from when it exists; else nids.toml next to the program or in
// <prefix>/share/anypsp/ above its bin/ folder (installed tools and packaged games).
[[nodiscard]] std::filesystem::path default_nid_database();

} // namespace anypsp::loader
