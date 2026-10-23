// SPDX-License-Identifier: GPL-2.0-or-later
//
// Renderer-free native host: runs a game's recompiled code with the HLE kernel
// and the software GE, opens no window and plays no audio. Used for testing and
// for collecting analysis feedback (code reached without translation).

#include "kernel.hpp"

#include "elf_prx.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"

#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace psprecomp {
void register_dynamic_code();
}

namespace {

const char *const kUsage =
    "usage: anypsp-run-<game> <EBOOT.BIN|module.prx> [--disc dir] [--ms dir] [--frames n]\n"
    "                         [--feedback file] [--dump frame.ppm] [--guest-path path]\n"
    "                         [--output file]\n"
    "Runs headless: no window, no audio. --feedback appends the code addresses that had no\n"
    "translated code (input for anypsp-analyze --feedback). --output writes what the program\n"
    "sends to the emulator: device (test output; default: standard output).\n";

// Import stubs without a translated wrapper (always, in the interpreter-only
// runner) are bound here, by address.
std::unordered_map<std::uint32_t, psprecomp::PspImport> g_stub_imports;

void import_by_address(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    const auto found = g_stub_imports.find(ctx.pc);
    if (found == g_stub_imports.end()) {
        rt.stop("no import bound at " + psprecomp::hex32(ctx.pc));
        return;
    }
    const auto caller_context = psprecomp::capture_runtime_execution_context();
    const std::uint32_t import_pc = ctx.pc;
    const std::uint32_t return_address = ctx.gpr[31];
    rt.invoke_import(found->second.library, found->second.nid, ctx);
    if (!rt.stopped() && psprecomp::runtime_execution_context_matches(caller_context) && ctx.pc == import_pc)
        ctx.pc = return_address;
}

void write_ppm(const std::string &path, const std::vector<std::uint8_t> &rgba) {
    std::ofstream out(path, std::ios::binary);
    out << "P6\n480 272\n255\n";
    for (std::size_t i = 0; i < 480u * 272u; ++i) out.write(reinterpret_cast<const char *>(&rgba[i * 4u]), 3);
    if (!out) throw psprecomp::Error("Cannot write " + path);
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2 || std::string(argv[1]).starts_with("--")) {
        std::cerr << kUsage;
        return 2;
    }
    std::string disc = ".", memstick = ".", feedback, dump, output, guest_path = "ms0:/PSP/GAME/ANYPSP/EBOOT.PBP";
    long frames = 600;
    for (int i = 2; i < argc; i += 2) {
        const std::string flag = argv[i];
        if (i + 1 >= argc) {
            std::cerr << flag << " needs a value\n" << kUsage;
            return 2;
        }
        const std::string value = argv[i + 1];
        if (flag == "--disc") disc = value;
        else if (flag == "--ms") memstick = value;
        else if (flag == "--feedback") feedback = value;
        else if (flag == "--dump") dump = value;
        else if (flag == "--output") output = value;
        else if (flag == "--guest-path") guest_path = value;
        else if (flag == "--frames") {
            try {
                frames = std::stol(value);
            } catch (const std::exception &) {
                frames = -1;
            }
            if (frames <= 0) {
                std::cerr << "--frames needs a positive number\n";
                return 2;
            }
        } else {
            std::cerr << "unknown option " << flag << "\n" << kUsage;
            return 2;
        }
    }

    try {
        if (!feedback.empty()) psprecomp::set_runtime_feedback_file(feedback);
        auto runtime = std::make_unique<psprecomp::Runtime>();
        auto &rt = *runtime;
        const std::uint32_t base = psprecomp::kDefaultPspUserLoadBase;
        const auto module = anypsp::loader::load_module(
            rt.memory(), psprecomp::Elf32Image::from_bytes(anypsp::loader::read_executable(argv[1]), argv[1]), argv[1], base);
        if (!module.info) throw psprecomp::Error(std::string(argv[1]) + " has no module info");
        std::vector<psprecomp::PspImport> imports;
        for (const auto &import : module.imports)
            if (!import.variable) imports.push_back({import.library, import.nid, import.stub_address});

        psprecomp::register_generated_functions(rt);
        for (const auto &import : imports) {
            if (rt.has_function(import.stub_address)) continue;
            g_stub_imports[import.stub_address] = import;
            rt.register_function(import.stub_address, &import_by_address, import.library + "::" + psprecomp::hex32(import.nid));
        }
        psprecomp::register_dynamic_code();
        psprecomp::set_runtime_missing_code_handler(&psprecomp::run_dynamic_code);

        auto kernel = std::make_unique<pspweb::Kernel>(rt);
        std::ofstream output_file;
        if (!output.empty()) {
            output_file.open(output, std::ios::binary);
            if (!output_file) throw psprecomp::Error("Cannot write " + output);
        }
        kernel->set_test_output([&output_file, to_file = !output.empty()](std::string_view text) {
            std::ostream &out = to_file ? static_cast<std::ostream &>(output_file) : std::cout;
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
        });
        kernel->reserve(module.base, module.end - module.base, "module:" + module.info->name);
        kernel->set_roots(disc, memstick);
        kernel->set_flash_root(std::filesystem::path(disc) / "flash0");
        kernel->add_main_module(*module.info, module.base, module.end - module.base);
        kernel->install(imports);
        kernel->boot(module.entry, module.info->gp, guest_path);
        std::cout << "[anypsp] " << module.info->name << " at " << psprecomp::hex32(module.base) << "-"
                  << psprecomp::hex32(module.end) << ", " << imports.size() << " imports, " << module.relocations.applied
                  << " relocations, " << rt.function_count() << " translated entries\n";

        long frame = 0;
        bool running = true;
        while (running && frame < frames) {
            running = kernel->frame(50.0);
            ++frame;
        }
        std::cout << "[anypsp] ran " << frame << " frames; "
                  << (running ? "frame limit reached" : "guest stopped: " + kernel->halt_reason()) << "\n";
        if (!dump.empty()) {
            std::vector<std::uint8_t> rgba(480u * 272u * 4u);
            kernel->read_framebuffer(rgba.data());
            write_ppm(dump, rgba);
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "[anypsp] fatal: " << e.what() << "\n";
        return 1;
    }
}
