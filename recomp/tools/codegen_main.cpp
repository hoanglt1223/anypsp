#include "elf_prx.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/decoder.hpp"
#include "codegen/emit_cpp.hpp"
#include "cpu/interp.h"
#include "psprecomp/codegen_policy.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/program_analysis.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <cmath>
#include <limits>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <map>
#include <optional>
#include <regex>
#include <string_view>
#include <set>
#include <sstream>
#include <vector>

namespace {
struct Function {
    std::string name;
    std::uint32_t address{};
    std::uint32_t size{};
};

std::uint32_t parse_hex(std::string text) {
    if (text.starts_with("0x") || text.starts_with("0X")) text.erase(0, 2);
    std::uint32_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (result.ec != std::errc{}) throw psprecomp::Error("Invalid hex value: " + text);
    return value;
}

std::vector<Function> load_functions(const std::filesystem::path &path) {
    std::ifstream in(path);
    if (!in) throw psprecomp::Error("Cannot open function CSV: " + path.string());
    std::vector<Function> out;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(in, line)) {
        ++line_number;
        if (line.empty() || line[0] == '#' || line.starts_with("name,")) continue;
        std::stringstream stream(line);
        std::string name, address, size;
        if (!std::getline(stream, name, ',') || !std::getline(stream, address, ',') || !std::getline(stream, size)) {
            throw psprecomp::Error("Invalid function CSV line " + std::to_string(line_number));
        }
        out.push_back({name, parse_hex(address), parse_hex(size)});
    }
    return out;
}

std::string safe_name(const std::string &name, std::uint32_t address) {
    std::string out;
    for (char c : name) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out.front()))) {
        out = "fn_" + psprecomp::hex32(address).substr(2) + "_" + out;
    }
    return out;
}

std::string cpp_escape(const std::string &text) {
    std::string out;
    for (char c : text) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else out += c;
    }
    return out;
}

using namespace anypsp::codegen;

std::string generated_unit_cpp_name(std::uint32_t unit) {
    std::ostringstream name;
    name << "recomp_unit_" << std::setfill('0') << std::setw(4) << unit;
    return name.str();
}

std::string generated_unit_cpp_entry_name(std::uint32_t unit) {
    return generated_unit_cpp_name(unit) + "_entry";
}

std::string direct_unit_chain_expression(
    std::uint32_t unit, std::uint32_t target,
    const std::map<std::uint32_t, std::uint16_t> *direct_entry_ids) {
    if (direct_entry_ids != nullptr) {
        const auto found = direct_entry_ids->find(target);
        if (found != direct_entry_ids->end() && found->second != 0u) {
            return "rt.invoke_chained_direct<&" + generated_unit_cpp_entry_name(unit) + ", " +
                std::to_string(unit) + "u, " + std::to_string(found->second) + "u, " +
                psprecomp::hex32(target) + "u>(ctx, &aot_mem)";
        }
        // Not a registered entry, so its bucket may not have been emitted at
        // all (e.g. a decoded JAL into data). Use the exact per-PC chain path.
        return "(ctx.pc = " + psprecomp::hex32(target) + "u, rt.invoke_chained_call(ctx, &aot_mem))";
    }
    return "rt.invoke_chained_direct<&" + generated_unit_cpp_name(unit) + ", " +
        std::to_string(unit) + "u>(ctx, &aot_mem)";
}

void emit_target(std::ostringstream &body, std::uint32_t target,
                 const std::set<std::uint32_t> &labels, const char *indent,
                 std::uint32_t executable_base = 0u,
                 std::uint32_t unit_span_bytes = 0u,
                 const std::map<std::uint32_t, std::uint16_t> *direct_entry_ids = nullptr,
                 const std::set<std::uint32_t> *import_stubs = nullptr,
                 std::uint32_t origin = 0u) {
    if (labels.contains(target)) {
        if (target <= origin)
            body << indent << "if (++local_transfers >= 2048u) { ctx.pc = " << psprecomp::hex32(target) << "u; return; }\n";
        body << indent << "goto L_" << psprecomp::hex32(target).substr(2) << ";\n";
        return;
    }

    // A fixed J/JAL to a PSP import must return to the outer dispatcher.
    // Trying the generated-unit chain first is guaranteed to fail because import
    // registration deliberately poisons/replaces that exact PC, and these stubs
    // are frequently hot in real titles. Emit the minimal correct handoff directly.
    if (import_stubs != nullptr && import_stubs->contains(target)) {
        body << indent << "ctx.pc = " << psprecomp::hex32(target) << "u; return;\n";
        return;
    }

    // A fixed-span AOT partition can split one guest CFG across neighbouring
    // C++ translation units. knows the destination bucket for every
    // fixed direct edge, so the common case bypasses Runtime's large per-PC
    // chain table and indexes the tiny unit table instead. A bucket containing
    // an import/HLE/host replacement is marked overridden at registration time
    // and invoke_chained_unit() falls back to the exact per-PC lookup there.
    if (unit_span_bytes != 0u && target >= executable_base) {
        const std::uint32_t unit = (target - executable_base) / unit_span_bytes;
        body << indent << "(void)" << direct_unit_chain_expression(unit, target, direct_entry_ids)
             << "; return;\n";
    } else {
        body << indent << "ctx.pc = " << psprecomp::hex32(target)
             << "u; (void)rt.invoke_chained_call(ctx, &aot_mem); return;\n";
    }
}


