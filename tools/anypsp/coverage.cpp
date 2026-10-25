// SPDX-License-Identifier: GPL-2.0-or-later
//
// anypsp-coverage: the VFPU instructions the code generator translates natively, as JSON for the
// compatibility site (tools/compat-site/generate.py --coverage): every VFPU instruction the
// reference interpreter knows, by mnemonic; native when the code generator translates all sampled
// encodings of it instead of calling the interpreter (anypsp::codegen::translated_inline; branches
// count when the generator emits them). HLE functions come from anypsp-hle-coverage
// (hle-coverage.json) and GE commands from the site generator's scan of the GE front end.


#include "codegen/emit_cpp.hpp"
#include "cpu/interp.h"
#include "psprecomp/decoder.hpp"

#include <cstdio>
#include <exception>
#include <iostream>
#include <map>
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
    if (argc > 1) {
        std::cerr << "usage: anypsp-coverage   (JSON on stdout)\n";
        return 2;
    }
    try {
        std::cout << "{\n";
        vfpu(std::cout);
        std::cout << "}\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "anypsp-coverage: " << e.what() << "\n";
        return 1;
    }
}
