// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "relocs.hpp"

#include "psprecomp/elf32.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/nid_registry.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace anypsp::loader {

// Reads a decrypted executable. A PBP is unpacked to its DATA.PSP; encrypted
// (~PSP, ~SCE) and compressed inputs are refused with an explanation.
[[nodiscard]] std::vector<std::uint8_t> read_executable(const std::filesystem::path &path);
[[nodiscard]] std::vector<std::uint8_t> unwrap_executable(std::vector<std::uint8_t> bytes, const std::string &name);

struct Import {
    std::string library;
    std::uint32_t nid{};
    std::uint32_t stub_address{}; // functions: the 8-byte stub; variables: the reference list
    bool variable{};
};

struct Export {
    std::string library; // empty for the system entries (module_start and friends)
    std::uint32_t nid{};
    std::uint32_t address{};
    bool variable{};
};

struct LoadedModule {
    std::string path;
    psprecomp::Elf32Image elf;
    std::uint32_t base{};
    std::uint32_t end{}; // one past the highest loaded byte
    std::uint32_t entry{};
    std::optional<psprecomp::PspModuleInfo> info;
    std::vector<Import> imports;
    std::vector<Export> exports;
    RelocationResult relocations;
    // Tables that could not be read, e.g. in a PRX cut off before its relocations,
    // which the PSP loads unrelocated.
    std::vector<std::string> problems;
};

inline constexpr std::uint32_t kNidModuleStart = 0xD632ACDBu;
inline constexpr std::uint32_t kNidModuleStop = 0xCEE8593Cu;
inline constexpr std::uint32_t kNidModuleBootStart = 0xD3744BE0u;
inline constexpr std::uint32_t kNidModuleRebootBefore = 0x2F064FA6u;
inline constexpr std::uint32_t kNidModuleRebootPhase = 0xADF12745u;
inline constexpr std::uint32_t kNidModuleInfo = 0xF01D73A7u;

// Bytes the image occupies from its load address (PRX) or from address 0 (ET_EXEC).
[[nodiscard]] std::uint32_t image_extent(const psprecomp::Elf32Image &elf);

// Copies, relocates and parses one module at `base` (ignored for ET_EXEC images).
[[nodiscard]] LoadedModule load_module(psprecomp::GuestMemory &memory, psprecomp::Elf32Image elf,
                                       std::string path, std::uint32_t base);

[[nodiscard]] std::optional<psprecomp::PspModuleInfo> find_module_info(const psprecomp::Elf32Image &elf,
                                                                       const psprecomp::GuestMemory &memory,
                                                                       std::uint32_t base);
[[nodiscard]] std::vector<Import> read_imports(const psprecomp::GuestMemory &memory, const psprecomp::PspModuleInfo &info);
[[nodiscard]] std::vector<Export> read_exports(const psprecomp::GuestMemory &memory, const psprecomp::PspModuleInfo &info);

// Load addresses. The first module goes to 0x08804000 (or its own address for
// ET_EXEC); the others are stacked down from the top of user memory so they stay
// clear of heaps the game allocates from the bottom. `fixed` (path -> base)
// overrides the plan, e.g. with the addresses the runtime used.
struct LayoutRequest {
    std::string path;
    std::uint32_t size{};
    bool relocatable{};
    std::uint32_t fixed_address{}; // ET_EXEC: lowest PT_LOAD address
};
inline constexpr std::uint32_t kUserMemoryTop = 0x0A000000u;
[[nodiscard]] std::vector<std::uint32_t> plan_layout(const std::vector<LayoutRequest> &modules,
                                                     const std::map<std::string, std::uint32_t> &fixed = {});
[[nodiscard]] std::map<std::string, std::uint32_t> read_layout_file(const std::filesystem::path &path);

enum class ImportTarget : std::uint8_t { Hle, GameModule, Unknown };

struct ImportLink {
    std::size_t module{};
    Import import;
    ImportTarget target{ImportTarget::Unknown};
    std::string name;              // NID name when known
    std::size_t export_module{};   // GameModule only
    std::uint32_t export_address{}; // GameModule only
};

// Links every import: to another loaded module that exports it, else to the HLE
// layer when the NID database knows the name, else it is reported as Unknown.
[[nodiscard]] std::vector<ImportLink> link_imports(const std::vector<LoadedModule> &modules,
                                                   const psprecomp::NidRegistry &nids);

// Function symbols (STT_FUNC) from .symtab, at runtime addresses; empty when stripped.
[[nodiscard]] std::map<std::uint32_t, std::string> read_function_symbols(const psprecomp::Elf32Image &elf,
                                                                        std::uint32_t base);

} // namespace anypsp::loader
