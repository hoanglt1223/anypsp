// SPDX-License-Identifier: GPL-2.0-or-later

#include "elf_prx.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace anypsp::loader {
namespace {

using psprecomp::Elf32Image;
using psprecomp::Error;
using psprecomp::GuestMemory;
using psprecomp::hex32;

constexpr std::uint32_t kPtLoad = 1u;
constexpr std::uint32_t kShtSymtab = 2u;
constexpr std::uint32_t kModuleAlignment = 0x100u;
constexpr std::size_t kModuleInfoSize = 52u;
constexpr std::size_t kMaxLibraryName = 128u;

const char *const kDecryptHint =
    "AnyPSP needs decrypted executables: decrypt EBOOT.BIN and the PRX modules of your own dump "
    "with an external tool, then point AnyPSP at the decrypted files.";

std::uint32_t le32(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16u) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24u);
}

bool starts_with(const std::vector<std::uint8_t> &bytes, const char *magic, std::size_t length) {
    return bytes.size() >= length && std::memcmp(bytes.data(), magic, length) == 0;
}

std::uint32_t align_down(std::uint32_t value, std::uint32_t alignment) { return value & ~(alignment - 1u); }

std::uint32_t parse_u32(const std::string &text, const std::string &context) {
    try {
        std::size_t used = 0;
        const unsigned long value = std::stoul(text, &used, 0);
        if (used != text.size() || value > 0xFFFFFFFFul) throw std::out_of_range(text);
        return static_cast<std::uint32_t>(value);
    } catch (const std::exception &) {
        throw Error(context + ": not a 32-bit number: '" + text + "'");
    }
}

} // namespace

std::vector<std::uint8_t> unwrap_executable(std::vector<std::uint8_t> bytes, const std::string &name) {
    if (starts_with(bytes, "\0PBP", 4)) {
        if (bytes.size() < 0x28u) throw Error(name + ": truncated PBP header");
        const std::uint32_t data_psp = le32(bytes, 0x20u);
        const std::uint32_t data_psar = le32(bytes, 0x24u);
        if (data_psp > data_psar || data_psar > bytes.size())
            throw Error(name + ": corrupt PBP offsets (DATA.PSP " + hex32(data_psp) + ", DATA.PSAR " + hex32(data_psar) + ")");
        bytes = std::vector<std::uint8_t>(bytes.begin() + data_psp, bytes.begin() + data_psar);
        if (bytes.empty()) throw Error(name + ": PBP has no DATA.PSP");
    }
    if (starts_with(bytes, "~SCE", 4) && bytes.size() > 0x40u &&
        std::memcmp(bytes.data() + 0x40, "~PSP", 4) == 0)
        throw Error(name + " is encrypted (~SCE/~PSP header). " + kDecryptHint);
    if (starts_with(bytes, "~PSP", 4)) throw Error(name + " is encrypted or compressed (~PSP header). " + kDecryptHint);
    if (bytes.size() >= 2u && bytes[0] == 0x1Fu && bytes[1] == 0x8Bu)
        throw Error(name + " is gzip-compressed. " + kDecryptHint);
    if (!starts_with(bytes, "\x7F" "ELF", 4)) throw Error(name + " is not an ELF/PRX file. " + kDecryptHint);
    return bytes;
}

std::vector<std::uint8_t> read_executable(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("Cannot open " + path.string());
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) throw Error("Failed reading " + path.string());
    return unwrap_executable(std::move(bytes), path.string());
}

std::uint32_t image_extent(const Elf32Image &elf) {
    std::uint32_t low = 0xFFFFFFFFu, high = 0u;
    for (const auto &segment : elf.segments()) {
        if (segment.type != kPtLoad || segment.memory_size == 0u) continue;
        low = std::min(low, segment.vaddr);
        const std::uint64_t end = static_cast<std::uint64_t>(segment.vaddr) + segment.memory_size;
        if (end > 0xFFFFFFFFull) throw Error("PT_LOAD segment wraps the address space");
        high = std::max(high, static_cast<std::uint32_t>(end));
    }
    if (high == 0u) throw Error("ELF has no PT_LOAD segment");
    return elf.is_psp_prx() ? high : high - low;
}

std::optional<psprecomp::PspModuleInfo> find_module_info(const Elf32Image &elf, const GuestMemory &memory,
                                                         std::uint32_t base) {
    if (auto info = elf.find_module_info(memory, base)) return info;
    if (elf.segments().empty()) return std::nullopt;
    // Without section headers, the first program header's p_paddr holds the file
    // offset of the module info (bit 31 marks kernel modules).
    const auto &first = elf.segments().front();
    const std::uint32_t offset = first.paddr & 0x7FFFFFFFu;
    if (offset < first.offset || offset - first.offset + kModuleInfoSize > first.file_size) return std::nullopt;
    const std::uint32_t address = (elf.is_psp_prx() ? base + first.vaddr : first.vaddr) + (offset - first.offset);
    if (!memory.contains(address, kModuleInfoSize)) return std::nullopt;
    std::string name;
    for (std::uint32_t i = 0; i < 28u; ++i) {
        const char c = static_cast<char>(memory.load8(address + 4u + i));
        if (c == '\0') break;
        name.push_back(c);
    }
    return psprecomp::PspModuleInfo{
        memory.load16(address), memory.load8(address + 2u), memory.load8(address + 3u), name,
        memory.load32(address + 32u), memory.load32(address + 36u), memory.load32(address + 40u),
        memory.load32(address + 44u), memory.load32(address + 48u), address,
    };
}

