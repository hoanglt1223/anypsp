// SPDX-License-Identifier: GPL-2.0-or-later
//
// Android host of a translated game (libmain.so, started by GameActivity through SDL): the HLE
// kernel with the translated code, the GE drawn with the Vulkan backend (gpu/vulkan, presented
// through vk_present.cpp) when the device has Vulkan 1.1, else with OpenGL ES 3 (host/ge_gl.cpp),
// SDL audio, and input from the touch overlay (GameActivity's TouchOverlay), gamepads and
// keyboards. Game files are prepared by SetupActivity in the app's files folder:
//   anypsp/game.properties, anypsp/nids.toml   from the APK
//   game/<module>                              an embedded homebrew executable
//   game.iso                                   a picked ISO, extracted here into game-disc/ on first start
//   game-disc/                                 the disc (picked folder, extracted ISO, or embedded)
//   memstick/                                  saves
// Arguments after the files folder (GameActivity EXTRA_ARGS) are for tests:
//   --frames N   stop after N vblanks      --dump file.png   write the last frame
//   --hashes F   write "<frame> <hash>" lines per vblank   --input frame:mask,...   scripted pad
//   --scale N    internal resolution (default 2)
//   --renderer auto|vulkan|gl   (auto: Vulkan when the build and the device have it, else GL ES 3)
// The same host builds on Linux with Mesa's OpenGL ES 3 (anypsp-game-gles, cmake/hosts.cmake), for
// GLES-only ARM handhelds and to test the Android path on a desktop; there the files folder is argv[1].

#include "disc.hpp"
#include "frame_output.hpp"
#include "ge_gl.hpp"
#ifdef ANYPSP_VULKAN
#include "vk_present.hpp"
#endif
#include "kernel.hpp"

#include "elf_prx.hpp"
#include "nid_db.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"
#include "psprecomp/sha256.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <GLES3/gl3.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#include <jni.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef ANYPSP_GAME_SHA256
#error "ANYPSP_GAME_SHA256 must give the SHA-256 of the translated executable"
#endif

namespace psprecomp {
void register_dynamic_code();
}

namespace {

namespace fs = std::filesystem;

constexpr int kWidth = 480, kHeight = 272;
constexpr double kVblankSeconds = 1.0 / 59.94;

std::atomic<std::uint32_t> g_touch_buttons{0};
SDL_AudioStream *g_audio = nullptr;

#ifdef __ANDROID__
// stdout and stderr (the runtime's [anypsp] and [hle] lines) into logcat.
void log_to_logcat() {
    static int fds[2];
    if (pipe(fds) != 0) return;
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    std::thread([] {
        std::string line;
        char buffer[512];
        for (ssize_t n; (n = read(fds[0], buffer, sizeof buffer)) > 0;) {
            for (ssize_t i = 0; i < n; ++i) {
                if (buffer[i] == '\n') {
                    __android_log_write(ANDROID_LOG_INFO, "anypsp", line.c_str());
                    line.clear();
                } else {
                    line += buffer[i];
                }
            }
        }
    }).detach();
}
#else
void log_to_logcat() {}
#endif

std::map<std::string, std::string> read_properties(const fs::path &path) {
    std::map<std::string, std::string> values;
    std::ifstream in(path);
    if (!in) throw psprecomp::Error("Cannot open " + path.string());
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq != std::string::npos) values[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return values;
}

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

struct Options {
    fs::path files;
    long frames = -1;
    int scale = 2;
    std::string dump, hashes, renderer = "auto";
    std::map<long, std::uint32_t> input;
};

Options parse(int argc, char **argv) {
    Options o;
    if (argc >= 2) o.files = argv[1];
#ifdef __ANDROID__
    else if (const char *path = SDL_GetAndroidInternalStoragePath()) o.files = path;
#else
    else throw psprecomp::Error("usage: anypsp-game-gles <files folder> [--frames N] [--dump f.png] [--hashes f] [--input s] [--scale n] [--renderer auto|vulkan|gl]");
#endif
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string flag = argv[i], value = argv[i + 1];
        if (flag == "--frames") o.frames = std::stol(value);
        else if (flag == "--scale") o.scale = std::clamp(std::stoi(value), 1, 4);
        else if (flag == "--dump") o.dump = value;
        else if (flag == "--hashes") o.hashes = value;
        else if (flag == "--input") o.input = parse_input_script(value);
        else if (flag == "--renderer") {
            if (value != "auto" && value != "vulkan" && value != "gl") throw psprecomp::Error("--renderer takes auto, vulkan or gl");
            o.renderer = value;
        }
        else std::cerr << "[anypsp] ignoring unknown argument " << flag << "\n";
    }
    return o;
}

