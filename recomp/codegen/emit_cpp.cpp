// Instruction emission and the source lowering passes of the code generator.
// Moved out of tools/codegen_main.cpp (PSPRecomp, MIT) so tests can compile the
// emitted code; AnyPSP changes are GPL-2.0-or-later like the rest of the tool.

#include "codegen/emit_cpp.hpp"

#include "psprecomp/codegen_policy.hpp"
#include "cpu/interp.h"
#include "psprecomp/common.hpp"

#include <cctype>
#include <cstdlib>
#include <regex>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace anypsp::codegen {

std::string reg(std::uint32_t index) {
    // Allegrex/MIPS $zero is immutable. Emitting a literal lets the host
    // compiler fold branches/arithmetic instead of materializing a load from
    // AllegrexContext for every source-register use of r0.
    if (index == 0u) return "0u";
    return "ctx.gpr[" + std::to_string(index) + "]";
}

std::string emit_regular(const psprecomp::DecodedInstruction &d, std::uint32_t pc) {
    const auto imm = static_cast<std::int32_t>(d.immediate);
    const auto uimm = static_cast<std::uint16_t>(d.immediate);
    std::ostringstream out;
    switch (d.kind) {
    case psprecomp::OpcodeKind::Nop: out << "    // nop\n"; break;
    case psprecomp::OpcodeKind::Sync:
    case psprecomp::OpcodeKind::Cache:
        out << psprecomp::codegen::memory_ordering_statement(d.kind);
        break;
    case psprecomp::OpcodeKind::Addiu: out << "    ctx.set_gpr(" << d.rt << ", " << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << "));\n"; break;
    case psprecomp::OpcodeKind::Slti: out << "    ctx.set_gpr(" << d.rt << ", static_cast<std::int32_t>(" << reg(d.rs) << ") < " << imm << " ? 1u : 0u);\n"; break;
    case psprecomp::OpcodeKind::Sltiu: out << "    ctx.set_gpr(" << d.rt << ", " << reg(d.rs) << " < static_cast<std::uint32_t>(" << imm << ") ? 1u : 0u);\n"; break;
    case psprecomp::OpcodeKind::Andi: out << "    ctx.set_gpr(" << d.rt << ", " << reg(d.rs) << " & " << uimm << "u);\n"; break;
    case psprecomp::OpcodeKind::Ori: out << "    ctx.set_gpr(" << d.rt << ", " << reg(d.rs) << " | " << uimm << "u);\n"; break;
    case psprecomp::OpcodeKind::Xori: out << "    ctx.set_gpr(" << d.rt << ", " << reg(d.rs) << " ^ " << uimm << "u);\n"; break;
    case psprecomp::OpcodeKind::Lui: out << "    ctx.set_gpr(" << d.rt << ", " << uimm << "u << 16u);\n"; break;
    case psprecomp::OpcodeKind::Add:
    case psprecomp::OpcodeKind::Addu: out << "    ctx.set_gpr(" << d.rd << ", " << reg(d.rs) << " + " << reg(d.rt) << ");\n"; break;
    case psprecomp::OpcodeKind::Sub:
    case psprecomp::OpcodeKind::Subu: out << "    ctx.set_gpr(" << d.rd << ", " << reg(d.rs) << " - " << reg(d.rt) << ");\n"; break;
    case psprecomp::OpcodeKind::And: out << "    ctx.set_gpr(" << d.rd << ", " << reg(d.rs) << " & " << reg(d.rt) << ");\n"; break;
    case psprecomp::OpcodeKind::Or: out << "    ctx.set_gpr(" << d.rd << ", " << reg(d.rs) << " | " << reg(d.rt) << ");\n"; break;
    case psprecomp::OpcodeKind::Xor: out << "    ctx.set_gpr(" << d.rd << ", " << reg(d.rs) << " ^ " << reg(d.rt) << ");\n"; break;
    case psprecomp::OpcodeKind::Nor: out << "    ctx.set_gpr(" << d.rd << ", ~(" << reg(d.rs) << " | " << reg(d.rt) << "));\n"; break;
    case psprecomp::OpcodeKind::Slt: out << "    ctx.set_gpr(" << d.rd << ", static_cast<std::int32_t>(" << reg(d.rs) << ") < static_cast<std::int32_t>(" << reg(d.rt) << ") ? 1u : 0u);\n"; break;
    case psprecomp::OpcodeKind::Sltu: out << "    ctx.set_gpr(" << d.rd << ", " << reg(d.rs) << " < " << reg(d.rt) << " ? 1u : 0u);\n"; break;
    case psprecomp::OpcodeKind::Max: out << "    ctx.set_gpr(" << d.rd << ", static_cast<std::int32_t>(" << reg(d.rs) << ") > static_cast<std::int32_t>(" << reg(d.rt) << ") ? " << reg(d.rs) << " : " << reg(d.rt) << ");\n"; break;
    case psprecomp::OpcodeKind::Min: out << "    ctx.set_gpr(" << d.rd << ", static_cast<std::int32_t>(" << reg(d.rs) << ") < static_cast<std::int32_t>(" << reg(d.rt) << ") ? " << reg(d.rs) << " : " << reg(d.rt) << ");\n"; break;
    case psprecomp::OpcodeKind::Movz:
        out << "    if (" << reg(d.rt) << " == 0u) ctx.set_gpr(" << d.rd << ", " << reg(d.rs) << ");\n";
        break;
    case psprecomp::OpcodeKind::Movn:
        out << "    if (" << reg(d.rt) << " != 0u) ctx.set_gpr(" << d.rd << ", " << reg(d.rs) << ");\n";
        break;
    case psprecomp::OpcodeKind::Sll: out << "    ctx.set_gpr(" << d.rd << ", " << reg(d.rt) << " << " << d.sa << "u);\n"; break;
    case psprecomp::OpcodeKind::Srl: out << "    ctx.set_gpr(" << d.rd << ", " << reg(d.rt) << " >> " << d.sa << "u);\n"; break;
    case psprecomp::OpcodeKind::Sra: out << "    ctx.set_gpr(" << d.rd << ", static_cast<std::uint32_t>(static_cast<std::int32_t>(" << reg(d.rt) << ") >> " << d.sa << "u));\n"; break;
    case psprecomp::OpcodeKind::Rotr: out << "    ctx.set_gpr(" << d.rd << ", std::rotr(" << reg(d.rt) << ", " << d.sa << "));\n"; break;
    case psprecomp::OpcodeKind::Sllv: out << "    ctx.set_gpr(" << d.rd << ", " << reg(d.rt) << " << (" << reg(d.rs) << " & 31u));\n"; break;
    case psprecomp::OpcodeKind::Srlv: out << "    ctx.set_gpr(" << d.rd << ", " << reg(d.rt) << " >> (" << reg(d.rs) << " & 31u));\n"; break;
    case psprecomp::OpcodeKind::Srav: out << "    ctx.set_gpr(" << d.rd << ", static_cast<std::uint32_t>(static_cast<std::int32_t>(" << reg(d.rt) << ") >> (" << reg(d.rs) << " & 31u)));\n"; break;
    case psprecomp::OpcodeKind::Rotrv: out << "    ctx.set_gpr(" << d.rd << ", std::rotr(" << reg(d.rt) << ", static_cast<int>(" << reg(d.rs) << " & 31u)));\n"; break;
    case psprecomp::OpcodeKind::Clz:
        out << "    ctx.set_gpr(" << d.rd << ", static_cast<std::uint32_t>(std::countl_zero(" << reg(d.rs) << ")));\n";
        break;
    case psprecomp::OpcodeKind::Clo:
        out << "    ctx.set_gpr(" << d.rd << ", static_cast<std::uint32_t>(std::countl_one(" << reg(d.rs) << ")));\n";
        break;
    case psprecomp::OpcodeKind::Ext: {
        const std::uint32_t size = d.rd + 1u;
        const std::uint32_t mask = size == 32u ? 0xFFFFFFFFu : ((1u << size) - 1u);
        out << "    ctx.set_gpr(" << d.rt << ", (" << reg(d.rs) << " >> " << d.sa << "u) & " << psprecomp::hex32(mask) << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Ins: {
        const std::uint32_t size = d.rd >= d.sa ? d.rd - d.sa + 1u : 0u;
        const std::uint32_t source_mask = size == 32u ? 0xFFFFFFFFu : (size == 0u ? 0u : ((1u << size) - 1u));
        const std::uint32_t destination_mask = source_mask << d.sa;
        out << "    ctx.set_gpr(" << d.rt << ", (" << reg(d.rt) << " & ~" << psprecomp::hex32(destination_mask)
            << "u) | ((" << reg(d.rs) << " & " << psprecomp::hex32(source_mask) << "u) << " << d.sa << "u));\n";
        break;
    }
    case psprecomp::OpcodeKind::Seb:
        out << "    ctx.set_gpr(" << d.rd << ", static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(" << reg(d.rt) << "))));\n";
        break;
    case psprecomp::OpcodeKind::Seh:
        out << "    ctx.set_gpr(" << d.rd << ", static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(" << reg(d.rt) << "))));\n";
        break;
    case psprecomp::OpcodeKind::Bitrev:
        out << "    ctx.set_gpr(" << d.rd << ", [](std::uint32_t value) { "
               "value = ((value >> 1u) & 0x55555555u) | ((value & 0x55555555u) << 1u); "
               "value = ((value >> 2u) & 0x33333333u) | ((value & 0x33333333u) << 2u); "
               "value = ((value >> 4u) & 0x0F0F0F0Fu) | ((value & 0x0F0F0F0Fu) << 4u); "
               "value = ((value >> 8u) & 0x00FF00FFu) | ((value & 0x00FF00FFu) << 8u); "
               "return (value >> 16u) | (value << 16u); }(" << reg(d.rt) << "));\n";
        break;
    case psprecomp::OpcodeKind::Wsbh:
        out << "    ctx.set_gpr(" << d.rd << ", ((" << reg(d.rt) << " & 0x00FF00FFu) << 8u) | ((" << reg(d.rt) << " & 0xFF00FF00u) >> 8u));\n";
        break;
    case psprecomp::OpcodeKind::Wsbw:
        out << "    ctx.set_gpr(" << d.rd << ", ((" << reg(d.rt) << " & 0x000000FFu) << 24u) | ((" << reg(d.rt) << " & 0x0000FF00u) << 8u) | ((" << reg(d.rt) << " & 0x00FF0000u) >> 8u) | ((" << reg(d.rt) << " & 0xFF000000u) >> 24u));\n";
        break;
    case psprecomp::OpcodeKind::Lw: out << "    ctx.set_gpr(" << d.rt << ", rt.memory().aot_load32(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << ")));\n"; break;
    case psprecomp::OpcodeKind::Lwl: out << "    ctx.set_gpr(" << d.rt << ", rt.memory().aot_load_word_left(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << "), " << reg(d.rt) << "));\n"; break;
    case psprecomp::OpcodeKind::Lwr: out << "    ctx.set_gpr(" << d.rt << ", rt.memory().aot_load_word_right(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << "), " << reg(d.rt) << "));\n"; break;
    case psprecomp::OpcodeKind::Sw: out << "    rt.memory().aot_store32(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << "), " << reg(d.rt) << ");\n"; break;
    case psprecomp::OpcodeKind::Swl: out << "    rt.memory().aot_store_word_left(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << "), " << reg(d.rt) << ");\n"; break;
    case psprecomp::OpcodeKind::Swr: out << "    rt.memory().aot_store_word_right(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << "), " << reg(d.rt) << ");\n"; break;
    case psprecomp::OpcodeKind::Lh: out << "    ctx.set_gpr(" << d.rt << ", static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(rt.memory().aot_load16(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << "))))));\n"; break;
    case psprecomp::OpcodeKind::Lhu: out << "    ctx.set_gpr(" << d.rt << ", rt.memory().aot_load16(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << ")));\n"; break;
    case psprecomp::OpcodeKind::Sh: out << "    rt.memory().aot_store16(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << "), static_cast<std::uint16_t>(" << reg(d.rt) << "));\n"; break;
    case psprecomp::OpcodeKind::Lb: out << "    ctx.set_gpr(" << d.rt << ", static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(rt.memory().aot_load8(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << "))))));\n"; break;
    case psprecomp::OpcodeKind::Lbu: out << "    ctx.set_gpr(" << d.rt << ", rt.memory().aot_load8(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << ")));\n"; break;
    case psprecomp::OpcodeKind::Sb: out << "    rt.memory().aot_store8(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << "), static_cast<std::uint8_t>(" << reg(d.rt) << "));\n"; break;
    case psprecomp::OpcodeKind::Lwc1: out << "    ctx.fpr[" << d.rt << "] = std::bit_cast<float>(rt.memory().aot_load32(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << ")));\n"; break;
    case psprecomp::OpcodeKind::Swc1: out << "    rt.memory().aot_store32(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << imm << "), std::bit_cast<std::uint32_t>(ctx.fpr[" << d.rt << "]));\n"; break;
    case psprecomp::OpcodeKind::Mfhi: out << "    ctx.set_gpr(" << d.rd << ", ctx.hi);\n"; break;
    case psprecomp::OpcodeKind::Mflo: out << "    ctx.set_gpr(" << d.rd << ", ctx.lo);\n"; break;
    case psprecomp::OpcodeKind::Mthi: out << "    ctx.hi = " << reg(d.rs) << ";\n"; break;
    case psprecomp::OpcodeKind::Mtlo: out << "    ctx.lo = " << reg(d.rs) << ";\n"; break;
    case psprecomp::OpcodeKind::Mult:
        out << "    { const std::int64_t product = static_cast<std::int64_t>(static_cast<std::int32_t>(" << reg(d.rs) << ")) * "
            << "static_cast<std::int64_t>(static_cast<std::int32_t>(" << reg(d.rt) << ")); "
            << "ctx.lo = static_cast<std::uint32_t>(product); ctx.hi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(product) >> 32u); }\n";
        break;
    case psprecomp::OpcodeKind::Multu:
        out << "    { const std::uint64_t product = static_cast<std::uint64_t>(" << reg(d.rs) << ") * static_cast<std::uint64_t>(" << reg(d.rt) << "); "
            << "ctx.lo = static_cast<std::uint32_t>(product); ctx.hi = static_cast<std::uint32_t>(product >> 32u); }\n";
        break;
    case psprecomp::OpcodeKind::Div:
        // $zero is emitted as a literal 0u.  If it is also the divisor, do not
        // emit a syntactically present / or % expression at all: MSVC diagnoses
        // division by a compile-time zero even when it sits behind an ordinary
        // `if (divisor == 0)` guard.  This is also cheaper and exactly matches
        // Allegrex/MIPS divide-by-zero HI/LO semantics.
        if (d.rt == 0u) {
            out << "    { const std::int32_t dividend = static_cast<std::int32_t>(" << reg(d.rs) << "); "
                << "ctx.lo = dividend >= 0 ? 0xFFFFFFFFu : 1u; "
                << "ctx.hi = static_cast<std::uint32_t>(dividend); }\n";
        } else {
            out << "    { const std::int32_t dividend = static_cast<std::int32_t>(" << reg(d.rs) << "); "
                << "const std::int32_t divisor = static_cast<std::int32_t>(" << reg(d.rt) << "); "
                << "if (divisor == 0) { ctx.lo = dividend >= 0 ? 0xFFFFFFFFu : 1u; ctx.hi = static_cast<std::uint32_t>(dividend); } "
                << "else if (dividend == static_cast<std::int32_t>(0x80000000u) && divisor == -1) { ctx.lo = 0x80000000u; ctx.hi = 0u; } "
                << "else { ctx.lo = static_cast<std::uint32_t>(dividend / divisor); ctx.hi = static_cast<std::uint32_t>(dividend % divisor); } }\n";
        }
        break;
    case psprecomp::OpcodeKind::Divu:
        if (d.rt == 0u) {
            out << "    { const std::uint32_t dividend = " << reg(d.rs) << "; "
                << "ctx.lo = dividend <= 0xFFFFu ? 0xFFFFu : 0xFFFFFFFFu; ctx.hi = dividend; }\n";
        } else {
            out << "    { const std::uint32_t dividend = " << reg(d.rs) << "; const std::uint32_t divisor = " << reg(d.rt) << "; "
                << "if (divisor == 0u) { ctx.lo = dividend <= 0xFFFFu ? 0xFFFFu : 0xFFFFFFFFu; ctx.hi = dividend; } "
                << "else { ctx.lo = dividend / divisor; ctx.hi = dividend % divisor; } }\n";
        }
        break;
    case psprecomp::OpcodeKind::Mfc1:
        out << "    ctx.set_gpr(" << d.rt << ", std::bit_cast<std::uint32_t>(ctx.fpr[" << d.rd << "]));\n";
        break;
    case psprecomp::OpcodeKind::Mtc1:
        out << "    ctx.fpr[" << d.rd << "] = std::bit_cast<float>(" << reg(d.rt) << ");\n";
        break;
    case psprecomp::OpcodeKind::Cfc1:
        if (d.rd == 31u) out << "    ctx.set_gpr(" << d.rt << ", ctx.fcr31);\n";
        else out << "    rt.unsupported(" << psprecomp::hex32(pc) << "u, " << psprecomp::hex32(d.word) << "u, \"unsupported CFC1 control register\"); return;\n";
        break;
    case psprecomp::OpcodeKind::Ctc1:
        if (d.rd == 31u) out << "    ctx.fcr31 = " << reg(d.rt) << " & 0x0181FFFFu;\n";
        else out << "    rt.unsupported(" << psprecomp::hex32(pc) << "u, " << psprecomp::hex32(d.word) << "u, \"unsupported CTC1 control register\"); return;\n";
        break;
    case psprecomp::OpcodeKind::AddS:
        out << "    ctx.fpr[" << d.sa << "] = AllegrexContext::fpu_add(ctx.fpr[" << d.rd << "], ctx.fpr[" << d.rt << "]);\n";
        break;
    case psprecomp::OpcodeKind::SubS:
        out << "    ctx.fpr[" << d.sa << "] = AllegrexContext::fpu_sub(ctx.fpr[" << d.rd << "], ctx.fpr[" << d.rt << "]);\n";
        break;
    case psprecomp::OpcodeKind::MulS:
        out << "    ctx.fpr[" << d.sa << "] = AllegrexContext::fpu_mul(ctx.fpr[" << d.rd << "], ctx.fpr[" << d.rt << "]);\n";
        break;
    case psprecomp::OpcodeKind::DivS:
        out << "    ctx.fpr[" << d.sa << "] = ctx.fpu_div(ctx.fpr[" << d.rd << "], ctx.fpr[" << d.rt << "]);\n";
        break;
    case psprecomp::OpcodeKind::SqrtS:
        out << "    ctx.fpr[" << d.sa << "] = AllegrexContext::fpu_sqrt(ctx.fpr[" << d.rd << "]);\n";
        break;
    case psprecomp::OpcodeKind::AbsS:
        out << "    ctx.set_fpr_bits(" << d.sa << ", ctx.fpr_bits(" << d.rd << ") & 0x7FFFFFFFu);\n";
        break;
    case psprecomp::OpcodeKind::MovS:
        out << "    ctx.set_fpr_bits(" << d.sa << ", ctx.fpr_bits(" << d.rd << "));\n";
        break;
    case psprecomp::OpcodeKind::NegS:
        out << "    ctx.set_fpr_bits(" << d.sa << ", ctx.fpr_bits(" << d.rd << ") ^ 0x80000000u);\n";
        break;
    case psprecomp::OpcodeKind::RoundWS:
        out << "    ctx.set_fpr_bits(" << d.sa << ", ctx.fpu_float_to_word(ctx.fpr[" << d.rd << "], 0u));\n";
        break;
    case psprecomp::OpcodeKind::TruncWS:
        out << "    ctx.set_fpr_bits(" << d.sa << ", ctx.fpu_float_to_word(ctx.fpr[" << d.rd << "], 1u));\n";
        break;
    case psprecomp::OpcodeKind::CeilWS:
        out << "    ctx.set_fpr_bits(" << d.sa << ", ctx.fpu_float_to_word(ctx.fpr[" << d.rd << "], 2u));\n";
        break;
    case psprecomp::OpcodeKind::FloorWS:
        out << "    ctx.set_fpr_bits(" << d.sa << ", ctx.fpu_float_to_word(ctx.fpr[" << d.rd << "], 3u));\n";
        break;
    case psprecomp::OpcodeKind::CvtWS:
        out << "    ctx.set_fpr_bits(" << d.sa << ", ctx.fpu_float_to_word(ctx.fpr[" << d.rd << "], 4u));\n";
        break;
    case psprecomp::OpcodeKind::CvtSW:
        out << "    ctx.fpr[" << d.sa << "] = static_cast<float>(static_cast<std::int32_t>(ctx.fpr_bits(" << d.rd << ")));\n";
        break;
    case psprecomp::OpcodeKind::FpuCompare: {
        const std::uint32_t condition = d.word & 0xFu;
        const std::string fs = "ctx.fpr[" + std::to_string(d.rd) + "]";
        const std::string ft = "ctx.fpr[" + std::to_string(d.rt) + "]";
        const std::string unordered = "(std::isnan(" + fs + ") || std::isnan(" + ft + "))";
        std::string expression;
        switch (condition) {
        case 0u: case 8u: expression = "false"; break;
        case 1u: case 9u: expression = unordered; break;
        case 2u: case 10u: expression = "(!" + unordered + " && " + fs + " == " + ft + ")"; break;
        case 3u: case 11u: expression = "(" + unordered + " || " + fs + " == " + ft + ")"; break;
        case 4u: case 12u: expression = "(" + fs + " < " + ft + ")"; break;
        case 5u: case 13u: expression = "(" + unordered + " || " + fs + " < " + ft + ")"; break;
        case 6u: case 14u: expression = "(" + fs + " <= " + ft + ")"; break;
        default: expression = "(" + unordered + " || " + fs + " <= " + ft + ")"; break;
        }
        out << "    ctx.set_fpu_condition(" << expression << ");\n";
        break;
    }
    case psprecomp::OpcodeKind::Vflush:
        if ((d.word & 0xFFFF0000u) == 0xFFFF0000u) {
            out << "    // vflush: architectural no-op that retains VFPU prefixes\n";
        } else {
            out << "    ctx.eat_vfpu_prefixes(); // VFPU sync/no-op consumes prefixes\n";
        }
        break;
    case psprecomp::OpcodeKind::Vpfx: {
        const std::uint32_t control = (d.word >> 24u) & 3u;
        std::uint32_t data = d.word & 0x000FFFFFu;
        if (control == 2u) data &= 0x00000FFFu;
        out << "    ctx.vfpu_ctrl[" << control << "u] = " << psprecomp::hex32(data) << "u;\n";
        break;
    }
    case psprecomp::OpcodeKind::Viim: {
        const std::uint32_t destination = (d.word >> 16u) & 0x7Fu;
        const std::int32_t immediate = static_cast<std::int16_t>(d.word & 0xFFFFu);
        out << "    { const float vfpu_value[1]{static_cast<float>(" << immediate << ")};\n"
            << "      ctx.write_vfpu_vector_with_destination_prefix(vfpu_value, " << destination << "u, 1u); }\n";
        break;
    }
    case psprecomp::OpcodeKind::Vfim: {
        const std::uint32_t destination = (d.word >> 16u) & 0x7Fu;
        const std::uint32_t half = d.word & 0xFFFFu;
        out << "    { const std::uint16_t vfpu_half = " << half << "u;\n"
            << "      const std::uint32_t vfpu_sign = static_cast<std::uint32_t>(vfpu_half & 0x8000u) << 16u;\n"
            << "      std::uint32_t vfpu_exponent = (vfpu_half >> 10u) & 0x1Fu;\n"
            << "      std::uint32_t vfpu_mantissa = vfpu_half & 0x03FFu;\n"
            << "      std::uint32_t vfpu_bits = 0u;\n"
            << "      if (vfpu_exponent == 0u) {\n"
            << "        if (vfpu_mantissa == 0u) vfpu_bits = vfpu_sign;\n"
            << "        else {\n"
            << "          std::uint32_t shift = 0u;\n"
            << "          while ((vfpu_mantissa & 0x0400u) == 0u) { vfpu_mantissa <<= 1u; ++shift; }\n"
            << "          vfpu_mantissa &= 0x03FFu;\n"
            << "          vfpu_bits = vfpu_sign | ((113u - shift) << 23u) | (vfpu_mantissa << 13u);\n"
            << "        }\n"
            << "      } else if (vfpu_exponent == 31u) {\n"
            << "        vfpu_bits = vfpu_sign | 0x7F800000u | (vfpu_mantissa << 13u);\n"
            << "      } else {\n"
            << "        vfpu_bits = vfpu_sign | ((vfpu_exponent + 112u) << 23u) | (vfpu_mantissa << 13u);\n"
            << "      }\n"
            << "      const float vfpu_value[1]{std::bit_cast<float>(vfpu_bits)};\n"
            << "      ctx.write_vfpu_vector_with_destination_prefix(vfpu_value, " << destination << "u, 1u); }\n";
        break;
    }
    case psprecomp::OpcodeKind::Vf2h: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        out << "    ctx.execute_vfpu_vf2h(" << destination << "u, " << source << "u, "
            << length << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Vh2f: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t source_length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        out << "    ctx.execute_vfpu_vh2f(" << destination << "u, " << source << "u, "
            << source_length << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Vf2i: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t immediate = (d.word >> 16u) & 31u;
        const std::uint32_t mode = (d.word >> 21u) & 31u;
        out << "    { float vfpu_s[4]{}; std::int32_t vfpu_d[4]{};\n"
            << "      ctx.read_vfpu_vector(vfpu_s, " << source << "u, " << length << "u);\n"
            << "      ctx.apply_vfpu_source_prefix(vfpu_s, " << length << "u, 0u);\n"
            << "      const float vfpu_scale = std::ldexp(1.0f, static_cast<int>(" << immediate << "u));\n"
            << "      for (std::uint32_t vfpu_i = 0; vfpu_i < " << length << "u; ++vfpu_i) {\n"
            << "        if (std::isnan(vfpu_s[vfpu_i])) { vfpu_d[vfpu_i] = std::numeric_limits<std::int32_t>::max(); continue; }\n"
            << "        const double vfpu_scaled = static_cast<double>(vfpu_s[vfpu_i] * vfpu_scale);\n"
            << "        if (vfpu_scaled > static_cast<double>(std::numeric_limits<std::int32_t>::max())) vfpu_d[vfpu_i] = std::numeric_limits<std::int32_t>::max();\n"
            << "        else if (vfpu_scaled <= static_cast<double>(std::numeric_limits<std::int32_t>::min())) vfpu_d[vfpu_i] = std::numeric_limits<std::int32_t>::min();\n"
            << "        else { double vfpu_rounded = 0.0;\n"
            << "          switch (" << mode << "u) {\n"
            << "          case 16u: vfpu_rounded = psprecomp::AllegrexContext::round_ties_to_even(vfpu_scaled); break;\n"
            << "          case 17u: vfpu_rounded = std::trunc(vfpu_scaled); break;\n"
            << "          case 18u: vfpu_rounded = std::ceil(vfpu_scaled); break;\n"
            << "          default: vfpu_rounded = std::floor(vfpu_scaled); break;\n"
            << "          }\n"
            << "          vfpu_d[vfpu_i] = static_cast<std::int32_t>(vfpu_rounded);\n"
            << "        }\n"
            << "      }\n"
            << "      const std::uint32_t vfpu_destination_prefix = ctx.vfpu_ctrl[2];\n"
            << "      for (std::uint32_t vfpu_i = 0; vfpu_i < " << length << "u; ++vfpu_i) {\n"
            << "        if (((vfpu_destination_prefix >> (8u + vfpu_i)) & 1u) == 0u)\n"
            << "          ctx.vfpu[psprecomp::AllegrexContext::vfpu_vector_lane_index(" << destination << "u, " << length << "u, vfpu_i)] = std::bit_cast<float>(static_cast<std::uint32_t>(vfpu_d[vfpu_i]));\n"
            << "      }\n"
            << "      ctx.eat_vfpu_prefixes(); }\n";
        break;
    }
    case psprecomp::OpcodeKind::Vi2f: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t immediate = (d.word >> 16u) & 31u;
        out << "    { float vfpu_s[4]{}, vfpu_d[4]{};\n"
            << "      ctx.read_vfpu_vector(vfpu_s, " << source << "u, " << length << "u);\n"
            << "      ctx.apply_vfpu_source_prefix(vfpu_s, " << length << "u, 0u);\n"
            << "      const float vfpu_scale = std::ldexp(1.0f, -static_cast<int>(" << immediate << "u));\n"
            << "      for (std::uint32_t vfpu_i = 0; vfpu_i < " << length << "u; ++vfpu_i) {\n"
            << "        const auto vfpu_integer = static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(vfpu_s[vfpu_i]));\n"
            << "        vfpu_d[vfpu_i] = static_cast<float>(vfpu_integer) * vfpu_scale;\n"
            << "      }\n"
            << "      ctx.write_vfpu_vector_with_destination_prefix(vfpu_d, " << destination << "u, " << length << "u); }\n";
        break;
    }
    case psprecomp::OpcodeKind::Vx2i: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t source_length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t operation = (d.word >> 16u) & 3u;
        out << "    ctx.execute_vfpu_vx2i(" << destination << "u, " << source << "u, "
            << source_length << "u, " << operation << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Mtv:
        out << "    ctx.set_vfpu_scalar_bits(" << (d.word & 0xFFu) << "u, " << reg(d.rt) << ");\n";
        break;
    case psprecomp::OpcodeKind::Mfv:
        out << "    ctx.set_gpr(" << d.rt << ", ctx.vfpu_scalar_bits(" << (d.word & 0xFFu) << "u));\n";
        break;
    case psprecomp::OpcodeKind::VmidT: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t side = size_code + 1u;
        out << "    ctx.execute_vfpu_matrix_init(" << (d.word & 0x7Fu) << "u, " << side << "u, 3u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Vmmov: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t side = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        out << "    ctx.execute_vfpu_vmmov(" << destination << "u, " << source << "u, "
            << side << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::VfpuMatrixInit: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t side = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t operation = (d.word >> 16u) & 15u;
        out << "    ctx.execute_vfpu_matrix_init(" << destination << "u, " << side << "u, "
            << operation << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Vidt: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t offset_mask = length >= 3u ? 3u : 1u;
        const std::uint32_t one_lane = destination & offset_mask;
        out << "    { float vfpu_value[4]{}; vfpu_value[" << one_lane << "u] = 1.0f;\n"
            << "      ctx.write_vfpu_vector_with_destination_prefix(vfpu_value, " << destination
            << "u, " << length << "u); }\n";
        break;
    }
    case psprecomp::OpcodeKind::Vtfm: {
        const std::uint32_t side = ((d.word >> 23u) & 3u) + 1u;
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t input_length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t target = (d.word >> 16u) & 0x7Fu;
        out << "    { float vfpu_matrix[16]{}, vfpu_target_raw[4]{}, vfpu_target[4]{}, vfpu_result[4]{};\n"
            << "      ctx.read_vfpu_matrix(vfpu_matrix, " << source << "u, " << side << "u);\n"
            << "      ctx.read_vfpu_vector(vfpu_target_raw, " << target << "u, " << side << "u);\n"
            << "      constexpr std::uint32_t vfpu_side = " << side << "u;\n"
            << "      constexpr std::uint32_t vfpu_input_length = " << input_length << "u;\n"
            << "      for (std::uint32_t i = 0; i < 4u; ++i) vfpu_target[i] = i < vfpu_input_length ? vfpu_target_raw[i] : 0.0f;\n"
            << "      if (vfpu_side - 1u >= vfpu_input_length) vfpu_target[vfpu_side - 1u] = 1.0f;\n"
            << "      for (std::uint32_t row = 0; row + 1u < vfpu_side; ++row) {\n"
            << "        float sum = 0.0f;\n"
            << "        for (std::uint32_t column = 0; column < vfpu_side; ++column) sum += vfpu_matrix[row * 4u + column] * vfpu_target[column];\n"
            << "        vfpu_result[row] = sum;\n"
            << "      }\n"
            << "      float vfpu_final_row[4]{vfpu_matrix[(vfpu_side - 1u) * 4u + 0u], vfpu_matrix[(vfpu_side - 1u) * 4u + 1u],\n"
            << "                              vfpu_matrix[(vfpu_side - 1u) * 4u + 2u], vfpu_matrix[(vfpu_side - 1u) * 4u + 3u]};\n"
            << "      ctx.apply_vfpu_source_prefix(vfpu_final_row, 4u, 0u);\n"
            << "      ctx.apply_vfpu_source_prefix(vfpu_target, 4u, 1u);\n"
            << "      for (std::uint32_t column = 0; column < 4u; ++column) vfpu_result[vfpu_side - 1u] += vfpu_final_row[column] * vfpu_target[column];\n"
            << "      const std::uint32_t vfpu_destination_prefix = ctx.vfpu_ctrl[2];\n"
            << "      const std::uint32_t vfpu_last_lane = vfpu_side - 1u;\n"
            << "      ctx.vfpu_ctrl[2] = ((vfpu_destination_prefix & (1u << 8u)) << vfpu_last_lane) |\n"
            << "                         ((vfpu_destination_prefix & 3u) << (vfpu_last_lane * 2u));\n"
            << "      ctx.write_vfpu_vector_with_destination_prefix(vfpu_result, " << destination << "u, vfpu_side); }\n";
        break;
    }
    case psprecomp::OpcodeKind::VfpuVectorInit: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const bool one = (((d.word >> 16u) & 31u) == 7u);
        out << "    { float vfpu_value[4]{" << (one ? "1.0f, 1.0f, 1.0f, 1.0f" : "") << "};\n"
            << "      ctx.write_vfpu_vector_with_destination_prefix(vfpu_value, " << destination
            << "u, " << length << "u); }\n";
        break;
    }
    case psprecomp::OpcodeKind::Vmmul: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t side = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t target = (d.word >> 16u) & 0x7Fu;
        out << "    { float vfpu_s[16]{}, vfpu_t[16]{}, vfpu_d[16]{};\n"
            << "      ctx.read_vfpu_matrix(vfpu_s, " << source << "u, " << side << "u);\n"
            << "      ctx.read_vfpu_matrix(vfpu_t, " << target << "u, " << side << "u);\n"
            << "      for (std::uint32_t a = 0; a < " << side << "u; ++a) {\n"
            << "        for (std::uint32_t b = 0; b < " << side << "u; ++b) {\n"
            << "          float sum = 0.0f;\n"
            << "          for (std::uint32_t c = 0; c < " << side << "u; ++c) sum += vfpu_s[b * 4u + c] * vfpu_t[a * 4u + c];\n"
            << "          vfpu_d[a * 4u + b] = sum;\n"
            << "        }\n"
            << "      }\n"
            << "      ctx.write_vfpu_matrix(vfpu_d, " << destination << "u, " << side << "u);\n"
            << "      ctx.eat_vfpu_prefixes(); }\n";
        break;
    }
    case psprecomp::OpcodeKind::Vmscl: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t side = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t target = (d.word >> 16u) & 0x7Fu;
        out << "    ctx.execute_vfpu_vmscl(" << destination << "u, " << source << "u, "
            << target << "u, " << side << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Vrot: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t immediate = (d.word >> 16u) & 31u;
        out << "    ctx.execute_vfpu_vrot(" << destination << "u, " << source << "u, "
            << length << "u, " << immediate << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Vocp: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        out << "    ctx.execute_vfpu_vocp(" << destination << "u, " << source << "u, "
            << length << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::VfpuHorizontal: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const bool average = ((d.word >> 16u) & 31u) == 7u;
        out << "    ctx.execute_vfpu_horizontal(" << destination << "u, " << source << "u, "
            << length << "u, " << (average ? "true" : "false") << ");\n";
        break;
    }
    case psprecomp::OpcodeKind::VfpuVec3: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t target = (d.word >> 16u) & 0x7Fu;
        const std::uint32_t major = d.word >> 26u;
        const std::uint32_t operation = major == 0x19u ? 2u : ((d.word >> 23u) & 7u);
        const char *expression = operation == 0u ? "vfpu_s[i] + vfpu_t[i]"
                               : operation == 1u ? "vfpu_s[i] - vfpu_t[i]"
                               : operation == 2u ? "vfpu_s[i] * vfpu_t[i]"
                                                 : "vfpu_s[i] / vfpu_t[i]";
        out << "    { float vfpu_s[4]{}, vfpu_t[4]{}, vfpu_d[4]{};\n"
            << "      ctx.read_vfpu_vector_with_source_prefix(vfpu_s, " << source << "u, " << length << "u, 0u);\n"
            << "      ctx.read_vfpu_vector_with_source_prefix(vfpu_t, " << target << "u, " << length << "u, 1u);\n"
            << "      for (std::uint32_t i = 0; i < " << length << "u; ++i) vfpu_d[i] = " << expression << ";\n"
            << "      ctx.write_vfpu_vector_with_destination_prefix(vfpu_d, " << destination << "u, " << length << "u); }\n";
        break;
    }
    case psprecomp::OpcodeKind::Vdot: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t target = (d.word >> 16u) & 0x7Fu;
        out << "    ctx.execute_vfpu_vdot(" << destination << "u, " << source << "u, "
            << target << "u, " << length << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Vhdp: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t target = (d.word >> 16u) & 0x7Fu;
        out << "    ctx.execute_vfpu_vhdp(" << destination << "u, " << source << "u, "
            << target << "u, " << length << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::VcrossQuat: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t target = (d.word >> 16u) & 0x7Fu;
        out << "    ctx.execute_vfpu_cross_quat(" << destination << "u, " << source << "u, "
            << target << "u, " << length << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Vminmax: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t target = (d.word >> 16u) & 0x7Fu;
        const bool maximum = ((d.word >> 23u) & 7u) == 3u;
        out << "    ctx.execute_vfpu_vminmax(" << destination << "u, " << source << "u, "
            << target << "u, " << length << "u, " << (maximum ? "true" : "false") << ");\n";
        break;
    }
    case psprecomp::OpcodeKind::VfpuCompare3: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t target = (d.word >> 16u) & 0x7Fu;
        const std::uint32_t operation = (d.word >> 23u) & 7u;
        out << "    ctx.execute_vfpu_compare3(" << destination << "u, " << source << "u, "
            << target << "u, " << length << "u, " << operation << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Vcmp: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t target = (d.word >> 16u) & 0x7Fu;
        const std::uint32_t condition = d.word & 15u;
        out << "    ctx.execute_vfpu_vcmp(" << source << "u, " << target << "u, "
            << length << "u, " << condition << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::Vcmov: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t condition_index = (d.word >> 16u) & 7u;
        const bool move_if_false = ((d.word >> 19u) & 1u) != 0u;
        out << "    ctx.execute_vfpu_vcmov(" << destination << "u, " << source << "u, "
            << length << "u, " << condition_index << "u, "
            << (move_if_false ? "true" : "false") << ");\n";
        break;
    }
    case psprecomp::OpcodeKind::Vscl: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t target = (d.word >> 16u) & 0x7Fu;
        out << "    ctx.execute_vfpu_vscl(" << destination << "u, " << source << "u, "
            << target << "u, " << length << "u);\n";
        break;
    }
    case psprecomp::OpcodeKind::VfpuUnary: {
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t destination = d.word & 0x7Fu;
        const std::uint32_t source = (d.word >> 8u) & 0x7Fu;
        const std::uint32_t operation = (d.word >> 16u) & 31u;
        std::string expression;
        switch (operation) {
        case 0u: expression = "vfpu_s[i]"; break;
        case 1u: expression = "std::fabs(vfpu_s[i])"; break;
        case 2u: expression = "-vfpu_s[i]"; break;
        case 4u: expression = "vfpu_s[i] <= 0.0f ? 0.0f : (vfpu_s[i] > 1.0f ? 1.0f : vfpu_s[i])"; break;
        case 5u: expression = "vfpu_s[i] < -1.0f ? -1.0f : (vfpu_s[i] > 1.0f ? 1.0f : vfpu_s[i])"; break;
        case 16u: expression = "1.0f / vfpu_s[i]"; break;
        case 17u: expression = "1.0f / std::sqrt(vfpu_s[i])"; break;
        case 18u: expression = "std::sin(vfpu_s[i] * 1.57079632679489661923f)"; break;
        case 19u: expression = "std::cos(vfpu_s[i] * 1.57079632679489661923f)"; break;
        case 20u: expression = "std::exp2(vfpu_s[i])"; break;
        case 21u: expression = "std::log2(vfpu_s[i])"; break;
        case 22u: expression = "std::fabs(std::sqrt(vfpu_s[i]))"; break;
        case 23u: expression = "std::asin(vfpu_s[i]) * 0.63661977236758134308f"; break;
        case 24u: expression = "-1.0f / vfpu_s[i]"; break;
        case 26u: expression = "-std::sin(vfpu_s[i] * 1.57079632679489661923f)"; break;
        default: expression = "1.0f / std::exp2(vfpu_s[i])"; break;
        }
        out << "    { float vfpu_s[4]{}, vfpu_d[4]{};\n"
            << "      ctx.read_vfpu_vector_with_source_prefix(vfpu_s, " << source << "u, " << length << "u, 0u);\n"
            << "      for (std::uint32_t i = 0; i < " << length << "u; ++i) vfpu_d[i] = " << expression << ";\n"
            << "      ctx.write_vfpu_vector_with_destination_prefix(vfpu_d, " << destination << "u, " << length << "u); }\n";
        break;
    }
    case psprecomp::OpcodeKind::Vcst: {
        static constexpr std::uint32_t constant_bits[32] = {
            0x00000000u, 0x7F7FFFFFu, 0x3FB504F3u, 0x3F3504F3u,
            0x3F906EBAu, 0x3F22F983u, 0x3EA2F983u, 0x3F490FDBu,
            0x3FC90FDBu, 0x40490FDBu, 0x402DF854u, 0x3FB8AA3Bu,
            0x3EDE5BD9u, 0x3F317218u, 0x40135D8Eu, 0x40C90FDBu,
            0x3F060A92u, 0x3E9A209Bu, 0x40549A78u, 0x3F5DB3D7u,
            0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
        };
        const std::uint32_t size_code = ((d.word >> 7u) & 1u) | (((d.word >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        const std::uint32_t selector = (d.word >> 16u) & 31u;
        const std::uint32_t destination = d.word & 0x7Fu;
        out << "    { const float vfpu_constant = std::bit_cast<float>("
            << psprecomp::hex32(constant_bits[selector]) << "u);\n"
            << "      const float vfpu_value[4]{vfpu_constant, vfpu_constant, vfpu_constant, vfpu_constant};\n"
            << "      ctx.write_vfpu_vector_with_destination_prefix(vfpu_value, " << destination
            << "u, " << length << "u); }\n";
        break;
    }
    case psprecomp::OpcodeKind::Lvs: {
        const std::int32_t offset = static_cast<std::int16_t>(d.word & 0xFFFCu);
        const std::uint32_t scalar_register = ((d.word >> 16u) & 0x1Fu) | ((d.word & 3u) << 5u);
        out << "    ctx.set_vfpu_scalar_bits(" << scalar_register << "u, rt.memory().aot_load32("
            << reg(d.rs) << " + static_cast<std::uint32_t>(" << offset << ")));\n";
        break;
    }
    case psprecomp::OpcodeKind::Svs: {
        const std::int32_t offset = static_cast<std::int16_t>(d.word & 0xFFFCu);
        const std::uint32_t scalar_register = ((d.word >> 16u) & 0x1Fu) | ((d.word & 3u) << 5u);
        out << "    rt.memory().aot_store32(" << reg(d.rs) << " + static_cast<std::uint32_t>(" << offset
            << "), ctx.vfpu_scalar_bits(" << scalar_register << "u));\n";
        break;
    }
    case psprecomp::OpcodeKind::Lvq: {
        const std::int32_t offset = static_cast<std::int16_t>(d.word & 0xFFFCu);
        const std::uint32_t vector_register = ((d.word >> 16u) & 0x1Fu) | ((d.word & 1u) << 5u);
        out << "    { const std::uint32_t vfpu_address = " << reg(d.rs) << " + static_cast<std::uint32_t>(" << offset << ");\n"
            << "      float vfpu_value[4]{\n"
            << "        std::bit_cast<float>(rt.memory().aot_load32(vfpu_address + 0u)),\n"
            << "        std::bit_cast<float>(rt.memory().aot_load32(vfpu_address + 4u)),\n"
            << "        std::bit_cast<float>(rt.memory().aot_load32(vfpu_address + 8u)),\n"
            << "        std::bit_cast<float>(rt.memory().aot_load32(vfpu_address + 12u))};\n"
            << "      ctx.write_vfpu_vector(vfpu_value, " << vector_register << "u, 4u); }\n";
        break;
    }
    case psprecomp::OpcodeKind::Svq: {
        const std::int32_t offset = static_cast<std::int16_t>(d.word & 0xFFFCu);
        const std::uint32_t vector_register = ((d.word >> 16u) & 0x1Fu) | ((d.word & 1u) << 5u);
        out << "    { float vfpu_value[4]{}; ctx.read_vfpu_vector(vfpu_value, " << vector_register << "u, 4u);\n"
            << "      const std::uint32_t vfpu_address = " << reg(d.rs) << " + static_cast<std::uint32_t>(" << offset << ");\n"
            << "      rt.memory().aot_store32(vfpu_address + 0u, std::bit_cast<std::uint32_t>(vfpu_value[0]));\n"
            << "      rt.memory().aot_store32(vfpu_address + 4u, std::bit_cast<std::uint32_t>(vfpu_value[1]));\n"
            << "      rt.memory().aot_store32(vfpu_address + 8u, std::bit_cast<std::uint32_t>(vfpu_value[2]));\n"
            << "      rt.memory().aot_store32(vfpu_address + 12u, std::bit_cast<std::uint32_t>(vfpu_value[3])); }\n";
        break;
    }
    default:
        out << "    rt.unsupported(" << psprecomp::hex32(pc) << "u, " << psprecomp::hex32(d.word)
            << "u, \"" << d.mnemonic << " not lowered yet\"); return;\n";
        break;
    }
    return out.str();
}

// VFPU encodings the generic decoder leaves as OpcodeKind::Vfpu: the VFPU4
// group-2 vector ops (vsrt/vbfy/vsocp/vsgn) and unaligned quad load/stores.
std::optional<std::string> emit_extra_vfpu(const psprecomp::DecodedInstruction &d) {
    const std::uint32_t w = d.word, major = w >> 26u;
    std::ostringstream out;
    if (const std::uint32_t op = (w >> 16u) & 31u; major == 0x34u && ((w >> 21u) & 31u) == 1u && (op <= 3u || op >= 28u)) {
        const std::uint32_t size_code = ((w >> 7u) & 1u) | (((w >> 15u) & 1u) << 1u);
        out << "    ctx.execute_vfpu_" << (op <= 3u ? "vrnd(" : "vi2x(") << (w & 0x7Fu) << "u, "
            << ((w >> 8u) & 0x7Fu) << "u, " << size_code + 1u << "u, " << (op & 3u) << "u);\n";
        return out.str();
    }
    if (const std::uint32_t op = (w >> 16u) & 31u; major == 0x34u && ((w >> 21u) & 31u) == 2u && op >= 25u && op <= 27u) {
        const std::uint32_t size_code = ((w >> 7u) & 1u) | (((w >> 15u) & 1u) << 1u);
        out << "    ctx.execute_vfpu_vt_color(" << (w & 0x7Fu) << "u, " << ((w >> 8u) & 0x7Fu) << "u, "
            << size_code + 1u << "u, " << (op & 3u) << "u);\n";
        return out.str();
    }
    if (major == 0x34u && ((w >> 21u) & 31u) == 2u) {
        const std::uint32_t size_code = ((w >> 7u) & 1u) | (((w >> 15u) & 1u) << 1u);
        const std::uint32_t length = size_code + 1u;
        std::uint32_t out_length = length;
        const std::uint32_t destination = w & 0x7Fu, source = (w >> 8u) & 0x7Fu;
        std::string op;
        switch ((w >> 16u) & 31u) {
        case 0: op = "d[0] = s[0] < s[1] ? s[0] : s[1]; d[1] = s[0] < s[1] ? s[1] : s[0]; "
                     "d[2] = s[2] < s[3] ? s[2] : s[3]; d[3] = s[2] < s[3] ? s[3] : s[2];"; break; // vsrt1
        case 1: op = "d[0] = s[0] < s[3] ? s[0] : s[3]; d[1] = s[1] < s[2] ? s[1] : s[2]; "
                     "d[2] = s[1] < s[2] ? s[2] : s[1]; d[3] = s[0] < s[3] ? s[3] : s[0];"; break; // vsrt2
        case 2: op = "d[0] = s[0] + s[1]; d[1] = s[0] - s[1]; d[2] = s[2] + s[3]; d[3] = s[2] - s[3];"; break; // vbfy1
        case 3: op = "d[0] = s[0] + s[2]; d[1] = s[1] + s[3]; d[2] = s[0] - s[2]; d[3] = s[1] - s[3];"; break; // vbfy2
        case 5: // vsocp: (1 - x, x) per input lane, saturated to [0, 1]
            op = "for (std::uint32_t i = 0; i < " + std::to_string(length) + "u && i < 2u; ++i) { "
                 "const float a = 1.0f - s[i], b = s[i]; d[i * 2u] = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a); "
                 "d[i * 2u + 1u] = b < 0.0f ? 0.0f : (b > 1.0f ? 1.0f : b); }";
            out_length = length * 2u > 4u ? 4u : length * 2u;
            break;
        case 8: op = "d[0] = s[0] < s[1] ? s[1] : s[0]; d[1] = s[0] < s[1] ? s[0] : s[1]; "
                     "d[2] = s[2] < s[3] ? s[3] : s[2]; d[3] = s[2] < s[3] ? s[2] : s[3];"; break; // vsrt3
        case 9: op = "d[0] = s[0] < s[3] ? s[3] : s[0]; d[1] = s[1] < s[2] ? s[2] : s[1]; "
                     "d[2] = s[1] < s[2] ? s[1] : s[2]; d[3] = s[0] < s[3] ? s[0] : s[3];"; break; // vsrt4
        case 10: // vsgn
            op = "for (std::uint32_t i = 0; i < " + std::to_string(length) +
                 "u; ++i) d[i] = s[i] > 0.0f ? 1.0f : (s[i] < 0.0f ? -1.0f : 0.0f);";
            break;
        default: return std::nullopt;
        }
        out << "    { float s[4]{}, d[4]{};\n"
            << "      ctx.read_vfpu_vector_with_source_prefix(s, " << source << "u, " << length << "u, 0u);\n"
            << "      " << op << "\n"
            << "      ctx.write_vfpu_vector_with_destination_prefix(d, " << destination << "u, " << out_length << "u); }\n";
        return out.str();
    }
    if (major == 0x35u || major == 0x3Du) { // lvl.q/lvr.q, svl.q/svr.q
        const std::int32_t offset = static_cast<std::int16_t>(w & 0xFFFCu);
        const std::uint32_t vector_register = ((w >> 16u) & 0x1Fu) | ((w & 1u) << 5u);
        const bool right = (w & 2u) != 0u;
        out << "    { const std::uint32_t vfpu_address = " << reg((w >> 21u) & 31u) << " + static_cast<std::uint32_t>("
            << offset << ");\n"
            << "      const std::uint32_t vfpu_lane = (vfpu_address >> 2u) & 3u;\n"
            << "      float q[4]{};\n"
            << "      ctx.read_vfpu_vector(q, " << vector_register << "u, 4u);\n";
        if (major == 0x35u) {
            if (!right)
                out << "      for (std::uint32_t i = 0; i <= vfpu_lane; ++i) q[3u - i] = std::bit_cast<float>("
                       "rt.memory().aot_load32(vfpu_address - 4u * i));\n";
            else
                out << "      for (std::uint32_t i = 0; i <= 3u - vfpu_lane; ++i) q[i] = std::bit_cast<float>("
                       "rt.memory().aot_load32(vfpu_address + 4u * i));\n";
            out << "      ctx.write_vfpu_vector(q, " << vector_register << "u, 4u); }\n";
        } else {
            if (!right)
                out << "      for (std::uint32_t i = 0; i <= vfpu_lane; ++i) rt.memory().aot_store32("
                       "vfpu_address - 4u * i, std::bit_cast<std::uint32_t>(q[3u - i]));\n";
            else
                out << "      for (std::uint32_t i = 0; i <= 3u - vfpu_lane; ++i) rt.memory().aot_store32("
                       "vfpu_address + 4u * i, std::bit_cast<std::uint32_t>(q[i]));\n";
            out << "    }\n";
        }
        return out.str();
    }
    return std::nullopt;
}

bool is_branch(psprecomp::OpcodeKind kind) {
    using K = psprecomp::OpcodeKind;
    switch (kind) {
    case K::Beq: case K::Bne: case K::Beql: case K::Bnel:
    case K::Blez: case K::Bgtz: case K::Blezl: case K::Bgtzl:
    case K::Bltz: case K::Bgez: case K::Bltzl: case K::Bgezl:
    case K::Bltzal: case K::Bgezal: case K::Bltzall: case K::Bgezall:
    case K::Bc1f: case K::Bc1t: case K::Bc1fl: case K::Bc1tl:
    case K::Bvf: case K::Bvt: case K::Bvfl: case K::Bvtl:
        return true;
    default:
        return false;
    }
}

bool is_likely_branch(psprecomp::OpcodeKind kind) {
    using K = psprecomp::OpcodeKind;
    return kind == K::Beql || kind == K::Bnel || kind == K::Blezl || kind == K::Bgtzl ||
           kind == K::Bltzl || kind == K::Bgezl || kind == K::Bltzall || kind == K::Bgezall ||
           kind == K::Bc1fl || kind == K::Bc1tl || kind == K::Bvfl || kind == K::Bvtl;
}

bool is_link_branch(psprecomp::OpcodeKind kind) {
    using K = psprecomp::OpcodeKind;
    return kind == K::Bltzal || kind == K::Bgezal || kind == K::Bltzall || kind == K::Bgezall;
}

std::string branch_condition(const psprecomp::DecodedInstruction &d) {
    using K = psprecomp::OpcodeKind;
    switch (d.kind) {
    case K::Beq: case K::Beql: return reg(d.rs) + " == " + reg(d.rt);
    case K::Bne: case K::Bnel: return reg(d.rs) + " != " + reg(d.rt);
    case K::Blez: case K::Blezl: return "static_cast<std::int32_t>(" + reg(d.rs) + ") <= 0";
    case K::Bgtz: case K::Bgtzl: return "static_cast<std::int32_t>(" + reg(d.rs) + ") > 0";
    case K::Bltz: case K::Bltzl: case K::Bltzal: case K::Bltzall:
        return "static_cast<std::int32_t>(" + reg(d.rs) + ") < 0";
    case K::Bgez: case K::Bgezl: case K::Bgezal: case K::Bgezall:
        return "static_cast<std::int32_t>(" + reg(d.rs) + ") >= 0";
    case K::Bc1f: case K::Bc1fl: return "!ctx.fpu_condition()";
    case K::Bc1t: case K::Bc1tl: return "ctx.fpu_condition()";
    case K::Bvf: case K::Bvfl: {
        const std::uint32_t condition_index = (d.word >> 18u) & 7u;
        return "((ctx.vfpu_ctrl[3] >> " + std::to_string(condition_index) + "u) & 1u) == 0u";
    }
    case K::Bvt: case K::Bvtl: {
        const std::uint32_t condition_index = (d.word >> 18u) & 7u;
        return "((ctx.vfpu_ctrl[3] >> " + std::to_string(condition_index) + "u) & 1u) != 0u";
    }
    default: return "false";
    }
}

// The generated corpus uses architectural register numbers known
// at code-generation time.  Keeping those writes behind AllegrexContext::set_gpr()
// leaves a call/branch-shaped abstraction at more than half a million static
// sites.  MSVC cannot reliably inline a tiny helper into the enormous AOT unit
// functions even under LTCG, so lower constant writes in the emitted source:
//   ctx.set_gpr(8, expr) -> ctx.gpr[8] = (expr)
//   ctx.set_gpr(0, expr) -> (void)(expr)
// The second form preserves source/memory evaluation (including any guest
// fault/side effect) while discarding only the architectural write to $zero.
// This pass is deliberately limited to --auto output; manual fixtures retain the
// readable helper form used by their source-level tests.
std::string lower_constant_gpr_writes(std::string text) {
    constexpr std::string_view needle = "ctx.set_gpr(";
    std::size_t search = 0u;
    while ((search = text.find(needle, search)) != std::string::npos) {
        const std::size_t open = search + needle.size() - 1u;
        std::size_t cursor = open + 1u;
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) ++cursor;
        const std::size_t index_begin = cursor;
        while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor]))) ++cursor;
        if (cursor == index_begin) { search += needle.size(); continue; }
        const std::uint32_t index = static_cast<std::uint32_t>(
            std::strtoul(text.substr(index_begin, cursor - index_begin).c_str(), nullptr, 10));
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) ++cursor;
        if (cursor >= text.size() || text[cursor] != ',') { search += needle.size(); continue; }
        const std::size_t expression_begin = cursor + 1u;

        int paren_depth = 1;
        bool in_string = false;
        bool in_char = false;
        bool escaped = false;
        std::size_t close = std::string::npos;
        for (cursor = expression_begin; cursor < text.size(); ++cursor) {
            const char ch = text[cursor];
            if (escaped) { escaped = false; continue; }
            if ((in_string || in_char) && ch == '\\') { escaped = true; continue; }
            if (!in_char && ch == '"') { in_string = !in_string; continue; }
            if (!in_string && ch == '\'') { in_char = !in_char; continue; }
            if (in_string || in_char) continue;
            if (ch == '(') ++paren_depth;
            else if (ch == ')' && --paren_depth == 0) { close = cursor; break; }
        }
        if (close == std::string::npos) { search += needle.size(); continue; }

        std::size_t first = expression_begin;
        std::size_t last = close;
        while (first < last && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
        while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1u]))) --last;
        const std::string expression = text.substr(first, last - first);
        std::string replacement;
        if (index == 0u) {
            replacement = "(void)(" + expression + ")";
        } else if (index < 32u) {
            replacement = "ctx.gpr[" + std::to_string(index) + "] = (" + expression + ")";
        } else {
            // Defensive fallback.  Automatic Allegrex decoding should never emit
            // an out-of-range architectural GPR, but preserve validation if it does.
            search = close + 1u;
            continue;
        }
        text.replace(search, close - search + 1u, replacement);
        search += replacement.size();
    }
    return text;
}