struct GeneratedFunctionInput {
    std::string name;
    std::uint32_t address{};
    std::set<std::uint32_t> instructions;
    std::set<std::uint32_t> entry_labels;
    std::uint32_t executable_base{};
    std::uint32_t unit_span_bytes{};
    const std::map<std::uint32_t, std::uint16_t> *direct_entry_ids{};
    const std::set<std::uint32_t> *import_stubs{};
};

std::string emit_function_source(const GeneratedFunctionInput &function,
                                 const psprecomp::GuestMemory &memory,
                                 const std::string &cpp_name) {
    std::ostringstream body;

    // Unit entry dispatch.  A `switch (ctx.pc)` over several hundred sparse
    // guest addresses compiles to a branch tree, and every outer dispatch pays
    // for it.  When the entries fit a bounded window we emit a dense
    // offset -> contiguous id table instead, so the switch covers 1..N with no
    // holes and lowers to a single jump table: two loads and one indirect jump.
    // The guarded offset/alignment test keeps the accepted PC set identical to
    // the exact comparisons it replaces.
    constexpr std::uint64_t kMaximumDenseSlots = 8192u;
    const bool dense_dispatch = !function.entry_labels.empty() &&
        (static_cast<std::uint64_t>(*function.entry_labels.rbegin() - *function.entry_labels.begin()) / 4u + 1u)
            <= kMaximumDenseSlots;
    const std::uint32_t dense_base = function.entry_labels.empty() ? 0u : *function.entry_labels.begin();
    const std::uint32_t dense_span = function.entry_labels.empty()
        ? 0u
        : (*function.entry_labels.rbegin() - dense_base) + 4u;
    const std::string table_name = "kEntryIds_" + cpp_name;

    if (dense_dispatch) {
        std::map<std::uint32_t, std::uint32_t> id_by_label;
        std::uint32_t next_id = 1u;
        for (const auto label : function.entry_labels) id_by_label[label] = next_id++;

        body << "static const std::uint16_t " << table_name << "[" << (dense_span / 4u) << "] = {";
        for (std::uint32_t offset = 0u; offset < dense_span; offset += 4u) {
            const auto found = id_by_label.find(dense_base + offset);
            if ((offset / 4u) % 32u == 0u) body << "\n   ";
            body << ' ' << (found != id_by_label.end() ? found->second : 0u) << ',';
        }
        body << "\n};\n";

        body << "void " << cpp_name << "_entry(Runtime &rt, AllegrexContext &ctx, std::uint16_t direct_entry_id, GuestMemory::AotFastView &aot_mem) {\n"
             << "    std::uint32_t jump_target = 0u;\n"
             << "    std::uint32_t local_transfers = 0u;\n"
             << "    std::uint32_t local_pc = ctx.pc;\n"
             << "    std::uint32_t entry_id = direct_entry_id;\n"
             << "LOCAL_DISPATCH:\n"
             << "    {\n"
             << "    if (entry_id == 0u) {\n"
             << "        const std::uint32_t entry_delta = local_pc - " << psprecomp::hex32(dense_base) << "u;\n"
             << "        entry_id = (entry_delta < " << dense_span
             << "u && (entry_delta & 3u) == 0u) ? " << table_name << "[entry_delta >> 2u] : 0u;\n"
             << "    }\n"
             << "    switch (entry_id) {\n";
        for (const auto label : function.entry_labels) {
            body << "    case " << id_by_label[label] << "u: goto L_"
                 << psprecomp::hex32(label).substr(2) << ";\n";
        }
        body << "    default:\n"
             // Entered at a PC inside the unit that was never translated: feedback
             // and the interpreter rather than a halt.
             << "        if (local_transfers == 0u) rt.run_missing_code(ctx);\n"
             // Local transfers advance local_pc without touching AllegrexContext.
             // Leaving the unit through this default arm hands control back to the
             // outer dispatcher, for which ctx.pc is the only statement of where to
             // resume -- so it must be materialized here.  Without this store the
             // dispatcher re-enters at the unit's original entry PC and executes
             // the wrong code.  local_transfers == 0u needs no store because no
             // local jump has happened yet and ctx.pc is still current.
             << "        else ctx.pc = local_pc;\n"
             << "        return;\n"
             << "    }\n"
             << "    }\n";
    } else {
        body << "void " << cpp_name << "_entry(Runtime &rt, AllegrexContext &ctx, std::uint16_t direct_entry_id, GuestMemory::AotFastView &aot_mem) {\n"
             << "    (void)direct_entry_id;\n"
             << "    std::uint32_t jump_target = 0u;\n"
             << "    std::uint32_t local_transfers = 0u;\n"
             // Plain JR shares the dense path's local-transfer code, which sets
             // local_pc and entry_id; the sparse switch only reads local_pc.
             << "    std::uint32_t local_pc = ctx.pc;\n"
             << "    [[maybe_unused]] std::uint32_t entry_id = 0u;\n"
             << "LOCAL_DISPATCH:\n"
             << "    switch (local_pc) {\n";
        for (const auto label : function.entry_labels) {
            body << "    case " << psprecomp::hex32(label) << "u: goto L_"
                 << psprecomp::hex32(label).substr(2) << ";\n";
        }
        body << "    default:\n"
             // Entered at a PC inside the unit that was never translated: feedback
             // and the interpreter rather than a halt.
             << "        if (local_transfers == 0u) rt.run_missing_code(ctx);\n"
             // Local transfers advance local_pc without touching AllegrexContext.
             // Leaving the unit through this default arm hands control back to the
             // outer dispatcher, for which ctx.pc is the only statement of where to
             // resume -- so it must be materialized here.  Without this store the
             // dispatcher re-enters at the unit's original entry PC and executes
             // the wrong code.  local_transfers == 0u needs no store because no
             // local jump has happened yet and ctx.pc is still current.
             << "        else ctx.pc = local_pc;\n"
             << "        return;\n"
             << "    }\n";
    }

    for (const auto block_start : function.entry_labels) {
        body << "L_" << psprecomp::hex32(block_start).substr(2) << ":\n";
        std::uint32_t pc = block_start;
        while (function.instructions.contains(pc)) {
            if (pc != block_start && function.entry_labels.contains(pc)) {
                body << "    goto L_" << psprecomp::hex32(pc).substr(2) << ";\n";
                break;
            }

            const auto decoded = psprecomp::decode_allegrex(memory.load32(pc));
            const auto reference = anypsp::cpu::instruction_info(decoded.word);
            // Control flow the generator does not model (eret, syscall outside an
            // import stub, a branch in a delay slot, an encoding the decoders read
            // differently) runs in the reference interpreter from this PC.
            const auto hand_to_interpreter = [&] {
                body << "    ctx.pc = " << psprecomp::hex32(pc) << "u; rt.run_missing_code(ctx); return; // "
                     << reference.name << "\n";
            };
            if (reference.control_flow != decoded.is_control_flow() ||
                reference.delay_slot != decoded.has_delay_slot()) {
                hand_to_interpreter();
                break;
            }
            if (decoded.has_delay_slot()) {
                const auto slot = psprecomp::decode_allegrex(memory.load32(pc + 4u));
                if (slot.is_control_flow() || anypsp::cpu::instruction_info(slot.word).control_flow) {
                    hand_to_interpreter();
                    break;
                }

                if (is_branch(decoded.kind)) {
                    const std::uint32_t target = pc + 4u + static_cast<std::uint32_t>(
                        static_cast<std::int32_t>(decoded.immediate) * 4);
                    const std::uint32_t fallthrough = pc + 8u;
                    const bool likely_branch = is_likely_branch(decoded.kind);
                    // The condition is read before the link write: bltzal $ra tests the old $ra.
                    body << "    { const bool branch_taken = " << branch_condition(decoded) << ";\n";
                    if (is_link_branch(decoded.kind)) {
                        body << "      ctx.set_gpr(31, " << psprecomp::hex32(pc + 8u) << "u);\n";
                    }
                    if (likely_branch) {
                        body << "      if (branch_taken) {\n"
                             << emit_instruction(slot, pc + 4u);
                        emit_target(body, target, function.entry_labels, "          ", function.executable_base, function.unit_span_bytes, function.direct_entry_ids, function.import_stubs, pc);
                        body << "      }\n";
                        emit_target(body, fallthrough, function.entry_labels, "      ", function.executable_base, function.unit_span_bytes, function.direct_entry_ids, function.import_stubs);
                        body << "    }\n";
                    } else {
                        body << emit_instruction(slot, pc + 4u)
                             << "      if (branch_taken) {\n";
                        emit_target(body, target, function.entry_labels, "          ", function.executable_base, function.unit_span_bytes, function.direct_entry_ids, function.import_stubs, pc);
                        body << "      }\n";
                        emit_target(body, fallthrough, function.entry_labels, "      ", function.executable_base, function.unit_span_bytes, function.direct_entry_ids, function.import_stubs);
                        body << "    }\n";
                    }
                } else if (decoded.kind == psprecomp::OpcodeKind::J ||
                           decoded.kind == psprecomp::OpcodeKind::Jal) {
                    const std::uint32_t target = ((pc + 4u) & 0xF0000000u) | (decoded.target << 2u);
                    if (decoded.kind == psprecomp::OpcodeKind::Jal) {
                        body << "    ctx.set_gpr(31, " << psprecomp::hex32(pc + 8u) << "u);\n";
                    }
                    body << emit_instruction(slot, pc + 4u);
                    if (decoded.kind == psprecomp::OpcodeKind::J) {
                        emit_target(body, target, function.entry_labels, "    ", function.executable_base, function.unit_span_bytes, function.direct_entry_ids, function.import_stubs, pc);
                    } else if (function.entry_labels.contains(target) &&
                               (function.import_stubs == nullptr || !function.import_stubs->contains(target))) {
                        // Import stubs are excluded: they may share the caller's unit
                        // in small modules, but must reach the HLE wrapper.
                        // Fixed same-unit JAL: the destination is already a C++
                        // label. Going through ctx.pc + LOCAL_DISPATCH needlessly
                        // re-decodes a dense entry id and burns the local-transfer
                        // counter. The delay slot and $ra write have already run.
                        body << "    goto L_" << psprecomp::hex32(target).substr(2) << ";\n";
                    } else {
                        // Cross-unit call. PSP import stubs are deliberate outer-
                        // dispatcher boundaries; do not pay a guaranteed-failing
                        // generated-unit chain attempt before reaching them.
                        const std::uint32_t return_pc = pc + 8u;
                        const bool target_is_import = function.import_stubs != nullptr &&
                            function.import_stubs->contains(target);
                        if (target_is_import) {
                            body << "    ctx.pc = " << psprecomp::hex32(target) << "u;\n"
                                 << "    return;\n";
                            break;
                        }
                        // Otherwise run the callee inline and resume locally only
                        // if it came back to our return address.
                        const bool direct_unit = function.unit_span_bytes != 0u &&
                            target >= function.executable_base;
                        const std::uint32_t target_unit = direct_unit
                            ? (target - function.executable_base) / function.unit_span_bytes : 0u;
                        if (!direct_unit)
                            body << "    ctx.pc = " << psprecomp::hex32(target) << "u;\n";
                        if (function.entry_labels.contains(return_pc)) {
                            body << "    if (";
                            if (direct_unit)
                                body << direct_unit_chain_expression(target_unit, target, function.direct_entry_ids);
                            else
                                body << "rt.invoke_chained_call(ctx, &aot_mem)";
                            body << " && ctx.pc == " << psprecomp::hex32(return_pc)
                                 << "u) goto L_" << psprecomp::hex32(return_pc).substr(2) << ";\n";
                        } else {
                            if (direct_unit)
                                body << "    (void)" << direct_unit_chain_expression(target_unit, target, function.direct_entry_ids) << ";\n";
                            else
                                body << "    (void)rt.invoke_chained_call(ctx, &aot_mem);\n";
                        }
                        body << "    return;\n";
                    }
                } else {
                    const std::uint32_t link = decoded.rd == 0u ? 31u : decoded.rd;
                    body << "    jump_target = " << reg(decoded.rs) << ";\n";
                    if (decoded.kind == psprecomp::OpcodeKind::Jalr) {
                        body << "    ctx.set_gpr(" << link << ", " << psprecomp::hex32(pc + 8u) << "u);\n";
                    }
                    body << emit_instruction(slot, pc + 4u);
                    if (decoded.kind == psprecomp::OpcodeKind::Jalr) {
                        // Indirect call: same bounded chaining as a direct one.
                        // The runtime rejects import wrappers, so a JALR into
                        // the kernel still leaves through the dispatcher.
                        const std::uint32_t return_pc = pc + 8u;
                        body << "    ctx.pc = jump_target;\n";
                        if (function.entry_labels.contains(return_pc)) {
                            body << "    if (rt.invoke_chained_call(ctx, &aot_mem) && ctx.pc == "
                                 << psprecomp::hex32(return_pc) << "u) goto L_"
                                 << psprecomp::hex32(return_pc).substr(2) << ";\n";
                        } else {
                            body << "    (void)rt.invoke_chained_call(ctx, &aot_mem);\n";
                        }
                        body << "    return;\n";
                    } else {
                        // Plain JR, including `jr $ra`.  A return that lands in
                        // this unit continues locally; otherwise the chained
                        // caller one frame up recognizes its return address.
                        body << "    local_pc = jump_target;\n"
                             << "    if (++local_transfers < 2048u) { entry_id = 0u; goto LOCAL_DISPATCH; }\n"
                             << "    ctx.pc = jump_target;\n"
                             << "    return;\n";
                    }
                }
                break;
            }

            if (decoded.is_control_flow()) {
                hand_to_interpreter();
                break;
            }
            body << emit_instruction(decoded, pc);
            const std::uint32_t next = pc + 4u;
            if (!function.instructions.contains(next)) {
                body << "    ctx.pc = " << psprecomp::hex32(next) << "u; return;\n";
                break;
            }
            if (function.entry_labels.contains(next)) {
                body << "    goto L_" << psprecomp::hex32(next).substr(2) << ";\n";
                break;
            }
            pc = next;
        }
    }
    body << "}\n\n";
    body << "void " << cpp_name << "(Runtime &rt, AllegrexContext &ctx) {\n"
         << "    auto aot_mem = rt.memory().aot_fast_view();\n"
         << "    " << cpp_name << "_entry(rt, ctx, 0u, aot_mem);\n}\n\n";
    return body.str();
}

