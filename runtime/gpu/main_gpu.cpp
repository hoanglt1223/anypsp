// SPDX-License-Identifier: GPL-2.0-or-later
//
// Headless runner with a renderer: runs a module like main_headless.cpp, draws
// the GE with the desktop OpenGL backend (or the software rasterizer) and
// writes frames as PNG files and per-frame hashes. Opens no visible window and
// plays no audio.

#include "frame_output.hpp"
#include "gl_host.hpp"

#include "kernel.hpp"

#include "elf_prx.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdio>
#include <filesystem>
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
    "usage: anypsp-<runner>-gpu <EBOOT.BIN|module.prx> [--renderer gl|soft] [--scale n] [--coherent 0|1]\n"
    "         [--frames n] [--frames-dir dir] [--png-every n] [--hashes file] [--screenshot file.png]\n"
    "         [--dump file.png] [--disc dir] [--ms dir] [--feedback file] [--guest-path path] [--output file]\n"
    "Runs headless (hidden GL context, no audio). --frames-dir writes frame_NNNNNN.png every --png-every\n"
    "frames (default 1); --hashes writes '<frame> <hash>' lines for every frame; --screenshot writes the\n"
    "picture at the program's last emulator: screenshot request; --dump writes the last frame.\n"
    "--coherent 1 (default) writes GPU pixels back to guest VRAM after every display list.\n";

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

struct Options {
    std::string module, disc = ".", memstick = ".", feedback, output, guest_path = "ms0:/PSP/GAME/ANYPSP/EBOOT.PBP";
    std::string renderer = "gl", frames_dir, hashes, screenshot, dump;
    long frames = 600, png_every = 1;
    int scale = 1;
    bool coherent = true;
};

long parse_positive(const std::string &flag, const std::string &value) {
    long n = -1;
    try {
        std::size_t used = 0;
        n = std::stol(value, &used);
        if (used != value.size()) n = -1;
    } catch (const std::exception &) {
        n = -1;
    }
    if (n <= 0) throw std::invalid_argument(flag + " needs a positive number");
    return n;
}

Options parse(int argc, char **argv) {
    if (argc < 2 || std::string(argv[1]).starts_with("--")) throw std::invalid_argument("missing module");
    Options o;
    o.module = argv[1];
    for (int i = 2; i < argc; i += 2) {
        const std::string flag = argv[i];
        if (i + 1 >= argc) throw std::invalid_argument(flag + " needs a value");
        const std::string value = argv[i + 1];
        if (flag == "--disc") o.disc = value;
        else if (flag == "--ms") o.memstick = value;
        else if (flag == "--feedback") o.feedback = value;
        else if (flag == "--output") o.output = value;
        else if (flag == "--guest-path") o.guest_path = value;
        else if (flag == "--frames-dir") o.frames_dir = value;
        else if (flag == "--hashes") o.hashes = value;
        else if (flag == "--screenshot") o.screenshot = value;
        else if (flag == "--dump") o.dump = value;
        else if (flag == "--frames") o.frames = parse_positive(flag, value);
        else if (flag == "--png-every") o.png_every = parse_positive(flag, value);
        else if (flag == "--scale") o.scale = static_cast<int>(std::min(parse_positive(flag, value), 8L));
        else if (flag == "--coherent") {
            if (value != "0" && value != "1") throw std::invalid_argument("--coherent takes 0 or 1");
            o.coherent = value == "1";
        } else if (flag == "--renderer") {
            if (value != "gl" && value != "soft") throw std::invalid_argument("--renderer takes gl or soft");
            o.renderer = value;
        } else {
            throw std::invalid_argument("unknown option " + flag);
        }
    }
    if (o.renderer == "soft" && o.scale != 1) throw std::invalid_argument("--scale needs --renderer gl");
    return o;
}

std::string frame_name(long frame) {
    char name[32];
    std::snprintf(name, sizeof name, "frame_%06ld.png", frame);
    return name;
}

} // namespace

