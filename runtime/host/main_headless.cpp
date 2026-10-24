// SPDX-License-Identifier: GPL-2.0-or-later
//
// Renderer-free native host: runs a game's recompiled code with the HLE kernel
// and the software GE, opens no window and plays no audio. Used for testing and
// for collecting analysis feedback (code reached without translation).

#include "kernel.hpp"

#include "elf_prx.hpp"
#include "nid_db.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"

#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <map>
#include <set>
#include <vector>

namespace psprecomp {
void register_dynamic_code();
}

namespace {

const char *const kUsage =
    "usage: anypsp-run-<game> <EBOOT.BIN|module.prx> [--disc dir] [--ms dir] [--frames n]\n"
    "                         [--feedback file] [--dump frame.ppm] [--guest-path path]\n"
    "                         [--output file] [--input frame:buttons,...] [--dump-at frame,...]\n"
    "                         [--hle-stubs file]\n"
    "Runs headless: no window, no audio. --feedback appends the code addresses that had no\n"
    "translated code (input for anypsp-analyze --feedback). --output writes what the program\n"
    "sends to the emulator: device (test output; default: standard output). --input holds\n"
    "the given sceCtrl button mask (hex) from that frame on; --dump-at writes frame<N>.ppm.\n"
    "Calling an imported function with no HLE implementation stops the run unless the\n"
    "--hle-stubs file lists it: one 'Library::name = value' (or Library::0xNID) per line.\n";

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

// "120:8,130:0" -> {{120, 0x8}, {130, 0}}.
std::map<long, std::uint32_t> parse_input_script(const std::string &text) {
    std::map<long, std::uint32_t> script;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = std::min(text.find(',', start), text.size());
        const std::string item = text.substr(start, end - start);
        const std::size_t colon = item.find(':');
        if (colon == std::string::npos) throw psprecomp::Error("--input item without frame:buttons: " + item);
        script[std::stol(item.substr(0, colon))] = static_cast<std::uint32_t>(std::stoul(item.substr(colon + 1), nullptr, 16));
        start = end + 1;
    }
    return script;
}

std::map<std::string, std::uint32_t> read_hle_stubs(const std::string &path) {
    std::map<std::string, std::uint32_t> stubs;
    std::ifstream in(path);
    if (!in) throw psprecomp::Error("Cannot open " + path);
    std::string line;
    for (int number = 1; std::getline(in, line); ++number) {
        const auto trim = [](std::string s) {
            const auto first = s.find_first_not_of(" \t\r");
            const auto last = s.find_last_not_of(" \t\r");
            return first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
        };
        line = trim(line.substr(0, line.find('#')));
        if (line.empty()) continue;
        const auto equals = line.find('=');
        const std::string key = trim(line.substr(0, equals));
        if (key.find("::") == std::string::npos)
            throw psprecomp::Error(path + ":" + std::to_string(number) + ": expected Library::function = value");
        try {
            stubs[key] = equals == std::string::npos ? 0u : static_cast<std::uint32_t>(std::stoul(trim(line.substr(equals + 1)), nullptr, 0));
        } catch (const std::exception &) {
            throw psprecomp::Error(path + ":" + std::to_string(number) + ": bad value");
        }
    }
    return stubs;
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
    std::string disc = ".", memstick = ".", feedback, dump, output, input, dump_at, hle_stubs, guest_path = "ms0:/PSP/GAME/ANYPSP/EBOOT.PBP";
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
        else if (flag == "--input") input = value;
        else if (flag == "--dump-at") dump_at = value;
        else if (flag == "--hle-stubs") hle_stubs = value;
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
        // Names for messages about functions the HLE does not implement.
        anypsp::loader::load_nid_toml(anypsp::loader::default_nid_database(), rt.nids());
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
        if (!hle_stubs.empty()) kernel->set_hle_stubs(read_hle_stubs(hle_stubs));
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
        if (const auto &missing = kernel->unimplemented_imports(); !missing.empty()) {
            std::map<std::string, std::vector<std::string>> by_library;
            for (const auto &name : missing) by_library[name.substr(0, name.find("::"))].push_back(name.substr(name.find("::") + 2));
            std::cout << "[anypsp] " << missing.size() << " of " << imports.size()
                      << " imports have no HLE implementation (calling one stops the run):\n";
            for (const auto &[library, names] : by_library) {
                std::cout << "[anypsp]   " << library << " (" << names.size() << "):";
                for (const auto &name : names) std::cout << " " << name;
                std::cout << "\n";
            }
        }
        kernel->boot(module.entry, module.info->gp, guest_path);
        std::cout << "[anypsp] " << module.info->name << " at " << psprecomp::hex32(module.base) << "-"
                  << psprecomp::hex32(module.end) << ", " << imports.size() << " imports, " << module.relocations.applied
                  << " relocations, " << rt.function_count() << " translated entries\n";

        long frame = 0;
        bool running = true;
        const auto input_script = parse_input_script(input);
        std::set<long> dump_frames;
        for (std::size_t start = 0; start < dump_at.size();) {
            const std::size_t end = std::min(dump_at.find(',', start), dump_at.size());
            dump_frames.insert(std::stol(dump_at.substr(start, end - start)));
            start = end + 1;
        }
        std::vector<std::uint8_t> frame_pixels(480u * 272u * 4u);
        while (running && frame < frames) {
            if (const auto press = input_script.find(frame); press != input_script.end()) kernel->set_pad(press->second);
            running = kernel->frame(50.0);
            ++frame;
            if (dump_frames.contains(frame)) {
                kernel->read_framebuffer(frame_pixels.data());
                write_ppm("frame" + std::to_string(frame) + ".ppm", frame_pixels);
            }
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