// Options for code of a module that is loaded at run time next to the main
// executable (a PRX the game loads with sceKernelLoadModule).
struct AutoOptions {
    // Wraps everything in psprecomp::<name>, so several modules link together.
    std::string module_namespace;
    // Origin of the fixed-span unit numbering. Using the main executable's
    // origin keeps the runtime's dense unit table valid for both.
    std::uint32_t unit_origin{};
    // First import cache slot; each linked module needs a range of its own.
    std::uint32_t import_slot_base{};
    // Extra function seeds (name,address,source CSV, e.g. from anypsp-analyze).
    std::filesystem::path seeds_csv;
};

std::map<std::uint32_t, std::string> load_seed_csv(const std::filesystem::path &path) {
    std::ifstream in(path);
    if (!in) throw psprecomp::Error("Cannot open seed CSV: " + path.string());
    std::map<std::uint32_t, std::string> seeds;
    std::string line;
    for (std::size_t number = 1; std::getline(in, line); ++number) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.starts_with("name,") || line[0] == '#') continue;
        const auto first = line.find(',');
        const auto second = first == std::string::npos ? std::string::npos : line.find(',', first + 1u);
        if (second == std::string::npos) throw psprecomp::Error(path.string() + ":" + std::to_string(number) + ": expected name,address,source");
        const std::string address = line.substr(first + 1u, second - first - 1u);
        std::size_t used = 0;
        unsigned long value = 0;
        try { value = std::stoul(address, &used, 0); } catch (const std::exception &) { used = 0; }
        if (used == 0u || used != address.size() || value > 0xFFFFFFFFul)
            throw psprecomp::Error(path.string() + ":" + std::to_string(number) + ": bad address '" + address + "'");
        seeds.try_emplace(static_cast<std::uint32_t>(value), line.substr(second + 1u));
    }
    return seeds;
}