std::string lower_aot_memory_accesses(std::string text) {
    // the outer generated-unit wrapper creates one AotFastView and
    // compile-time direct chains pass it by reference across unit boundaries.
    // Lower ordinary aligned byte/half/word accesses to that shared view.
    static const std::regex access_pattern(
        R"(rt\.memory\(\)\.aot_(load|store)(8|16|32)\()" );
    return std::regex_replace(text, access_pattern, "aot_mem.aot_$1$2(");
}

std::string lower_constant_fpr_accesses(std::string text) {
    // All Allegrex FPR operands emitted by the decoder are architectural
    // constants in [0,31]. Avoid helper calls/bounds branches in enormous AOT
    // functions so MSVC can keep float lanes in registers across basic blocks.
    const std::regex read_pattern(R"(ctx\.fpr_bits\(([0-9]+)\))");
    std::smatch match;
    std::string lowered;
    lowered.reserve(text.size());
    std::string::const_iterator begin = text.cbegin();
    while (std::regex_search(begin, text.cend(), match, read_pattern)) {
        lowered.append(begin, match[0].first);
        const auto index = static_cast<std::uint32_t>(std::strtoul(match[1].str().c_str(), nullptr, 10));
        if (index < 32u)
            lowered += "std::bit_cast<std::uint32_t>(ctx.fpr[" + std::to_string(index) + "])";
        else
            lowered.append(match[0].first, match[0].second);
        begin = match[0].second;
    }
    lowered.append(begin, text.cend());
    text = std::move(lowered);

    constexpr std::string_view needle = "ctx.set_fpr_bits(";
    std::size_t search = 0u;
    while ((search = text.find(needle, search)) != std::string::npos) {
        std::size_t cursor = search + needle.size();
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) ++cursor;
        const std::size_t index_begin = cursor;
        while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor]))) ++cursor;
        if (cursor == index_begin) { search += needle.size(); continue; }
        const auto index = static_cast<std::uint32_t>(
            std::strtoul(text.substr(index_begin, cursor - index_begin).c_str(), nullptr, 10));
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) ++cursor;
        if (index >= 32u || cursor >= text.size() || text[cursor] != ',') {
            search += needle.size(); continue;
        }
        const std::size_t expression_begin = cursor + 1u;
        int depth = 1;
        bool in_string = false, in_char = false, escaped = false;
        std::size_t close = std::string::npos;
        for (cursor = expression_begin; cursor < text.size(); ++cursor) {
            const char ch = text[cursor];
            if (escaped) { escaped = false; continue; }
            if ((in_string || in_char) && ch == '\\') { escaped = true; continue; }
            if (!in_char && ch == '"') { in_string = !in_string; continue; }
            if (!in_string && ch == '\'') { in_char = !in_char; continue; }
            if (in_string || in_char) continue;
            if (ch == '(') ++depth;
            else if (ch == ')' && --depth == 0) { close = cursor; break; }
        }
        if (close == std::string::npos) { search += needle.size(); continue; }
        std::size_t first = expression_begin, last = close;
        while (first < last && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
        while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1u]))) --last;
        const std::string expression = text.substr(first, last - first);
        const std::string replacement = "ctx.fpr[" + std::to_string(index) +
            "] = std::bit_cast<float>(" + expression + ")";
        text.replace(search, close - search + 1u, replacement);
        search += replacement.size();
    }
    return text;
}

