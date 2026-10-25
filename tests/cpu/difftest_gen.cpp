// SPDX-License-Identifier: GPL-2.0-or-later
//
// Writes the cases of the code generator's differential test: for a seeded
// sample of encodings of every instruction the generator translates inline
// (upstream kinds and cpu/generated_ops.h), one function holding exactly the C++ psp_recomp emits for it
// (after the same source lowering), plus a table of the words.
//
//   anypsp-difftest-gen <output.cpp> [samples per kind]

#include "codegen/emit_cpp.hpp"
#include "cpu/interp.h"

#include "psprecomp/common.hpp"
#include "psprecomp/decoder.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Major opcode / function patterns that cover every inline kind: each entry is
// a fixed bit pattern plus the mask of bits to randomise.
struct Pattern {
    std::uint32_t fixed;
    std::uint32_t random_mask;
    int sample_scale = 1;
};

std::vector<Pattern> patterns() {
    std::vector<Pattern> list;
    const std::uint32_t fields = 0x03FFFFFFu;
    for (std::uint32_t op : {0x08u, 0x09u, 0x0Au, 0x0Bu, 0x0Cu, 0x0Du, 0x0Eu, 0x0Fu,      // immediates
                             0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u,             // loads
                             0x28u, 0x29u, 0x2Au, 0x2Bu, 0x2Eu, 0x2Fu,                    // stores, cache
                             0x31u, 0x39u, 0x32u, 0x3Au, 0x36u, 0x3Eu,                    // lwc1/swc1, lv/sv
                             0x30u, 0x38u, 0x35u, 0x3Du})                                 // ll/sc, lvl/lvr, svl/svr
        list.push_back({op << 26u, fields});
    for (std::uint32_t fn = 0u; fn < 64u; ++fn) list.push_back({fn, fields & ~0x3Fu});    // SPECIAL
    list.push_back({0x1Fu << 26u, fields & ~0x3Fu});                                      // ext
    list.push_back({(0x1Fu << 26u) | 4u, fields & ~0x3Fu});                               // ins
    list.push_back({(0x1Fu << 26u) | 0x20u, fields & ~0x3Fu});                            // bshfl
    list.push_back({(0x11u << 26u) | (0x00u << 21u), 0x001FFFFFu});                       // mfc1
    list.push_back({(0x11u << 26u) | (0x04u << 21u), 0x001FFFFFu});                       // mtc1
    for (std::uint32_t fn = 0u; fn < 64u; ++fn) list.push_back({(0x11u << 26u) | (0x10u << 21u) | fn, 0x001FFFC0u});
    list.push_back({(0x11u << 26u) | (0x14u << 21u) | 0x20u, 0x001FFFC0u});               // cvt.s.w
    list.push_back({(0x12u << 26u) | (0x03u << 21u), 0x001FFFFFu});                       // mfv
    list.push_back({(0x12u << 26u) | (0x07u << 21u), 0x001FFFFFu});                       // mtv
    list.push_back({0x37u << 26u, fields});                                               // vpfx, viim, vfim
    for (std::uint32_t fn = 0u; fn < 64u; ++fn) list.push_back({(0x1Cu << 26u) | fn, fields & ~0x3Fu}); // SPECIAL2
    list.push_back({(0x11u << 26u) | (0x02u << 21u), 0x001FFFFFu});                       // cfc1
    list.push_back({(0x11u << 26u) | (0x06u << 21u), 0x001FFFFFu});                       // ctc1
    for (std::uint32_t major : {0x18u, 0x19u, 0x1Bu})                                     // VFPU0/1/3: vadd .. vslt
        for (std::uint32_t op = 0u; op < 8u; ++op) list.push_back({(major << 26u) | (op << 23u), 0x007FFFFFu, 4});
    for (std::uint32_t major : {0x34u, 0x3Cu})                                            // VFPU4/6: unary, matrix ops
        for (std::uint32_t group = 0u; group < 32u; ++group)
            for (std::uint32_t op = 0u; op < 32u; ++op)
                list.push_back({(major << 26u) | (group << 21u) | (op << 16u), 0x0000FFFFu});
    list.push_back({0x3Fu << 26u, fields});                                               // vflush
    list.push_back({(0x37u << 26u) | (6u << 23u), 0x007FFFFFu});                          // viim
    list.push_back({(0x37u << 26u) | (7u << 23u), 0x007FFFFFu});                          // vfim
    list.push_back({(0x12u << 26u) | (0x03u << 21u) | 0x80u, 0x001F007Fu});               // mfvc
    return list;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: anypsp-difftest-gen <output.cpp> [samples per pattern]\n";
        return 2;
    }
    const int samples = argc > 2 ? std::stoi(argv[2]) : 24;
    std::mt19937 rng(0x5EEDu);
    std::map<std::uint32_t, std::string> cases;  // word -> emitted body
    for (const auto &pattern : patterns()) {
        for (int i = 0; i < samples * pattern.sample_scale; ++i) {
            const std::uint32_t word = pattern.fixed | (static_cast<std::uint32_t>(rng()) & pattern.random_mask);
            const auto decoded = psprecomp::decode_allegrex(word);
            if (decoded.is_control_flow() || !anypsp::codegen::translated_inline(decoded)) continue;
            cases.emplace(word, anypsp::codegen::emit_instruction(decoded, 0x08804000u));
        }
    }

    std::ostringstream body;
    std::size_t index = 0;
    for (const auto &[word, code] : cases) {
        body << "static void case_" << index++ << "(Runtime &rt, AllegrexContext &ctx) {\n"
             << "    auto aot_mem = rt.memory().aot_fast_view();\n    (void)aot_mem;\n"
             << code << "}\n\n";
    }
    std::ostringstream out;
    out << "// Generated by anypsp-difftest-gen; do not edit.\n"
        << "#include \"difftest_cases.hpp\"\n#include \"cpu/generated_ops.h\"\n"
        << "#include <bit>\n#include <cmath>\n#include <cstdint>\n#include <limits>\n\n"
        << "namespace psprecomp {\n\n"
        << anypsp::codegen::lower_generated_source(body.str())
        << "} // namespace psprecomp\n\n"
        << "const DiffCase kDiffCases[] = {\n";
    index = 0;
    for (const auto &[word, code] : cases) {
        const auto decoded = psprecomp::decode_allegrex(word);
        out << "    {" << psprecomp::hex32(word) << "u, \"" << anypsp::cpu::instruction_info(word).name << "\", &psprecomp::case_" << index++
            << "},\n";
    }
    out << "};\nconst std::size_t kDiffCaseCount = " << cases.size() << ";\n";
    std::ofstream file(argv[1], std::ios::binary);
    file << out.str();
    if (!file) {
        std::cerr << "cannot write " << argv[1] << "\n";
        return 1;
    }
    std::cout << cases.size() << " cases\n";
    return 0;
}