std::string open_namespace(const AutoOptions &options) {
    return options.module_namespace.empty() ? "namespace psprecomp {\n"
                                            : "namespace psprecomp {\nnamespace " + options.module_namespace + " {\n";
}

std::string close_namespace(const AutoOptions &options) {
    return options.module_namespace.empty() ? "} // namespace psprecomp\n"
                                            : "} // namespace " + options.module_namespace + "\n} // namespace psprecomp\n";
}

void write_import_wrappers(std::ostream &out, const std::vector<psprecomp::PspImport> &imports,
                           std::uint32_t slot_base = 0u) {
    for (std::size_t i = 0; i < imports.size(); ++i) {
        out << "static void import_" << i << "(Runtime &rt, AllegrexContext &ctx) {\n"
            << "    const RuntimeExecutionContextToken caller_context = capture_runtime_execution_context();\n"
            << "    const std::uint32_t import_pc = ctx.pc;\n"
            << "    const std::uint32_t return_address = ctx.gpr[31];\n"
            << "    rt.invoke_import_cached(" << (slot_base + i) << "u, \"" << cpp_escape(imports[i].library) << "\", "
            << psprecomp::hex32(imports[i].nid) << "u, ctx);\n"
            << "    if (!rt.stopped() && runtime_execution_context_matches(caller_context) &&\n"
            << "        ctx.pc == import_pc) ctx.pc = return_address;\n"
            << "}\n\n";
    }
}