// VFPU operands in automatic AOT are encoded literals. Convert the
// generic lane/prefix helpers into compile-time templates so row/column mapping,
// scalar-vs-control selection and 1..4-lane loops disappear before MSVC sees the
// giant translation units. Dynamic-size matrix helpers remain untouched.
std::string lower_constant_vfpu_accesses(std::string text) {
    const std::regex read_prefix(
        R"(ctx\.read_vfpu_vector_with_source_prefix\(([A-Za-z0-9_]+), ([0-9]+)u, ([1-4])u, ([01])u\);)" );
    text = std::regex_replace(text, read_prefix,
        "ctx.read_vfpu_vector_with_source_prefix_ct<$2u, $3u, $4u>($1);");

    const std::regex read_plain(
        R"(ctx\.read_vfpu_vector\(([A-Za-z0-9_]+), ([0-9]+)u, ([1-4])u\);)" );
    text = std::regex_replace(text, read_plain,
        "ctx.read_vfpu_vector_ct<$2u, $3u>($1);");

    const std::regex write_prefix(
        R"(ctx\.write_vfpu_vector_with_destination_prefix\(([A-Za-z0-9_]+), ([0-9]+)u, ([1-4])u\);)" );
    text = std::regex_replace(text, write_prefix,
        "ctx.write_vfpu_vector_with_destination_prefix_ct<$2u, $3u>($1);");

    const std::regex write_plain(
        R"(ctx\.write_vfpu_vector\(([A-Za-z0-9_]+), ([0-9]+)u, ([1-4])u\);)" );
    text = std::regex_replace(text, write_plain,
        "ctx.write_vfpu_vector_ct<$2u, $3u>($1);");

    const std::regex source_prefix_plain(
        R"(ctx\.apply_vfpu_source_prefix\(([A-Za-z0-9_]+), ([1-4])u, ([01])u\);)" );
    text = std::regex_replace(text, source_prefix_plain,
        "ctx.apply_vfpu_source_prefix_ct<$2u, $3u>($1);");

    const std::regex fpu_to_word(
        R"(ctx\.fpu_float_to_word\(([^,\n]+), ([0-3])u\))" );
    text = std::regex_replace(text, fpu_to_word,
        "ctx.fpu_float_to_word_ct<$2u>($1)");

    const std::regex vcmp(
        R"(ctx\.execute_vfpu_vcmp\(([0-9]+)u, ([0-9]+)u, ([1-4])u, ([0-9]+)u\);)" );
    text = std::regex_replace(text, vcmp,
        "ctx.execute_vfpu_vcmp_ct<$1u, $2u, $3u, $4u>();");

    const std::regex vcmov(
        R"(ctx\.execute_vfpu_vcmov\(([0-9]+)u, ([0-9]+)u, ([1-4])u, ([0-7])u, (true|false)\);)" );
    text = std::regex_replace(text, vcmov,
        "ctx.execute_vfpu_vcmov_ct<$1u, $2u, $3u, $4u, $5>();");

    const std::regex vdot(
        R"(ctx\.execute_vfpu_vdot\(([0-9]+)u, ([0-9]+)u, ([0-9]+)u, ([1-4])u\);)" );
    text = std::regex_replace(text, vdot,
        "ctx.execute_vfpu_vdot_ct<$1u, $2u, $3u, $4u>();");

    const std::regex vscl(
        R"(ctx\.execute_vfpu_vscl\(([0-9]+)u, ([0-9]+)u, ([0-9]+)u, ([1-4])u\);)" );
    text = std::regex_replace(text, vscl,
        "ctx.execute_vfpu_vscl_ct<$1u, $2u, $3u, $4u>();");

    const std::regex scalar_read(R"(ctx\.vfpu_scalar_bits\(([0-9]+)u\))");
    text = std::regex_replace(text, scalar_read,
        "ctx.vfpu_scalar_bits_ct<$1u>()");

    // set_vfpu_scalar_bits second arguments in generated code stay on one line.
    const std::regex scalar_write(
        R"(ctx\.set_vfpu_scalar_bits\(([0-9]+)u, ([^;\n]+)\);)" );
    text = std::regex_replace(text, scalar_write,
        "ctx.set_vfpu_scalar_bits_ct<$1u>($2);");
    return text;
}

