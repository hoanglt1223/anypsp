// SPDX-License-Identifier: GPL-2.0-or-later
//
// Headless runner with a renderer: runs a module like main_headless.cpp, draws
// the GE with the desktop OpenGL backend (or the software rasterizer) and
// writes frames as PNG files and per-frame hashes. Opens no visible window and
// plays no audio.

#include "desktop.hpp"
#include "display.hpp"
#include "display_loop.hpp"
#include "patches.hpp"
#include "disc.hpp"
#include "frame_output.hpp"
#include "gl_host.hpp"
#include "input_map.hpp"

#include "kernel.hpp"

#include "elf_prx.hpp"
#include "nid_db.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_stdinc.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace psprecomp {
void register_dynamic_code();
}

namespace {

#ifdef ANYPSP_PLAY
constexpr bool kWindowByDefault = true;
#else
constexpr bool kWindowByDefault = false;
#endif

const char *const kUsage =
    "usage: anypsp-<runner>-gpu <game.iso|disc folder|EBOOT.BIN|module.prx> [--window|--headless] [--fullscreen]\n"
    "         [--mute] [--wav file.wav] [--keymap file.ini] [--write-input file.ini] [--cache dir]\n"
    "         [--renderer gl|soft] [--scale n] [--coherent lazy|1|0]\n"
    "         [--frames n] [--frames-dir dir] [--png-every n] [--hashes file] [--screenshot file.png]\n"
    "         [--dump file.png] [--disc dir] [--ms dir] [--feedback file] [--guest-path path] [--output file]\n"
    "         [--input frame:buttons,...] [--hle-stubs file] [--deterministic] [--smoothing on|off]\n"
    "         [--interpolate on|off]\n"
    "         [--refresh auto|hz] [--vrr on|off] [--sync-to-monitor on|off] [--pause-fps 30|60] [--fps-cap off|n]\n"
    "         [--patches auto|file.ini]\n"
    "An ISO image or extracted disc folder boots PSP_GAME/SYSDIR/BOOT.BIN or EBOOT.BIN (unencrypted only);\n"
    "ISO images are extracted once into --cache (default: the per-user data folder).\n"
    "--window plays in a resizable window at 59.94 fps with sound, keyboard and gamepad input (anypsp-play's\n"
    "default; F11 toggles fullscreen), runs until the window closes unless --frames is given, and keeps saves\n"
    "in the per-user data folder's memstick (override with --ms). --keymap names the input file; otherwise\n"
    "anypsp-input.ini next to the program or in the per-user data folder is used. --write-input writes the\n"
    "current bindings to a file and exits. --scale defaults to the input file's Scale (2) in a window.\n"
    "Headless (the default except for anypsp-play): hidden GL context, no sound device, scripted input.\n"
    "--wav records the audio output. --frames-dir writes frame_NNNNNN.png every --png-every\n"
    "frames (default 1); --hashes writes '<frame> <hash>' lines for every frame; --screenshot writes the\n"
    "picture at the program's last emulator: screenshot request; --dump writes the last frame.\n"
    "--coherent lazy (default) writes GPU pixels back to guest VRAM only when the CPU touches them and\n"
    "reloads VRAM the CPU wrote into the GPU copy; 1 writes back after every display list (slow, for\n"
    "debugging); 0 never writes back.\n"
    "--deterministic drives guest time from executed code instead of the host clock, so scripted runs repeat.\n"
"Display settings (a window only; docs/user/desktop.md): --smoothing on blends the in-between vblank of games\n"
"drawing at 30 fps (F9); --refresh sets the monitor rate pacing assumes (F5; auto detects it); --vrr presents\n"
"each vblank when ready (F6); --sync-to-monitor runs vblanks at the monitor rate within 1% (F7); --fps-cap\n"
"limits pictures shown (F8); --pause-fps limits them while paused (F4; Pause key pauses). They are kept in\n"
"anypsp-display.ini next to the program or in the per-user data folder.\n"
"--patches auto applies patches/<disc ID>.ini (next to the program or in the per-user data folder), for\n"
"example the 60 fps patch for UNH 1; --patches file.ini names a patch file.\n"
    "--input holds the given sceCtrl button mask (hex) from that frame on. --hle-stubs lists imports\n"
    "without HLE that return a fixed value: one 'Library::name = value' (or Library::0xNID) per line.\n";

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

// Same formats as main_headless.cpp.
std::map<long, std::uint32_t> parse_input_script(const std::string &text) {
    std::map<long, std::uint32_t> script;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = std::min(text.find(',', start), text.size());
        const std::string item = text.substr(start, end - start);
        const std::size_t colon = item.find(':');
        if (colon == std::string::npos) throw std::invalid_argument("--input item without frame:buttons: " + item);
        try {
            script[std::stol(item.substr(0, colon))] = static_cast<std::uint32_t>(std::stoul(item.substr(colon + 1), nullptr, 16));
        } catch (const std::logic_error &) {
            throw std::invalid_argument("bad --input item: " + item);
        }
        start = end + 1;
    }
    return script;
}