int generate_manual(const std::filesystem::path &elf_path,
                    const std::filesystem::path &csv_path,
                    const std::filesystem::path &output_path) {
    const auto elf = psprecomp::Elf32Image::from_file(elf_path);
    auto functions = load_functions(csv_path);
    const std::uint32_t load_base = psprecomp::kDefaultPspUserLoadBase;
    if (elf.is_psp_prx()) {
        for (auto &function : functions) if (function.address < load_base) function.address += load_base;
    }

    psprecomp::GuestMemory memory;
    (void)elf.load_and_relocate(memory, load_base);
    std::vector<psprecomp::PspImport> imports;
    if (const auto module = elf.find_module_info(memory, load_base)) imports = elf.scan_imports(memory, *module);

    std::ofstream out(output_path);
    if (!out) throw psprecomp::Error("Cannot create generated source");
    out << "#include \"psprecomp/runtime.hpp\"\n#include \"cpu/interp.h\"\n#include <bit>\n#include <cmath>\n#include <cstdint>\n#include <limits>\n\nnamespace psprecomp {\n";

    std::vector<GeneratedFunctionInput> generated;
    for (const auto &function : functions) {
        GeneratedFunctionInput input{function.name, function.address, {}, {}};
        const std::uint32_t end = function.address + function.size;
        for (std::uint32_t pc = function.address; pc < end;) {
            input.instructions.insert(pc);
            input.entry_labels.insert(pc);
            const auto decoded = psprecomp::decode_allegrex(memory.load32(pc));
            pc += decoded.has_delay_slot() ? 8u : 4u;
        }
        out << emit_function_source(input, memory, safe_name(input.name, input.address));
        generated.push_back(std::move(input));
    }

    write_import_wrappers(out, imports);
    out << "void register_generated_functions(Runtime &runtime) {\n";
    for (const auto &function : generated) {
        const auto cpp_name = safe_name(function.name, function.address);
        for (const auto label : function.entry_labels) {
            out << "    runtime.register_function(" << psprecomp::hex32(label) << "u, &"
                << cpp_name << ", \"" << cpp_escape(function.name) << "\");\n";
        }
    }
    for (std::size_t i = 0; i < imports.size(); ++i) {
        out << "    runtime.register_function(" << psprecomp::hex32(imports[i].stub_address)
            << "u, &import_" << i << ", \"" << cpp_escape(imports[i].library)
            << "::" << psprecomp::hex32(imports[i].nid) << "\");\n";
    }
    out << "}\n} // namespace psprecomp\n";
    std::cout << "Generated " << functions.size() << " manual functions, " << imports.size()
              << " import wrappers into " << output_path.string() << "\n";
    return 0;
}