struct Game {
    fs::path module, disc_root;
    std::string guest_path;
};

// The game files SetupActivity prepared; an ISO is extracted into game-disc/ once.
Game locate_game(const fs::path &files, const std::map<std::string, std::string> &props) {
    const fs::path iso = files / "game.iso", disc = files / "game-disc";
    if (fs::is_regular_file(iso)) {
        const fs::path part = files / "game-disc.extracting";
        fs::remove_all(part);
        std::cout << "[anypsp] extracting " << iso << "\n";
        anypsp::disc::extract_iso(iso, part);
        fs::remove_all(disc);
        fs::rename(part, disc);
        fs::remove(iso);
    }
    if (fs::is_directory(disc / "PSP_GAME")) {
        const auto boot = anypsp::disc::find_boot(disc);
        return {boot.module, boot.disc_root, boot.guest_path};
    }
    const auto module = props.find("module");
    if (module != props.end() && fs::is_regular_file(files / "game" / module->second)) {
        const auto guest = props.find("guest_path");
        return {files / "game" / module->second, files / "game",
                guest != props.end() ? guest->second : std::string("ms0:/PSP/GAME/ANYPSP/EBOOT.PBP")};
    }
    throw psprecomp::Error("No game files in " + files.string() + "; start the app again to choose them.");
}

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

// Keyboard (emulator, Chromebooks, Bluetooth keyboards): arrows, Z/X/A/S, Q/W, Enter, Right Shift.
std::uint32_t keyboard_buttons() {
    const bool *keys = SDL_GetKeyboardState(nullptr);
    struct Key { SDL_Scancode code; std::uint32_t mask; };
    static const Key map[] = {{SDL_SCANCODE_UP, 0x10},     {SDL_SCANCODE_RIGHT, 0x20}, {SDL_SCANCODE_DOWN, 0x40},
                              {SDL_SCANCODE_LEFT, 0x80},   {SDL_SCANCODE_Z, 0x4000},   {SDL_SCANCODE_X, 0x2000},
                              {SDL_SCANCODE_A, 0x8000},    {SDL_SCANCODE_S, 0x1000},   {SDL_SCANCODE_Q, 0x100},
                              {SDL_SCANCODE_W, 0x200},     {SDL_SCANCODE_RETURN, 0x8}, {SDL_SCANCODE_RSHIFT, 0x1}};
    std::uint32_t mask = 0;
    for (const auto &k : map)
        if (keys[k.code]) mask |= k.mask;
    return mask;
}

struct Pad {
    std::uint32_t buttons = 0;
    std::uint8_t lx = 128, ly = 128;
};

// Cross is the bottom face button (South), as on the PSP; analog from the left stick.
Pad gamepad_state(const std::vector<SDL_Gamepad *> &pads) {
    Pad p;
    struct Button { SDL_GamepadButton b; std::uint32_t mask; };
    static const Button map[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x10},       {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x20},    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x40},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x80},     {SDL_GAMEPAD_BUTTON_SOUTH, 0x4000},       {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
        {SDL_GAMEPAD_BUTTON_WEST, 0x8000},        {SDL_GAMEPAD_BUTTON_NORTH, 0x1000},       {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x100},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x200}, {SDL_GAMEPAD_BUTTON_START, 0x8},        {SDL_GAMEPAD_BUTTON_BACK, 0x1}};
    for (SDL_Gamepad *pad : pads) {
        for (const auto &m : map)
            if (SDL_GetGamepadButton(pad, m.b)) p.buttons |= m.mask;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) p.buttons |= 0x100;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) p.buttons |= 0x200;
        const int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        if (std::abs(x) > 4000 || std::abs(y) > 4000) {
            p.lx = static_cast<std::uint8_t>(std::clamp((x + 32768) >> 8, 0, 255));
            p.ly = static_cast<std::uint8_t>(std::clamp((y + 32768) >> 8, 0, 255));
        }
    }
    return p;
}