std::map<std::string, std::uint32_t> read_hle_stubs(const std::string &path) {
    std::map<std::string, std::uint32_t> stubs;
    std::ifstream in(path);
    if (!in) throw psprecomp::Error("Cannot open " + path);
    const auto trim = [](const std::string &s) {
        const auto first = s.find_first_not_of(" \t\r");
        const auto last = s.find_last_not_of(" \t\r");
        return first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
    };
    std::string line;
    for (int number = 1; std::getline(in, line); ++number) {
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

struct Options {
    std::string module, disc = ".", memstick = ".", feedback, output, guest_path = "ms0:/PSP/GAME/ANYPSP/EBOOT.PBP";
    std::string renderer = "gl", frames_dir, hashes, screenshot, dump, hle_stubs;
    std::string wav, keymap, write_input, cache, patches;
    std::map<long, std::uint32_t> input;
    std::vector<std::pair<std::string, std::string>> display; // --smoothing ...: display setting overrides
    long frames = 600, png_every = 1;
    int scale = 1;
    pspweb::GeGl::Coherence coherence = pspweb::GeGl::Coherence::Lazy;
    bool window = kWindowByDefault, fullscreen = false, mute = false, deterministic = false;
    bool frames_set = false, scale_set = false, disc_set = false, ms_set = false, guest_set = false;
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
    Options o;
    int i = 1;
    if (argc >= 2 && !std::string(argv[1]).starts_with("--")) o.module = argv[i++];
    for (; i < argc; i += 2) {
        const std::string flag = argv[i];
        if (flag == "--window" || flag == "--headless") {
            o.window = flag == "--window";
            --i;
            continue;
        }
        if (flag == "--fullscreen" || flag == "--mute" || flag == "--deterministic") {
            (flag == "--mute" ? o.mute : flag == "--deterministic" ? o.deterministic : o.fullscreen) = true;
            --i;
            continue;
        }
        if (i + 1 >= argc) throw std::invalid_argument(flag + " needs a value");
        const std::string value = argv[i + 1];
        if (flag == "--disc") o.disc = value, o.disc_set = true;
        else if (flag == "--ms") o.memstick = value, o.ms_set = true;
        else if (flag == "--feedback") o.feedback = value;
        else if (flag == "--output") o.output = value;
        else if (flag == "--guest-path") o.guest_path = value, o.guest_set = true;
        else if (flag == "--wav") o.wav = value;
        else if (flag == "--keymap") o.keymap = value;
        else if (flag == "--write-input") o.write_input = value;
        else if (flag == "--cache") o.cache = value;
        else if (flag == "--frames-dir") o.frames_dir = value;
        else if (flag == "--hashes") o.hashes = value;
        else if (flag == "--screenshot") o.screenshot = value;
        else if (flag == "--dump") o.dump = value;
        else if (flag == "--hle-stubs") o.hle_stubs = value;
        else if (flag == "--input") o.input = parse_input_script(value);
        else if (flag == "--smoothing") o.display.emplace_back("Smoothing", value);
        else if (flag == "--interpolate") o.display.emplace_back("Interpolate", value);
        else if (flag == "--patches") o.patches = value;
        else if (flag == "--refresh") o.display.emplace_back("Refresh", value);
        else if (flag == "--vrr") o.display.emplace_back("VRR", value);
        else if (flag == "--sync-to-monitor") o.display.emplace_back("SyncToMonitor", value);
        else if (flag == "--pause-fps") o.display.emplace_back("PauseFps", value);
        else if (flag == "--fps-cap") o.display.emplace_back("FpsCap", value);
        else if (flag == "--frames") o.frames = parse_positive(flag, value), o.frames_set = true;
        else if (flag == "--png-every") o.png_every = parse_positive(flag, value);
        else if (flag == "--scale") o.scale = static_cast<int>(std::min(parse_positive(flag, value), 8L)), o.scale_set = true;
        else if (flag == "--coherent") {
            if (value == "lazy") o.coherence = pspweb::GeGl::Coherence::Lazy;
            else if (value == "1") o.coherence = pspweb::GeGl::Coherence::Full;
            else if (value == "0") o.coherence = pspweb::GeGl::Coherence::Off;
            else throw std::invalid_argument("--coherent takes lazy, 1 or 0");
        } else if (flag == "--renderer") {
            if (value != "gl" && value != "soft") throw std::invalid_argument("--renderer takes gl or soft");
            o.renderer = value;
        } else {
            throw std::invalid_argument("unknown option " + flag);
        }
    }
    if (o.module.empty() && o.write_input.empty()) throw std::invalid_argument("missing game or module");
    if (o.renderer == "soft" && o.scale != 1) throw std::invalid_argument("--scale needs --renderer gl");
    if (o.renderer == "soft" && o.window) throw std::invalid_argument("--window needs --renderer gl");
    return o;
}

std::function<void(const std::int16_t *, std::uint32_t)> g_audio;

// Per-user data folder (%APPDATA%\anyport\anypsp\ and the like): saves, extracted discs, input file.
std::filesystem::path user_data_dir() {
    char *path = SDL_GetPrefPath("anyport", "anypsp");
    if (path == nullptr) throw psprecomp::Error(std::string("No per-user data folder: ") + SDL_GetError());
    std::filesystem::path result = std::filesystem::path(reinterpret_cast<const char8_t *>(path));
    SDL_free(path);
    return result;
}

std::filesystem::path program_dir() {
    const char *base = SDL_GetBasePath(); // owned by SDL
    return base != nullptr ? std::filesystem::path(reinterpret_cast<const char8_t *>(base)) : std::filesystem::path();
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
        const std::filesystem::path user_dir = user_data_dir();
        anypsp::input::InputMap input_map;
        const std::filesystem::path keymap =
            !o.keymap.empty() ? std::filesystem::path(o.keymap) : anypsp::input::find_input_file(program_dir(), user_dir);
        if (!keymap.empty()) input_map.load(keymap);
        if (!o.write_input.empty()) {
            input_map.save(o.write_input);
            std::cout << "[anypsp] wrote " << o.write_input << "\n";
            return 0;
        }
        if (anypsp::disc::is_disc(o.module)) {
            const auto target = anypsp::disc::resolve(o.module, o.cache.empty() ? user_dir / "discs" : std::filesystem::path(o.cache));
            o.module = target.module.string();
            if (!o.disc_set) o.disc = target.disc_root.string();
            if (!o.guest_set) o.guest_path = target.guest_path;
            std::cout << "[anypsp] booting " << o.guest_path << " from " << o.disc << "\n";
        }
        if (o.window) {
            if (!o.scale_set) o.scale = input_map.scale();
            if (!o.ms_set) o.memstick = (user_dir / "memstick").string();
            std::cout << "[anypsp] memory stick (saves): " << o.memstick << "\n";
        }
        std::filesystem::create_directories(o.memstick);

        if (!o.feedback.empty()) psprecomp::set_runtime_feedback_file(o.feedback);
        auto runtime = std::make_unique<psprecomp::Runtime>();
        auto &rt = *runtime;
        // Names for messages about functions the HLE does not implement.
        anypsp::loader::load_nid_toml(anypsp::loader::default_nid_database(), rt.nids());
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
        if (o.deterministic) kernel->set_deterministic(true);
        if (!o.hle_stubs.empty()) kernel->set_hle_stubs(read_hle_stubs(o.hle_stubs));
        if (o.renderer == "gl") {
            gl = std::make_unique<anypsp::gpu::GlHost>(rt.memory());
            gl->start(o.scale, o.window ? "AnyPSP - " + module.info->name : std::string());
            gl->renderer().set_coherence(o.coherence);
            if (o.coherence == pspweb::GeGl::Coherence::Lazy) kernel->track_gpu_vram();
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
        std::vector<anypsp::patches::Patch> game_patches;
        if (!o.patches.empty()) {
            std::filesystem::path file = o.patches;
            if (o.patches == "auto") {
                const std::string id = anypsp::patches::disc_id(o.disc);
                file = anypsp::patches::find_file(id, program_dir(), user_dir);
                if (file.empty()) std::cout << "[anypsp] no patch file for disc " << (id.empty() ? "(unknown)" : id) << "\n";
            }
            if (!file.empty()) {
                game_patches = anypsp::patches::load(file);
                std::cout << "[anypsp] " << game_patches.size() << " patch(es) from " << file.string() << "\n";
            }
        }
        anypsp::patches::apply(game_patches, anypsp::patches::When::Load, rt.memory());
        std::uint64_t patch_writes = 0;
        kernel->boot(module.entry, module.info->gp, o.guest_path);
        std::cout << "[anypsp] " << module.info->name << " at " << psprecomp::hex32(module.base) << "-"
                  << psprecomp::hex32(module.end) << ", " << imports.size() << " imports, " << module.relocations.applied
                  << " relocations, " << rt.function_count() << " translated entries\n";

        anypsp::display::Settings display;
        const std::filesystem::path display_file = anypsp::display::find_file(program_dir(), user_dir);
        if (o.window) display = anypsp::display::load(display_file);
        for (const auto &[name, value] : o.display) anypsp::display::apply_setting(display, name, value, "--");
        if (display.interpolate) {
            if (!gl) throw std::invalid_argument("--interpolate needs --renderer gl");
            kernel->ge().set_interpolation(&gl->enable_in_between());
            std::cout << "[anypsp] frame interpolation on\n";
        }
        anypsp::display::FrameTracker frame_tracker; // headless captures with --interpolate
        std::vector<std::uint8_t> real_rgba;
        std::uint64_t in_between_shown = 0;
        std::optional<anypsp::desktop::Controls> controls;
        anypsp::desktop::AudioOut audio;
        bool audio_open = false;
        std::unique_ptr<anypsp::desktop::WavWriter> wav;
        if (!o.wav.empty()) wav = std::make_unique<anypsp::desktop::WavWriter>(o.wav);
        if (o.window) {
            controls.emplace(input_map);
            if (o.fullscreen) gl->set_fullscreen(true);
            if (!o.mute) audio_open = audio.open();
        }
        g_audio = [&](const std::int16_t *samples, std::uint32_t count) {
            if (wav) wav->write(samples, count);
            if (audio_open) audio.push(samples, count);
        };
        kernel->set_audio_sink([](const std::int16_t *samples, std::uint32_t count) { g_audio(samples, count); });
        std::optional<anypsp::desktop::DisplayLoop> window_loop;
        if (controls)
            window_loop.emplace(*gl, audio_open ? &audio : nullptr, display, user_dir / anypsp::display::kFileName,
                                module.info->name);

        const bool per_frame = !o.frames_dir.empty() || hashes.is_open();
        const long frame_limit = o.window && !o.frames_set ? std::numeric_limits<long>::max() : o.frames;
        long frame = 0;
        bool running = true, closed = false;
        const auto started = std::chrono::steady_clock::now();
        while (running && frame < frame_limit) {
            if (controls) {
                if (!controls->poll(*gl)) {
                    closed = true;
                    break;
                }
                window_loop->hotkeys(controls->take_hotkeys());
                if (window_loop->paused()) {
                    window_loop->show_paused();
                    continue;
                }
                if (o.input.empty()) {
                    const auto pad = controls->pad();
                    kernel->set_pad(pad.buttons, pad.lx, pad.ly);
                }
            }
            if (const auto press = o.input.find(frame); press != o.input.end()) kernel->set_pad(press->second);
            running = kernel->frame(50.0);
            ++frame;
            if (!game_patches.empty()) patch_writes += anypsp::patches::apply(game_patches, anypsp::patches::When::Vblank, rt.memory());
            if (window_loop) window_loop->after_vblank(kernel->display());
            if (!per_frame) continue;
            capture();
            if (display.interpolate && !window_loop) {
                // What the window would show on this vblank.
                const auto &d = kernel->display();
                const unsigned previous = frame_tracker.after_vblank(d.buffer_changes);
                if (previous == 2u) {
                    real_rgba.swap(rgba);
                    gl->capture_in_between(d.framebuffer, d.stride, d.format, rgba);
                    if (anypsp::display::in_between_on_new_frame(true, previous, anypsp::display::mean_difference(rgba, real_rgba)))
                        ++in_between_shown;
                    else
                        rgba.swap(real_rgba);
                }
            }
            if (hashes.is_open()) {
                char line[48];
                std::snprintf(line, sizeof line, "%ld %016llx\n", frame,
                              static_cast<unsigned long long>(anypsp::gpu::frame_hash(rgba)));
                hashes << line;
            }
            if (!o.frames_dir.empty() && frame % o.png_every == 0)
                anypsp::gpu::write_png((std::filesystem::path(o.frames_dir) / frame_name(frame)).string(), rgba, width, height);
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "[anypsp] ran " << frame << " frames in " << static_cast<long>(seconds) << " s ("
                  << static_cast<long>(seconds > 0.0 ? frame / seconds : 0.0) << " fps, " << kernel->display().buffer_changes
                  << " game frames); "
                  << (closed ? "window closed" : running ? "frame limit reached" : "guest stopped: " + kernel->halt_reason()) << "\n";
        g_audio = nullptr;
        if (wav) {
            wav->close();
            std::cout << "[anypsp] " << wav->frames() << " audio frames in " << o.wav << "\n";
        }
        if (!game_patches.empty()) std::cout << "[anypsp] patches wrote " << patch_writes << " value(s) during the run\n";
        if (display.interpolate) {
            const auto &t = kernel->ge().interpolation_totals();
            std::cout << "[anypsp] interpolation: " << t.frames << " game frames, " << t.matched << " of " << t.prims
                      << " primitives paired, " << kernel->ge().moved_prims() << " moved";
            if (per_frame && !window_loop) std::cout << ", " << in_between_shown << " in-between pictures captured";
            std::cout << "\n";
        }
        if (audio.dropped_frames() != 0u)
            std::cout << "[anypsp] dropped " << audio.dropped_frames() << " audio frames to keep latency down\n";
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