int main(int argc, char **argv) {
    Options o;
    try {
        o = parse(argc, argv);
    } catch (const std::invalid_argument &e) {
        std::cerr << e.what() << "\n" << kUsage;
        return 2;
    }

    try {
        if (!o.feedback.empty()) psprecomp::set_runtime_feedback_file(o.feedback);
        auto runtime = std::make_unique<psprecomp::Runtime>();
        auto &rt = *runtime;
        const std::uint32_t base = psprecomp::kDefaultPspUserLoadBase;
        const auto module = anypsp::loader::load_module(
            rt.memory(), psprecomp::Elf32Image::from_bytes(anypsp::loader::read_executable(o.module), o.module), o.module, base);
        if (!module.info) throw psprecomp::Error(o.module + " has no module info");
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

        // Declared before the kernel so the renderer outlives the GE that points at it.
        std::unique_ptr<anypsp::gpu::GlHost> gl;
        auto kernel = std::make_unique<pspweb::Kernel>(rt);
        if (o.renderer == "gl") {
            gl = std::make_unique<anypsp::gpu::GlHost>(rt.memory());
            gl->start(o.scale);
            gl->renderer().set_coherent(o.coherent);
            kernel->ge().set_gl(&gl->renderer());
            std::cout << "[anypsp] GL renderer: " << gl->device() << ", scale " << gl->scale() << "\n";
        } else {
            std::cout << "[anypsp] software renderer\n";
        }
        const int width = 480 * (gl ? gl->scale() : 1), height = 272 * (gl ? gl->scale() : 1);
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
        auto capture = [&] {
            const auto &d = kernel->display();
            if (gl) gl->capture(d.framebuffer, d.stride, d.format, rgba);
            else kernel->read_framebuffer(rgba.data());
        };

        std::ofstream output_file;
        if (!o.output.empty()) {
            output_file.open(o.output, std::ios::binary);
            if (!output_file) throw psprecomp::Error("Cannot write " + o.output);
        }
        kernel->set_test_output([&output_file, to_file = !o.output.empty()](std::string_view text) {
            std::ostream &out = to_file ? static_cast<std::ostream &>(output_file) : std::cout;
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
        });
        long screenshots = 0;
        if (!o.screenshot.empty()) {
            kernel->set_screenshot_sink([&] {
                capture();
                anypsp::gpu::write_png(o.screenshot, rgba, width, height);
                ++screenshots;
            });
        }
        std::ofstream hashes;
        if (!o.hashes.empty()) {
            hashes.open(o.hashes, std::ios::binary);
            if (!hashes) throw psprecomp::Error("Cannot write " + o.hashes);
        }
        if (!o.frames_dir.empty()) std::filesystem::create_directories(o.frames_dir);

        kernel->reserve(module.base, module.end - module.base, "module:" + module.info->name);
        kernel->set_roots(o.disc, o.memstick);
        kernel->set_flash_root(std::filesystem::path(o.disc) / "flash0");
        kernel->add_main_module(*module.info, module.base, module.end - module.base);
        kernel->install(imports);
        kernel->boot(module.entry, module.info->gp, o.guest_path);
        std::cout << "[anypsp] " << module.info->name << " at " << psprecomp::hex32(module.base) << "-"
                  << psprecomp::hex32(module.end) << ", " << imports.size() << " imports, " << module.relocations.applied
                  << " relocations, " << rt.function_count() << " translated entries\n";

        const bool per_frame = !o.frames_dir.empty() || hashes.is_open();
        long frame = 0;
        bool running = true;
        while (running && frame < o.frames) {
            running = kernel->frame(50.0);
            ++frame;
            if (!per_frame) continue;
            capture();
            if (hashes.is_open()) {
                char line[48];
                std::snprintf(line, sizeof line, "%ld %016llx\n", frame,
                              static_cast<unsigned long long>(anypsp::gpu::frame_hash(rgba)));
                hashes << line;
            }
            if (!o.frames_dir.empty() && frame % o.png_every == 0)
                anypsp::gpu::write_png((std::filesystem::path(o.frames_dir) / frame_name(frame)).string(), rgba, width, height);
        }
        std::cout << "[anypsp] ran " << frame << " frames; "
                  << (running ? "frame limit reached" : "guest stopped: " + kernel->halt_reason()) << "\n";
        if (!o.screenshot.empty())
            std::cout << "[anypsp] " << screenshots << " screenshot request(s)" << (screenshots ? ", last in " + o.screenshot : "")
                      << "\n";
        if (!o.dump.empty()) {
            capture();
            anypsp::gpu::write_png(o.dump, rgba, width, height);
        }
        if (hashes.is_open() && !hashes.flush()) throw psprecomp::Error("Cannot write " + o.hashes);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "[anypsp] fatal: " << e.what() << "\n";
        return 1;
    }
}