bool write_text_if_changed(const std::filesystem::path &path, const std::string &text) {
    if (std::ifstream existing(path, std::ios::binary); existing) {
        const std::string old((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
        if (old == text) return false;
    }
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) throw psprecomp::Error("Cannot create generated file: " + path.string());
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    std::filesystem::rename(temporary, path);
    return true;
}

int generate_auto(const std::filesystem::path &elf_path,
                  const std::filesystem::path &output_dir,
                  std::uint32_t load_base,
                  std::uint32_t unit_span_bytes,
                  const AutoOptions &options = {}) {
    psprecomp::GuestMemory memory;
    const auto module = anypsp::loader::load_module(
        memory, psprecomp::Elf32Image::from_bytes(anypsp::loader::read_executable(elf_path), elf_path.string()),
        elf_path.string(), load_base);
    const auto &elf = module.elf;
    std::vector<psprecomp::PspImport> imports;
    for (const auto &import : module.imports)
        if (!import.variable) imports.push_back({import.library, import.nid, import.stub_address});
    std::set<std::uint32_t> import_stubs;
    for (const auto &import : imports) import_stubs.insert(import.stub_address);
    const auto relocation_sites = anypsp::loader::analysis_sites(module.relocations);
    psprecomp::AnalysisOptions analysis;
    analysis.relocation_sites = &relocation_sites;
    if (!options.seeds_csv.empty()) analysis.extra_seeds = load_seed_csv(options.seeds_csv);
    const auto program = psprecomp::analyze_program(elf, memory, load_base, analysis);
    if (program.executable_ranges.empty()) throw psprecomp::Error("ELF has no executable ranges");

    std::filesystem::create_directories(output_dir);
    const std::uint32_t executable_base =
        options.unit_origin != 0u ? options.unit_origin : program.executable_ranges.front().start;
    if (executable_base > program.executable_ranges.front().start)
        throw psprecomp::Error("--unit-origin must not lie above the module's code");

    // Emit every discovered guest instruction once. Function seeds remain analysis
    // metadata and dispatcher entries, but overlapping CFGs no longer duplicate C++.
    std::set<std::uint32_t> global_entries = program.covered_entry_labels;
    for (const auto &[seed, source] : program.seeds) {
        (void)source;
        if (program.covered_labels.contains(seed)) global_entries.insert(seed);
    }
    for (const auto pc : program.covered_labels) {
        const std::uint32_t bucket_start = executable_base + ((pc - executable_base) / unit_span_bytes) * unit_span_bytes;
        if (pc == bucket_start || !program.covered_labels.contains(pc - 4u)) global_entries.insert(pc);
    }

    struct Unit {
        std::uint32_t bucket{};
        std::set<std::uint32_t> instructions;
        std::set<std::uint32_t> entries;
    };
    std::map<std::uint32_t, Unit> units_by_bucket;
    for (const auto pc : program.covered_labels) {
        const std::uint32_t bucket = (pc - executable_base) / unit_span_bytes;
        auto &unit = units_by_bucket[bucket];
        unit.bucket = bucket;
        unit.instructions.insert(pc);
    }
    for (const auto entry : global_entries) {
        const std::uint32_t bucket = (entry - executable_base) / unit_span_bytes;
        auto it = units_by_bucket.find(bucket);
        if (it != units_by_bucket.end() && it->second.instructions.contains(entry)) it->second.entries.insert(entry);
    }

    std::vector<Unit> units;
    units.reserve(units_by_bucket.size());
    for (auto &[bucket, unit] : units_by_bucket) {
        (void)bucket;
        if (!unit.entries.empty()) units.push_back(std::move(unit));
    }

    // assign the same compact entry ids used by each unit's dense
    // dispatcher globally before emission. Fixed cross-unit edges can pass the
    // destination id directly and skip the target unit's PC->entry table lookup.
    std::map<std::uint32_t, std::uint16_t> direct_entry_ids;
    for (const auto &unit : units) {
        // Only dense dispatchers understand entry ids; a sparse unit (entries
        // spread wider than the dense table) is entered by PC instead.
        const bool dense = (static_cast<std::uint64_t>(*unit.entries.rbegin() - *unit.entries.begin()) / 4u + 1u) <= 8192u;
        if (!dense) continue;
        std::uint16_t id = 1u;
        for (const auto label : unit.entries) direct_entry_ids[label] = id++;
    }

    // External declarations let fixed cross-unit edges become native direct
    // calls. Under MSVC /GL + /LTCG the linker can optimize across these TUs
    // instead of forcing every known edge through a function-pointer branch.
    const auto units_header_path = output_dir / "generated_units.hpp";
    std::ostringstream units_header;
    units_header << "#pragma once\n\n#include <cstdint>\n#include \"psprecomp/guest_memory.hpp\"\n\nnamespace psprecomp {\nclass Runtime;\nstruct AllegrexContext;\n";
    if (!options.module_namespace.empty()) units_header << "namespace " << options.module_namespace << " {\n";
    for (const auto &unit : units) {
        units_header << "void " << generated_unit_cpp_name(unit.bucket)
                     << "(Runtime &, AllegrexContext &);\n";
        units_header << "void " << generated_unit_cpp_entry_name(unit.bucket)
                     << "(Runtime &, AllegrexContext &, std::uint16_t, GuestMemory::AotFastView &);\n";
    }
    units_header << close_namespace(options);
    (void)write_text_if_changed(units_header_path, units_header.str());

    std::set<std::filesystem::path> expected_cpp;
    std::size_t rewritten_units = 0u;
    std::size_t registered_entries = 0u;
    for (const auto &unit : units) {
        std::ostringstream suffix;
        suffix << std::setfill('0') << std::setw(4) << unit.bucket;
        const std::string suffix_text = suffix.str();
        const auto path = output_dir / ("generated_unit_" + suffix_text + ".cpp");
        expected_cpp.insert(path.filename());

        const GeneratedFunctionInput generated_unit{
            "recomp_unit_" + suffix_text,
            executable_base + unit.bucket * unit_span_bytes,
            unit.instructions,
            unit.entries,
            executable_base,
            unit_span_bytes,
            &direct_entry_ids,
            &import_stubs,
        };

        std::ostringstream out;
        out << "#include \"psprecomp/runtime.hpp\"\n#include \"cpu/interp.h\"\n#include \"generated_units.hpp\"\n#include <bit>\n#include <cmath>\n#include <cstdint>\n#include <limits>\n\n"
            << open_namespace(options);
        // The register-cache lowering passes (per-basic-block GPR/FPR caches and
        // the cross-unit hot-register cache) are deliberately absent.  They kept
        // large numbers of guest registers live in C++ locals and in a second
        // long-lived object alongside AllegrexContext; inside the ~10,000-line
        // single functions emitted here, that drove MSVC's optimizer past the
        // point where it converges.  Affected units never finished compiling and
        // each grew past 2 GB, exhausting system memory during a normal build.
        // This pipeline matches the last configuration observed booting on
        // hardware.
        out << lower_generated_source(emit_function_source(generated_unit, memory, generated_unit.name));
        out << "void register_generated_unit_" << unit.bucket << "(Runtime &runtime) {\n";
        out << "    runtime.register_generated_unit(" << unit.bucket << "u, "
            << psprecomp::hex32(generated_unit.address) << "u, " << unit_span_bytes
            << "u, &" << generated_unit.name << ", &"
            << generated_unit_cpp_entry_name(unit.bucket) << ");\n";
        for (const auto label : unit.entries) {
            out << "    runtime.register_function(" << psprecomp::hex32(label) << "u, &"
                << generated_unit.name << ", \"" << generated_unit.name << "\");\n";
            ++registered_entries;
        }
        out << "}\n" << close_namespace(options);
        rewritten_units += write_text_if_changed(path, out.str()) ? 1u : 0u;
    }

    const auto registry_path = output_dir / "generated_registry.cpp";
    expected_cpp.insert(registry_path.filename());
    std::ostringstream registry;
    registry << "#include \"psprecomp/runtime.hpp\"\n#include \"cpu/interp.h\"\n#include <cstdint>\n\n" << open_namespace(options);
    for (const auto &unit : units) registry << "void register_generated_unit_" << unit.bucket << "(Runtime &runtime);\n";
    registry << "\n";
    write_import_wrappers(registry, imports, options.import_slot_base);
    registry << "void register_generated_functions(Runtime &runtime) {\n";
    for (const auto &unit : units) registry << "    register_generated_unit_" << unit.bucket << "(runtime);\n";
    for (std::size_t i = 0; i < imports.size(); ++i) {
        registry << "    runtime.register_function(" << psprecomp::hex32(imports[i].stub_address)
                 << "u, &import_" << i << ", \"" << cpp_escape(imports[i].library)
                 << "::" << psprecomp::hex32(imports[i].nid) << "\");\n";
    }
    registry << "}\n" << close_namespace(options);
    const bool registry_rewritten = write_text_if_changed(registry_path, registry.str());

    for (const auto &entry : std::filesystem::directory_iterator(output_dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".cpp" ||
            !entry.path().filename().string().starts_with("generated_")) continue;
        if (!expected_cpp.contains(entry.path().filename())) std::filesystem::remove(entry.path());
    }

    std::ostringstream report;
    report << "{\n"
           << "  \"mode\": \"automatic_global_cfg\",\n"
           << "  \"emission\": \"single_copy_basic_blocks\",\n"
           << "  \"partitioning\": \"fixed_executable_address_span\",\n"
           << "  \"discovered_function_seeds\": " << program.seeds.size() << ",\n"
           << "  \"unique_emitted_instructions\": " << program.covered_labels.size() << ",\n"
           << "  \"unique_registered_block_entries\": " << registered_entries << ",\n"
           << "  \"translation_units\": " << units.size() << ",\n"
           << "  \"rewritten_translation_units\": " << rewritten_units << ",\n"
           << "  \"registry_rewritten\": " << (registry_rewritten ? "true" : "false") << ",\n"
           << "  \"import_wrappers\": " << imports.size() << ",\n"
           << "  \"unit_span_bytes\": " << unit_span_bytes << "\n"
           << "}\n";
    write_text_if_changed(output_dir / "auto_codegen_report.json", report.str());

    std::cout << "Automatic global codegen completed\n"
              << "  function seeds:    " << program.seeds.size() << "\n"
              << "  emitted code PCs:  " << program.covered_labels.size() << "\n"
              << "  registered blocks: " << registered_entries << "\n"
              << "  source units:      " << units.size() << "\n"
              << "  rewritten units:   " << rewritten_units << "\n"
              << "  import wrappers:   " << imports.size() << "\n"
              << "  output:            " << output_dir.string() << "\n";
    return 0;
}

} // namespace

