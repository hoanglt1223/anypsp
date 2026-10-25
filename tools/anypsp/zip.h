// SPDX-License-Identifier: MIT
// From AnyGBA (tools/anygba/zip.h), Copyright (c) 2026 Henry Nguyen; see THIRD_PARTY.md.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Just enough ZIP for APKs and game packages: copy the entries of an existing archive (what
// aapt2 wrote) unchanged and add new ones stored uncompressed, which native libraries need
// so Android can map them straight from the APK. No ZIP64.
namespace anypsp::driver::zip {

struct Entry {
    std::string name;  // '/'-separated path inside the archive
    std::vector<std::uint8_t> data;
};

// Writes `out`: every entry of `base` whose name is not in `entries`, then `entries`.
// Throws std::runtime_error when `base` is not a ZIP this code understands or on I/O errors.
void write(const std::filesystem::path& out, const std::optional<std::filesystem::path>& base,
           const std::vector<Entry>& entries);

struct Listed {
    std::string name;
    std::uint16_t method = 0;  // 0 = stored, 8 = deflated
    std::uint32_t crc32 = 0;
    std::uint32_t size = 0;
};
// The central directory of `archive`. Throws std::runtime_error like write().
std::vector<Listed> list(const std::filesystem::path& archive);

// Every regular file under `dir`, named by its path relative to `dir` with '/' separators,
// sorted by name.
std::vector<Entry> directory_entries(const std::filesystem::path& dir, const std::string& prefix = "");

}  // namespace anypsp::driver::zip