// Where the GE draws and how its pictures reach the window.
class Screen {
public:
    virtual ~Screen() = default;
    virtual pspweb::GeSurfaces &renderer() = 0;
    virtual void render(const pspweb::DisplayState &d) = 0;  // the displayed framebuffer into the output picture
    virtual void show() = 0;                                  // the output picture letterboxed into the window
    virtual std::vector<std::uint8_t> read() = 0;            // the output picture, RGBA rows, top row first
    virtual std::string device() const = 0;
    int width() { return kWidth * renderer().render_scale(); }
    int height() { return kHeight * renderer().render_scale(); }
};

SDL_Window *make_window(const std::string &title, SDL_WindowFlags api) {
    SDL_Window *window = SDL_CreateWindow(title.c_str(), 0, 0, api | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr) throw psprecomp::Error(std::string("cannot create the window: ") + SDL_GetError());
    return window;
}

class GlScreen final : public Screen {
public:
    GlScreen(psprecomp::GuestMemory &memory, int scale, const std::string &title) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        window_ = make_window(title, SDL_WINDOW_OPENGL);
        context_ = SDL_GL_CreateContext(window_);
        if (context_ == nullptr) {
            SDL_DestroyWindow(window_);
            throw psprecomp::Error(std::string("cannot create an OpenGL ES 3 context: ") + SDL_GetError());
        }
        SDL_GL_MakeCurrent(window_, context_);
        SDL_GL_SetSwapInterval(1);
        device_ = std::string(reinterpret_cast<const char *>(glGetString(GL_RENDERER))) + ", " +
                  reinterpret_cast<const char *>(glGetString(GL_VERSION));
        renderer_ = std::make_unique<pspweb::GeGl>(memory);
        if (!renderer_->init()) throw psprecomp::Error("the GE's OpenGL ES 3 backend failed to start");
        renderer_->set_render_scale(scale);
        glGenTextures(1, &texture_);
        glBindTexture(GL_TEXTURE_2D, texture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width(), height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenFramebuffers(1, &fbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) throw psprecomp::Error("output framebuffer incomplete");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    ~GlScreen() override {
        renderer_.reset();
        SDL_GL_DestroyContext(context_);
        SDL_DestroyWindow(window_);
    }
    pspweb::GeSurfaces &renderer() override { return *renderer_; }
    std::string device() const override { return "OpenGL ES: " + device_; }

    void render(const pspweb::DisplayState &d) override {
        renderer_->present(d.framebuffer, d.stride, d.format, fbo_, width(), height());
    }

    void show() override {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        int pw = w, ph = w * kHeight / kWidth;
        if (ph > h) {
            ph = h;
            pw = h * kWidth / kHeight;
        }
        const int x = (w - pw) / 2, y = (h - ph) / 2;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glViewport(0, 0, w, h);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBlitFramebuffer(0, 0, width(), height(), x, y, x + pw, y + ph, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        SDL_GL_SwapWindow(window_);
    }

    std::vector<std::uint8_t> read() override {
        const auto row = static_cast<std::size_t>(width()) * 4u;
        std::vector<std::uint8_t> bottom_up(row * static_cast<std::size_t>(height())), rgba(bottom_up.size());
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, width(), height(), GL_RGBA, GL_UNSIGNED_BYTE, bottom_up.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        for (int y = 0; y < height(); ++y)
            std::copy_n(&bottom_up[static_cast<std::size_t>(height() - 1 - y) * row], row, &rgba[static_cast<std::size_t>(y) * row]);
        return rgba;
    }

private:
    SDL_Window *window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    std::string device_;
    std::unique_ptr<pspweb::GeGl> renderer_;
    GLuint fbo_ = 0, texture_ = 0;
};

#ifdef ANYPSP_VULKAN
class VkScreen final : public Screen {
public:
    VkScreen(psprecomp::GuestMemory &memory, int scale, const std::string &title, const fs::path &pipeline_cache)
        : window_(make_window(title, SDL_WINDOW_VULKAN)) {
        try {
            presenter_ = std::make_unique<anypsp::android::VkPresenter>(window_);
            pspweb::VkOptions options;
            options.pipeline_cache = pipeline_cache.string();
            renderer_ = std::make_unique<pspweb::GeVk>(memory, presenter_->context(), options);
            renderer_->set_render_scale(scale);
        } catch (...) {
            renderer_.reset();
            presenter_.reset();
            SDL_DestroyWindow(window_);
            throw;
        }
    }
    ~VkScreen() override {
        renderer_.reset();
        presenter_.reset();
        SDL_DestroyWindow(window_);
    }
    pspweb::GeSurfaces &renderer() override { return *renderer_; }
    std::string device() const override { return "Vulkan: " + presenter_->context().device_name; }
    void render(const pspweb::DisplayState &d) override { renderer_->render_output(d.framebuffer, d.stride, d.format); }
    void show() override {
        renderer_->device().finish();
        presenter_->present(renderer_->device().output_image(), width(), height());
    }
    std::vector<std::uint8_t> read() override {
        std::vector<std::uint8_t> rgba;
        renderer_->device().read_output(rgba);
        return rgba;
    }

private:
    SDL_Window *window_;
    std::unique_ptr<anypsp::android::VkPresenter> presenter_;
    std::unique_ptr<pspweb::GeVk> renderer_;
};
#endif

std::unique_ptr<Screen> make_screen(const Options &o, psprecomp::GuestMemory &memory, const std::string &title, const fs::path &files) {
#ifdef ANYPSP_VULKAN
    if (o.renderer != "gl") {
        try {
            return std::make_unique<VkScreen>(memory, o.scale, title, files / "cache" / "vulkan-pipelines.bin");
        } catch (const std::exception &e) {
            if (o.renderer == "vulkan") throw;
            std::cout << "[anypsp] no Vulkan (" << e.what() << "); drawing with OpenGL ES 3\n";
        }
    }
#else
    if (o.renderer == "vulkan") throw psprecomp::Error("this build has no Vulkan renderer");
#endif
    return std::make_unique<GlScreen>(memory, o.scale, title);
}

int run(const Options &o) {
    const fs::path files = o.files;
    const std::string nids = (files / "anypsp" / "nids.toml").string();
    setenv("ANYPSP_NID_DATABASE", nids.c_str(), 1);
    const auto props = read_properties(files / "anypsp" / "game.properties");
    const Game game = locate_game(files, props);
    const std::string hash = psprecomp::sha256_file(game.module);
    if (hash != ANYPSP_GAME_SHA256) {
        fs::remove_all(files / "game-disc");
        throw psprecomp::Error("These game files are not the ones this app was built from (executable SHA-256 " + hash +
                               "). Start the app again and choose the same copy of the game.");
    }

    const auto title = props.count("title") ? props.at("title") : std::string("AnyPSP");
    fs::create_directories(files / "cache");

    auto runtime = std::make_unique<psprecomp::Runtime>();
    auto &rt = *runtime;
    anypsp::loader::load_nid_toml(anypsp::loader::default_nid_database(), rt.nids());
    const auto module = anypsp::loader::load_module(
        rt.memory(), psprecomp::Elf32Image::from_bytes(anypsp::loader::read_executable(game.module), game.module.string()),
        game.module.string(), psprecomp::kDefaultPspUserLoadBase);
    if (!module.info) throw psprecomp::Error(game.module.string() + " has no module info");
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

    // Before the kernel, so it outlives the GE pointing at it.
    const std::unique_ptr<Screen> screen_owner = make_screen(o, rt.memory(), title, files);
    Screen &screen = *screen_owner;
    std::cout << "[anypsp] " << screen.device() << ", scale " << screen.renderer().render_scale() << "\n";
    auto kernel = std::make_unique<pspweb::Kernel>(rt);
    kernel->set_interactive_dialogs(false);
    screen.renderer().set_coherence(pspweb::GeSurfaces::Coherence::Lazy);
    kernel->track_gpu_vram();
    kernel->ge().set_gl(&screen.renderer());
    kernel->set_test_output([](std::string_view text) { std::cout.write(text.data(), static_cast<std::streamsize>(text.size())); });

    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 44100};
    g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (g_audio != nullptr) SDL_ResumeAudioStreamDevice(g_audio);
    else std::cout << "[anypsp] no audio: " << SDL_GetError() << "\n";
    kernel->set_audio_sink([](const std::int16_t *samples, std::uint32_t frames) {
        // Drop rather than queue past ~100 ms, so sound never lags the picture.
        if (g_audio != nullptr && SDL_GetAudioStreamQueued(g_audio) < 44100 * 4 / 10)
            SDL_PutAudioStreamData(g_audio, samples, static_cast<int>(frames * 4u));
    });

    const fs::path memstick = files / "memstick";
    fs::create_directories(memstick);
    kernel->reserve(module.base, module.end - module.base, "module:" + module.info->name);
    kernel->set_roots(game.disc_root, memstick);
    kernel->set_flash_root(game.disc_root / "flash0");
    kernel->set_user_font_dir(files / "flash0" / "font");
    kernel->add_main_module(*module.info, module.base, module.end - module.base);
    kernel->install(imports);
    kernel->boot(module.entry, module.info->gp, game.guest_path);
    std::cout << "[anypsp] " << module.info->name << " booted from " << game.guest_path << ", " << rt.function_count()
              << " translated entries\n";

    std::ofstream hashes;
    if (!o.hashes.empty()) hashes.open(files / o.hashes, std::ios::binary);
    std::vector<SDL_Gamepad *> pads;
    long frame = 0;
    bool running = true, quit = false;
    double owed = 0.0;  // vblanks due by the wall clock and not yet run
    auto last = std::chrono::steady_clock::now();
    const auto started = last;
    while (running && !quit && (o.frames < 0 || frame < o.frames)) {
        for (SDL_Event e; SDL_PollEvent(&e);) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_TERMINATING: quit = true; break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (SDL_Gamepad *pad = SDL_OpenGamepad(e.gdevice.which)) pads.push_back(pad);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                std::erase_if(pads, [&](SDL_Gamepad *pad) {
                    if (SDL_GetGamepadID(pad) != e.gdevice.which) return false;
                    SDL_CloseGamepad(pad);
                    return true;
                });
                break;
            case SDL_EVENT_DID_ENTER_FOREGROUND: last = std::chrono::steady_clock::now(); owed = 0.0; break;
            default: break;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        owed = std::min(owed + std::chrono::duration<double>(now - last).count() / kVblankSeconds, 4.0);
        last = now;
        // Scripted runs go as fast as the device allows; play keeps PSP time.
        int due = o.frames >= 0 ? 1 : static_cast<int>(owed);
        owed -= due;
        for (; due > 0 && running && (o.frames < 0 || frame < o.frames); --due) {
            Pad pad = gamepad_state(pads);
            pad.buttons |= g_touch_buttons.load() | keyboard_buttons();
            if (!o.input.empty()) {
                static std::uint32_t scripted = 0;
                if (const auto it = o.input.find(frame); it != o.input.end()) scripted = it->second;
                pad.buttons = scripted;
            }
            kernel->set_pad(pad.buttons, pad.lx, pad.ly);
            running = kernel->frame(50.0);
            ++frame;
            if (hashes.is_open()) {
                screen.render(kernel->display());
                hashes << frame << " " << std::hex << anypsp::gpu::frame_hash(screen.read()) << std::dec << "\n";
            }
        }
        screen.render(kernel->display());
        screen.show();
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cout << "[anypsp] ran " << frame << " frames in " << static_cast<long>(seconds) << " s; "
              << (quit ? "closed" : running ? "frame limit reached" : "guest stopped: " + kernel->halt_reason()) << "\n";
    if (!o.dump.empty()) {
        screen.render(kernel->display());
        anypsp::gpu::write_png((files / o.dump).string(), screen.read(), screen.width(), screen.height());
        std::cout << "[anypsp] wrote " << (files / o.dump).string() << "\n";
    }
    if (g_audio != nullptr) SDL_DestroyAudioStream(g_audio);
    g_audio = nullptr;
    for (SDL_Gamepad *pad : pads) SDL_CloseGamepad(pad);
    return running || quit || o.frames >= 0 ? 0 : 1;
}

} // namespace

#ifdef __ANDROID__
extern "C" JNIEXPORT void JNICALL Java_io_anyport_anypsp_GameActivity_nativeSetTouchButtons(JNIEnv *, jclass, jint mask) {
    g_touch_buttons.store(static_cast<std::uint32_t>(mask));
}
#endif

int main(int argc, char **argv) {
    log_to_logcat();
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        std::cerr << "[anypsp] SDL_Init failed: " << SDL_GetError() << "\n";
        return 1;
    }
    int rc = 1;
    try {
        rc = run(parse(argc, argv));
    } catch (const std::exception &e) {
        std::cerr << "[anypsp] fatal: " << e.what() << "\n";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "AnyPSP", e.what(), nullptr);
    }
    SDL_Quit();
    return rc;
}