namespace {

using K = psprecomp::OpcodeKind;

// Kinds translated inline. Their emission is checked against the reference
// interpreter by tests/cpu (random-state differential tests); everything else,
// including all VFPU arithmetic, calls the interpreter.
bool inline_kind(const psprecomp::DecodedInstruction &d) {
    switch (d.kind) {
    case K::Nop: case K::Sync: case K::Cache:
    case K::Addiu: case K::Slti: case K::Sltiu: case K::Andi: case K::Ori: case K::Xori: case K::Lui:
    case K::Add: case K::Addu: case K::Sub: case K::Subu: case K::And: case K::Or: case K::Xor: case K::Nor:
    case K::Slt: case K::Sltu: case K::Max: case K::Min: case K::Movz: case K::Movn:
    case K::Sll: case K::Srl: case K::Sra: case K::Rotr: case K::Sllv: case K::Srlv: case K::Srav: case K::Rotrv:
    case K::Clz: case K::Clo: case K::Ext: case K::Ins: case K::Seb: case K::Seh: case K::Bitrev:
    case K::Wsbh: case K::Wsbw:
    case K::Lw: case K::Lwl: case K::Lwr: case K::Sw: case K::Swl: case K::Swr:
    case K::Lh: case K::Lhu: case K::Sh: case K::Lb: case K::Lbu: case K::Sb: case K::Lwc1: case K::Swc1:
    case K::Mfhi: case K::Mflo: case K::Mthi: case K::Mtlo: case K::Mult: case K::Multu: case K::Div: case K::Divu:
    case K::Mfc1: case K::Mtc1:
    case K::AddS: case K::SubS: case K::MulS: case K::DivS: case K::SqrtS: case K::AbsS: case K::MovS: case K::NegS:
    case K::RoundWS: case K::TruncWS: case K::CeilWS: case K::FloorWS: case K::CvtWS: case K::CvtSW: case K::FpuCompare:
    case K::Lvs: case K::Svs: case K::Lvq: case K::Svq: case K::Vpfx:
        return true;
    case K::Mfv: case K::Mtv:
        // Control registers have write masks and fixed bits; the interpreter handles them.
        return (d.word & 0xFFu) < 128u;
    default:
        return false;
    }
}

} // namespace

