#pragma once

// Finds the executable of a UMD game given as an ISO image or an extracted
// disc folder. ISO images are extracted once into a cache folder, since the
// kernel serves disc0: from host files. Only unencrypted executables are
// accepted: nothing here decrypts.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace anypsp::disc {

struct BootTarget {
    std::filesystem::path module;     // host path of the executable to load
    std::filesystem::path disc_root;  // host folder disc0:/umd0: resolve to
    std::string guest_path;           // the executable's path as the game sees it
};

// What an executable's first bytes say it is.
enum class ExecutableKind { Elf, Encrypted, Pbp, Other };
ExecutableKind classify_executable(const std::filesystem::path &path);

// Extracts every file of the ISO 9660 image into `destination` (created).
// Throws psprecomp::Error on a malformed image or an I/O failure.
// `progress`, when set, receives bytes written and total file bytes.
void extract_iso(const std::filesystem::path &iso, const std::filesystem::path &destination,
                 const std::function<void(std::uint64_t, std::uint64_t)> &progress = {});

// Returns the extracted folder for `iso` under `cache_root`, extracting it on
// first use (or when the image changed since).
std::filesystem::path cached_extraction(const std::filesystem::path &iso, const std::filesystem::path &cache_root);

// Locates PSP_GAME/SYSDIR/BOOT.BIN or EBOOT.BIN (the first unencrypted ELF) in an
// extracted disc folder. Throws psprecomp::Error explaining what is missing or encrypted.
BootTarget find_boot(const std::filesystem::path &disc_root);

// True when `path` names something resolve() can boot: an .iso file or a disc folder.
bool is_disc(const std::filesystem::path &path);

// ISO or folder -> boot target; ISO images go through cached_extraction.
BootTarget resolve(const std::filesystem::path &path, const std::filesystem::path &cache_root);

} // namespace anypsp::disc