int generate_dynamic(const std::filesystem::path &listing, const std::filesystem::path &output) {
    std::ifstream in(listing);
    if (!in) throw psprecomp::Error("cannot read " + listing.string());
    psprecomp::Runtime runtime;
    std::ostringstream functions, registry;
    std::set<std::pair<std::uint32_t, std::uint64_t>> seen;
    std::string line;
    std::size_t count = 0u;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string token;
        if (!(fields >> token)) continue;
        const std::uint32_t address = parse_hex(token);
        std::vector<std::uint32_t> words;
        while (fields >> token) words.push_back(parse_hex(token));
        if (words.empty()) continue;
        const std::uint64_t hash = psprecomp::dynamic_code_hash(words.data(), words.size());
        if (!seen.insert({address, hash}).second) continue;
        GeneratedFunctionInput function;
        std::ostringstream name;
        name << "dynamic_" << std::hex << std::uppercase << address << "_" << hash;
        function.name = name.str();
        function.address = address;
        for (std::size_t i = 0; i < words.size(); ++i) {
            const std::uint32_t pc = address + static_cast<std::uint32_t>(i * 4u);
            runtime.memory().store32(pc, words[i]);
            function.instructions.insert(pc);
            function.entry_labels.insert(pc);
        }
        functions << lower_generated_source(emit_function_source(function, runtime.memory(), function.name));
        registry << "    register_dynamic_function(" << psprecomp::hex32(address) << "u, 0x" << std::hex << hash
                 << std::dec << "ull, &" << function.name << ");\n";
        ++count;
    }
    std::ostringstream out;
    out << "#include \"psprecomp/interpreter.hpp\"\n#include \"psprecomp/runtime.hpp\"\n#include \"cpu/interp.h\"\n#include <bit>\n#include <cmath>\n"
        << "#include <cstdint>\n#include <limits>\n\nnamespace psprecomp {\n\n" << functions.str()
        << "void register_dynamic_code() {\n" << registry.str() << "}\n\n} // namespace psprecomp\n";
    write_text_if_changed(output, out.str());
    std::cout << count << " dynamic code blocks written to " << output.string() << "\n";
    return 0;
}