std::vector<Import> read_imports(const GuestMemory &memory, const psprecomp::PspModuleInfo &info) {
    std::vector<Import> imports;
    for (std::uint32_t cursor = info.stub_top; cursor < info.stub_end;) {
        if (!memory.contains(cursor, 20u)) throw Error("Import table entry outside memory at " + hex32(cursor));
        const std::uint32_t name = memory.load32(cursor);
        const std::uint32_t words = memory.load8(cursor + 8u);
        const std::uint32_t variables = memory.load8(cursor + 9u);
        const std::uint32_t functions = memory.load16(cursor + 10u);
        const std::uint32_t nids = memory.load32(cursor + 12u);
        const std::uint32_t stubs = memory.load32(cursor + 16u);
        if (words < 5u || cursor + words * 4u > info.stub_end)
            throw Error("Corrupt import table entry size " + std::to_string(words) + " at " + hex32(cursor));
        const std::string library = memory.read_c_string(name, kMaxLibraryName);
        if (functions != 0u) {
            if (!memory.contains(nids, functions * 4u) || !memory.contains(stubs, functions * 8u))
                throw Error("Import tables of " + library + " lie outside memory");
            for (std::uint32_t i = 0; i < functions; ++i)
                imports.push_back({library, memory.load32(nids + i * 4u), stubs + i * 8u, false});
        }
        if (variables != 0u && words >= 6u) {
            const std::uint32_t table = memory.load32(cursor + 20u);
            if (!memory.contains(table, variables * 8u))
                throw Error("Variable import table of " + library + " lies outside memory");
            for (std::uint32_t i = 0; i < variables; ++i)
                imports.push_back({library, memory.load32(table + i * 8u + 4u), memory.load32(table + i * 8u), true});
        }
        cursor += words * 4u;
    }
    return imports;
}

std::vector<Export> read_exports(const GuestMemory &memory, const psprecomp::PspModuleInfo &info) {
    std::vector<Export> exports;
    for (std::uint32_t cursor = info.ent_top; cursor < info.ent_end;) {
        if (!memory.contains(cursor, 16u)) throw Error("Export table entry outside memory at " + hex32(cursor));
        const std::uint32_t name = memory.load32(cursor);
        const std::uint32_t words = memory.load8(cursor + 8u);
        const std::uint32_t variables = memory.load8(cursor + 9u);
        const std::uint32_t functions = memory.load16(cursor + 10u);
        const std::uint32_t table = memory.load32(cursor + 12u);
        if (words < 4u || cursor + words * 4u > info.ent_end)
            throw Error("Corrupt export table entry size " + std::to_string(words) + " at " + hex32(cursor));
        const std::uint32_t total = functions + variables;
        if (total != 0u && !memory.contains(table, total * 8u))
            throw Error("Export table at " + hex32(table) + " lies outside memory");
        const std::string library = name != 0u ? memory.read_c_string(name, kMaxLibraryName) : std::string();
        for (std::uint32_t i = 0; i < total; ++i)
            exports.push_back({library, memory.load32(table + i * 4u), memory.load32(table + (total + i) * 4u), i >= functions});
        cursor += words * 4u;
    }
    return exports;
}

LoadedModule load_module(GuestMemory &memory, Elf32Image elf, std::string path, std::uint32_t base) {
    LoadedModule module{std::move(path), std::move(elf)};
    const auto &image = module.elf;
    std::uint32_t low = 0xFFFFFFFFu;
    for (const auto &segment : image.segments())
        if (segment.type == kPtLoad) low = std::min(low, segment.vaddr);
    module.base = image.is_psp_prx() ? base : low;
    module.end = module.base + image_extent(image);
    image.load_into(memory, module.base);
    module.relocations = apply_relocations(image, memory, module.base);
    module.entry = image.runtime_entry(module.base);
    module.info = find_module_info(image, memory, module.base);
    if (!module.info) {
        module.problems.push_back("no module info");
        return module;
    }
    try {
        module.imports = read_imports(memory, *module.info);
    } catch (const Error &e) {
        module.problems.push_back(std::string("imports: ") + e.what());
    }
    try {
        module.exports = read_exports(memory, *module.info);
    } catch (const Error &e) {
        module.problems.push_back(std::string("exports: ") + e.what());
    }
    return module;
}

