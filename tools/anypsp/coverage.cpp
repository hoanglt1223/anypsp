// SPDX-License-Identifier: GPL-2.0-or-later
//
// anypsp-coverage: measures what the runtime and the code generator cover, as JSON for the
// compatibility site (tools/compat-site/generate.py --coverage):
//  - HLE: every function of data/nids.toml offered to the kernel as an import; the ones it
//    has no implementation for are the ones it reports as unimplemented.
//  - VFPU: every VFPU instruction the reference interpreter knows, by mnemonic; lowered when the
//    code generator translates all sampled encodings of it natively instead of calling the
//    interpreter (anypsp::codegen::translated_inline; branches count when the generator emits them).
// GE commands are counted by the site generator from the GE front end's source.

#include "kernel.hpp"
#include "nid_db.hpp"

#include "codegen/emit_cpp.hpp"
#include "cpu/interp.h"
#include "psprecomp/decoder.hpp"
#include "psprecomp/nid_registry.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

std::string json_string(const std::string &s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) < 0x20) continue;
        out += c;
    }
    return out + "\"";
}

struct Count {
    std::size_t done = 0, total = 0;
};

void hle(std::ostream &out, const std::filesystem::path &nids) {
    psprecomp::NidRegistry registry;
    anypsp::loader::load_nid_toml(nids, registry);
    std::vector<psprecomp::PspImport> imports;
    std::map<std::string, Count> libraries;
    std::set<std::string> seen;
    for (const auto &symbol : registry.all()) {
        if (!seen.insert(symbol.library + ":" + std::to_string(symbol.nid)).second) continue;
        imports.push_back({symbol.library, symbol.nid, 0u});
        ++libraries[symbol.library].total;
    }
    auto runtime = std::make_unique<psprecomp::Runtime>();
    auto kernel = std::make_unique<pspweb::Kernel>(*runtime);
    kernel->install(imports);
    std::set<std::string> missing(kernel->unimplemented_imports().begin(), kernel->unimplemented_imports().end());
    // Names are "Library::name"; count per library.
    std::map<std::string, std::size_t> missing_per_library;
    for (const auto &name : missing) ++missing_per_library[name.substr(0, name.find("::"))];
    // Kernel-mode libraries (*ForKernel) are only imported by firmware modules, never by games.
    Count all, user;
    for (auto &[library, count] : libraries) {
        count.done = count.total - std::min(count.total, missing_per_library[library]);
        all.done += count.done;
        all.total += count.total;
        if (library.find("ForKernel") == std::string::npos) {
            user.done += count.done;
            user.total += count.total;
        }
    }
    out << "  \"hle\": {\"implemented\": " << all.done << ", \"total\": " << all.total << ", \"user_implemented\": "
        << user.done << ", \"user_total\": " << user.total << ", \"libraries\": {";
    bool first = true;
    for (const auto &[library, count] : libraries) {
        out << (first ? "\n" : ",\n") << "    " << json_string(library) << ": [" << count.done << ", " << count.total << "]";
        first = false;
    }
    out << "\n  }},\n";
}

void vfpu(std::ostream &out) {
    // Primary opcodes of the VFPU and its loads/stores; register fields fixed, the rest varied.
    static constexpr std::uint32_t kPrimaries[] = {0x12, 0x18, 0x19, 0x1B, 0x32, 0x34, 0x35, 0x36, 0x37, 0x3A, 0x3C, 0x3D, 0x3E, 0x3F};
    struct Op {
        std::size_t encodings = 0, lowered = 0;
    };
    std::map<std::string, Op> ops;
    for (const std::uint32_t primary : kPrimaries) {
        for (std::uint32_t high = 0; high < 1024u; ++high) {
            for (std::uint32_t low = 0; low < 4u; ++low) {
                for (const std::uint32_t regs : {0x0000u, 0x2305u}) {
                    const std::uint32_t word = primary << 26u | high << 16u | (low & 2u) << 14u | (low & 1u) << 7u | regs;
                    const auto info = anypsp::cpu::instruction_info(word);
                    if (!info.valid) continue;
                    const std::string name = info.name;
                    const auto decoded = psprecomp::decode_allegrex(word);
                    const bool lowered = info.control_flow ? anypsp::codegen::is_branch(decoded.kind)
                                                           : anypsp::codegen::translated_inline(decoded);
                    Op &op = ops[name];
                    ++op.encodings;
                    if (lowered) ++op.lowered;
                }
            }
        }
    }
    Count all;
    std::vector<std::string> lowered, missing, partial;
    for (const auto &[name, op] : ops) {
        ++all.total;
        if (op.lowered == op.encodings) ++all.done, lowered.push_back(name);
        else if (op.lowered == 0) missing.push_back(name);
        else partial.push_back(name);
    }
    const auto list = [&out](const std::vector<std::string> &names) {
        out << "[";
        for (std::size_t i = 0; i < names.size(); ++i) out << (i ? ", " : "") << json_string(names[i]);
        out << "]";
    };
    out << "  \"vfpu\": {\"lowered\": " << all.done << ", \"total\": " << all.total << ", \"native\": ";
    list(lowered);
    out << ", \"partial\": ";
    list(partial);
    out << ", \"interpreted\": ";
    list(missing);
    out << "}\n";
}

} // namespace

int main(int argc, char **argv) {
    try {
        std::filesystem::path nids = anypsp::loader::default_nid_database();
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--nids" && i + 1 < argc) nids = argv[++i];
            else {
                std::cerr << "usage: anypsp-coverage [--nids nids.toml]   (JSON on stdout)\n";
                return 2;
            }
        }
        // The kernel names unimplemented imports on stderr as it installs them; keep stdout JSON only.
        std::cout << "{\n";
        hle(std::cout, nids);
        vfpu(std::cout);
        std::cout << "}\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "anypsp-coverage: " << e.what() << "\n";
        return 1;
    }
}