int main(int argc, char **argv) {
    try {
        if (argc == 4 && std::string_view(argv[1]) == "--dynamic") return generate_dynamic(argv[2], argv[3]);
        if (argc >= 4 && std::string_view(argv[2]) == "--auto") {
            // Positional load base and unit span, then optional module flags.
            std::vector<std::string_view> positional;
            AutoOptions options;
            for (int i = 4; i < argc; ++i) {
                const std::string_view arg = argv[i];
                const bool has_value = i + 1 < argc;
                if (arg == "--namespace" && has_value) options.module_namespace = argv[++i];
                else if (arg == "--unit-origin" && has_value) options.unit_origin = static_cast<std::uint32_t>(std::stoul(argv[++i], nullptr, 0));
                else if (arg == "--import-slot-base" && has_value) options.import_slot_base = static_cast<std::uint32_t>(std::stoul(argv[++i], nullptr, 0));
                else if (arg == "--seeds" && has_value) options.seeds_csv = argv[++i];
                else if (!arg.starts_with("--") && positional.size() < 2u) positional.push_back(arg);
                else {
                    std::cerr << "Usage: psp_recomp <ELF> --auto <generated_dir> [load_base_hex] [unit_span_bytes]\n"
                                 "         [--namespace name] [--unit-origin address] [--import-slot-base n] [--seeds seeds.csv]\n";
                    return 2;
                }
            }
            const std::uint32_t load_base = !positional.empty()
                ? static_cast<std::uint32_t>(std::stoul(std::string(positional[0]), nullptr, 0))
                : psprecomp::kDefaultPspUserLoadBase;
            const std::uint32_t unit_span = positional.size() >= 2u
                ? static_cast<std::uint32_t>(std::stoul(std::string(positional[1]), nullptr, 0))
                : 0x20000u;
            if (unit_span == 0u || (unit_span & 3u) != 0u) throw psprecomp::Error("unit_span_bytes must be non-zero and 4-byte aligned");
            return generate_auto(argv[1], argv[3], load_base, unit_span, options);
        }
        if (argc == 4) return generate_manual(argv[1], argv[2], argv[3]);
        std::cerr << "Usage:\n"
                  << "  psp_recomp <ELF> <functions.csv> <generated_manifest.cpp>\n"
                  << "  psp_recomp <ELF> --auto <generated_dir> [load_base_hex] [unit_span_bytes]\n"
                  << "         [--namespace name] [--unit-origin address] [--import-slot-base n] [--seeds seeds.csv]\n";
        return 2;
    } catch (const std::exception &e) {
        std::cerr << "psp_recomp error: " << e.what() << "\n";
        return 1;
    }
}