std::vector<std::uint32_t> plan_layout(const std::vector<LayoutRequest> &modules,
                                       const std::map<std::string, std::uint32_t> &fixed) {
    std::vector<std::uint32_t> bases(modules.size(), 0u);
    std::uint32_t top = kUserMemoryTop;
    for (std::size_t i = 0; i < modules.size(); ++i) {
        const auto &module = modules[i];
        if (!module.relocatable) {
            bases[i] = module.fixed_address;
        } else if (const auto it = fixed.find(module.path); it != fixed.end()) {
            bases[i] = it->second;
        } else if (i == 0u) {
            bases[i] = psprecomp::kDefaultPspUserLoadBase;
        } else {
            if (module.size > top - psprecomp::kDefaultPspUserLoadBase) throw Error("No room for module " + module.path);
            bases[i] = align_down(top - module.size, kModuleAlignment);
            top = bases[i];
        }
    }
    for (std::size_t i = 0; i < modules.size(); ++i)
        for (std::size_t j = i + 1u; j < modules.size(); ++j) {
            const std::uint64_t a0 = bases[i], a1 = a0 + modules[i].size;
            const std::uint64_t b0 = bases[j], b1 = b0 + modules[j].size;
            if (a0 < b1 && b0 < a1)
                throw Error("Modules " + modules[i].path + " and " + modules[j].path + " overlap at " + hex32(bases[j]));
        }
    return bases;
}

std::map<std::string, std::uint32_t> read_layout_file(const std::filesystem::path &path) {
    std::ifstream in(path);
    if (!in) throw Error("Cannot open layout file " + path.string());
    std::map<std::string, std::uint32_t> layout;
    std::string line;
    for (std::size_t number = 1; std::getline(in, line); ++number) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        const auto space = line.find_first_of(" \t", first);
        const auto rest = space == std::string::npos ? std::string::npos : line.find_first_not_of(" \t", space);
        const std::string context = path.string() + ":" + std::to_string(number);
        if (rest == std::string::npos) throw Error(context + ": expected '<base address> <module path>'");
        const std::uint32_t base = parse_u32(line.substr(first, space - first), context);
        if ((base & (kModuleAlignment - 1u)) != 0u) throw Error(context + ": base is not 256-byte aligned");
        auto module = line.substr(rest);
        while (!module.empty() && (module.back() == ' ' || module.back() == '\t')) module.pop_back();
        if (!layout.emplace(module, base).second) throw Error(context + ": " + module + " is listed twice");
    }
    return layout;
}

std::vector<ImportLink> link_imports(const std::vector<LoadedModule> &modules, const psprecomp::NidRegistry &nids) {
    struct Target {
        std::size_t module;
        std::uint32_t address;
    };
    std::map<std::tuple<std::string, std::uint32_t, bool>, Target> exported;
    for (std::size_t m = 0; m < modules.size(); ++m)
        for (const auto &e : modules[m].exports)
            if (!e.library.empty()) exported.try_emplace({e.library, e.nid, e.variable}, Target{m, e.address});

    std::vector<ImportLink> links;
    for (std::size_t m = 0; m < modules.size(); ++m) {
        for (const auto &import : modules[m].imports) {
            ImportLink link{m, import};
            link.name = nids.resolve(import.library, import.nid).value_or("");
            if (const auto it = exported.find({import.library, import.nid, import.variable}); it != exported.end()) {
                link.target = ImportTarget::GameModule;
                link.export_module = it->second.module;
                link.export_address = it->second.address;
            } else if (!link.name.empty()) {
                link.target = ImportTarget::Hle;
            }
            links.push_back(std::move(link));
        }
    }
    return links;
}

std::map<std::uint32_t, std::string> read_function_symbols(const Elf32Image &elf, std::uint32_t base) {
    std::map<std::uint32_t, std::string> symbols;
    const auto &bytes = elf.bytes();
    const auto &sections = elf.sections();
    const auto in_file = [&](std::uint64_t offset, std::uint64_t size) { return offset + size <= bytes.size(); };
    for (const auto &section : sections) {
        if (section.type != kShtSymtab || section.link >= sections.size() || !in_file(section.offset, section.size)) continue;
        const auto &strings = sections[section.link];
        if (!in_file(strings.offset, strings.size)) continue;
        for (std::uint64_t off = section.offset; off + 16u <= static_cast<std::uint64_t>(section.offset) + section.size; off += 16u) {
            const std::uint32_t name = le32(bytes, off);
            const std::uint32_t value = le32(bytes, off + 4u);
            const std::uint8_t info = bytes[off + 12u];
            const std::uint16_t shndx = static_cast<std::uint16_t>(bytes[off + 14u] | (bytes[off + 15u] << 8u));
            if ((info & 0x0Fu) != 2u || shndx == 0u || name >= strings.size) continue;
            const char *start = reinterpret_cast<const char *>(bytes.data() + strings.offset + name);
            const auto length = strnlen(start, strings.size - name);
            symbols.try_emplace(elf.is_psp_prx() ? base + value : value, std::string(start, length));
        }
    }
    return symbols;
}

} // namespace anypsp::loader