namespace {

// Instructions emitted as a call to an inline template of cpu/generated_ops.h,
// by interpreter instruction name. The template takes the instruction word.
const char *generated_op_template(std::string_view name) {
    static const std::unordered_map<std::string_view, const char *> table{
        {"vadd", "vec3"}, {"vsub", "vec3"}, {"vdiv", "vec3"}, {"vmul", "vec3"},
        {"vmov", "vv2"}, {"vabs", "vv2"}, {"vneg", "vv2"}, {"vsat0", "vv2"}, {"vsat1", "vv2"},
        {"vrcp", "vv2"}, {"vrsq", "vv2"}, {"vsin", "vv2"}, {"vcos", "vv2"}, {"vexp2", "vv2"},
        {"vlog2", "vv2"}, {"vsqrt", "vv2"}, {"vasin", "vv2"}, {"vnrcp", "vv2"}, {"vnsin", "vv2"},
        {"vrexp2", "vv2"},
        {"vdot", "dot"}, {"vhdp", "dot"}, {"vfad", "dot"}, {"vavg", "dot"},
        {"vscl", "vscl"},
        {"vmin", "compare3"}, {"vmax", "compare3"}, {"vsge", "compare3"}, {"vslt", "compare3"},
        {"vscmp", "compare3"},
        {"vcmp", "vcmp"}, {"vcmov", "vcmov"},
        {"vcst", "vcst"}, {"vidt", "vidt"},
        {"vmidt", "matrix_init"}, {"vmzero", "matrix_init"}, {"vmone", "matrix_init"},
        {"vmmov", "vmmov"}, {"vmmul", "vmmul"}, {"vmscl", "vmscl"},
        {"v(h)tfm2", "vtfm"}, {"v(h)tfm3", "vtfm"}, {"v(h)tfm4", "vtfm"},
        {"vrot", "vrot"},
        {"viim.s", "viim"}, {"vfim.s", "viim"},
        {"vcrsp.t/vqmul.q", "cross_quat"}, {"vocp", "vocp"}, {"vsgn", "vsgn"},
    };
    const auto found = table.find(name);
    return found == table.end() ? nullptr : found->second;
}

// Interpreter-only instructions with an inline translation that is not an
// upstream decoder kind. Same contract as emit_regular.
std::optional<std::string> emit_generated_op(std::uint32_t word, std::uint32_t pc) {
    const auto info = anypsp::cpu::instruction_info(word);
    if (!info.valid || info.control_flow) return std::nullopt;
    const std::string_view name = info.name;
    const std::string w = psprecomp::hex32(word) + "u";
    const std::uint32_t rt = (word >> 16u) & 31u, rd = (word >> 11u) & 31u;
    if (const char *op = generated_op_template(name))
        return "    if (!anypsp::cpu::gen::" + std::string(op) + "<" + w + ">(rt, ctx, " + psprecomp::hex32(pc) +
               "u)) return; // " + std::string(name) + "\n";
    if (name == "vzero" || name == "vone")
        return "    if (!anypsp::cpu::gen::vector_init<" + w + ", " + (name == "vone" ? "true" : "false") +
               ">(rt, ctx, " + psprecomp::hex32(pc) + "u)) return; // " + std::string(name) + "\n";
    if (name == "madd" || name == "maddu" || name == "msub" || name == "msubu")
        return "    anypsp::cpu::gen::multiply_accumulate<" + w + ">(ctx); // " + std::string(name) + "\n";
    if (name == "vflush") {
        // Only 0xFFFF0000 and above keep the prefixes.
        if ((word & 0xFFFF0000u) == 0xFFFF0000u) return std::string("    // vflush\n");
        return std::string("    ctx.vfpu_ctrl[0] = 0xE4u; ctx.vfpu_ctrl[1] = 0xE4u; ctx.vfpu_ctrl[2] = 0u; // vflush\n");
    }
    if (name == "mfv") {  // to a control register (data registers are an upstream kind)
        const std::uint32_t imm = word & 0xFFu;
        if (rt == 0u) return std::string("    // mfv to $zero\n");
        if (imm >= 128u + 16u) return std::string("    // mfv past the control registers leaves rt alone\n");
        return "    ctx.gpr[" + std::to_string(rt) + "] = ctx.vfpu_ctrl[" + std::to_string(imm - 128u) + "]; // mfvc\n";
    }
    if (name == "vmfvc") {
        const std::uint32_t imm = (word >> 8u) & 0x7Fu;
        const std::string lane = std::to_string(psprecomp::AllegrexContext::vfpu_scalar_index(word & 0x7Fu));
        return "    ctx.vfpu[" + lane + "] = std::bit_cast<float>(" +
               (imm < 16u ? "ctx.vfpu_ctrl[" + std::to_string(imm) + "]" : std::string("0u")) + "); // vmfvc\n";
    }
    if (name == "mfic")
        return rt == 0u ? std::string("    // mfic to $zero\n")
                        : "    ctx.gpr[" + std::to_string(rt) + "] = rt.interrupts_enabled() ? 1u : 0u; // mfic\n";
    if (name == "mtic")
        return "    rt.set_interrupts_enabled((" + reg(rt) + " & 1u) != 0u); // mtic\n";
    if (name == "cfc1") {
        if (rt == 0u) return std::string("    // cfc1 to $zero\n");
        const std::string value = rd == 31u ? "ctx.fcr31" : rd == 0u ? "0x00003351u" : "0u";
        return "    ctx.gpr[" + std::to_string(rt) + "] = " + value + "; // cfc1\n";
    }
    if (name == "ctc1") {
        // The host FPU runs in the guest's rounding mode, so it switches here.
        if (rd != 31u) return std::string("    // ctc1 to a register other than FCR31 is ignored\n");
        return "    ctx.fcr31 = " + reg(rt) + " & 0x0181FFFFu; anypsp::cpu::enter_guest(ctx); // ctc1\n";
    }
    return std::nullopt;
}

} // namespace

bool translated_inline(const psprecomp::DecodedInstruction &d) {
    const auto info = anypsp::cpu::instruction_info(d.word);
    if (!info.valid || info.control_flow) return false;
    if (!inline_kind(d)) return emit_generated_op(d.word, 0u).has_value();
    // The generic decoder and the reference tables must agree on what the word
    // is: Allegrex decodes fewer bits than MIPS32 in several encodings.
    if (d.kind == K::FpuCompare) return std::string_view(info.name).starts_with("c.");
    if (d.kind == K::Nop) return d.word == 0u;
    return d.mnemonic == info.name;
}

std::string emit_instruction(const psprecomp::DecodedInstruction &d, std::uint32_t pc) {
    if (inline_kind(d) && translated_inline(d)) return emit_regular(d, pc);
    if (!inline_kind(d)) {
        if (auto code = emit_generated_op(d.word, pc)) return *code;
    }
    return "    if (!anypsp::cpu::execute(rt, ctx, " + psprecomp::hex32(pc) + "u, " + psprecomp::hex32(d.word) +
           "u)) return; // " + anypsp::cpu::instruction_info(d.word).name + "\n";
}

std::string lower_generated_source(std::string text) {
    return lower_constant_vfpu_accesses(lower_aot_memory_accesses(
        lower_constant_fpr_accesses(lower_constant_gpr_writes(std::move(text)))));
}

} // namespace anypsp::codegen
