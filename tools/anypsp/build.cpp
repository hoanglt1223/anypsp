// SPDX-License-Identifier: GPL-2.0-or-later
#include "build.h"

#include "disc.hpp"
#include "psprecomp/sha256.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
extern char** environ;
#endif

#ifndef ANYPSP_VERSION
#define ANYPSP_VERSION "0.0.0"
#endif

namespace anypsp::driver {

namespace fs = std::filesystem;

namespace {

#if defined(_WIN32)
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Quoting that CommandLineToArgvW and the MSVC runtime undo exactly.
std::wstring quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        out.append(ch == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        backslashes = 0;
        out += ch;
    }
    out.append(backslashes * 2, L'\\');
    return out + L"\"";
}
#endif

std::string exe_suffix(Target target) { return target == Target::Windows ? ".exe" : ""; }

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<std::uint8_t> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path_string(p));
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t at) {
    if (at + 4 > b.size()) return 0;
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
           static_cast<std::uint32_t>(b[at + 2]) << 16 | static_cast<std::uint32_t>(b[at + 3]) << 24;
}

// The PARAM.SFO inside an EBOOT.PBP (homebrew), or nothing.
std::vector<std::uint8_t> pbp_sfo(const fs::path& module) {
    std::vector<std::uint8_t> bytes;
    try {
        bytes = read_file(module);
    } catch (const std::runtime_error&) {
        return {};
    }
    if (bytes.size() < 0x28 || std::memcmp(bytes.data(), "\0PBP", 4) != 0) return {};
    const std::uint32_t start = le32(bytes, 0x08), end = le32(bytes, 0x0C);
    if (start >= end || end > bytes.size()) return {};
    return {bytes.begin() + start, bytes.begin() + end};
}

bool looks_like_iso(const fs::path& p) {
    if (lower(p.extension().string()) == ".iso") return true;
    std::ifstream in(p, std::ios::binary);
    char magic[5] = {};
    in.seekg(0x8001);
    return in.read(magic, 5) && std::memcmp(magic, "CD001", 5) == 0;
}

// anypsp-analyze or psp_recomp: next to this program (installed), or where a build tree puts
// them (psp_recomp under recomp/, per configuration with multi-config generators).
fs::path find_tool(const std::optional<fs::path>& tools, const std::string& name) {
#if defined(_WIN32)
    const std::string file = name + ".exe";
#else
    const std::string& file = name;
#endif
    std::vector<fs::path> dirs;
    if (tools) {
        dirs = {*tools, *tools / "recomp", *tools / "Release", *tools / "recomp" / "Release"};
    } else if (const auto exe = executable_path()) {
        const fs::path dir = exe->parent_path();
        dirs = {dir, dir / "recomp", dir.parent_path() / "recomp" / dir.filename()};
    }
    for (const auto& d : dirs) {
        std::error_code ec;
        if (fs::is_regular_file(d / file, ec)) return d / file;
    }
    throw std::runtime_error("cannot find " + file + (tools ? " under " + path_string(*tools) : " next to this program") +
                             "; pass --tools <folder with anypsp-analyze and psp_recomp>");
}

// The AnyPSP sources the game is compiled against: --source, the installed copy
// (<prefix>/share/anypsp/source), or the tree this program was built from.
fs::path find_source(const std::optional<fs::path>& given) {
    const auto ok = [](const fs::path& p) {
        std::error_code ec;
        return fs::is_regular_file(p / "CMakeLists.txt", ec) && fs::is_regular_file(p / "runtime" / "gpu" / "main_gpu.cpp", ec);
    };
    if (given) {
        if (!ok(*given)) throw std::runtime_error("--source " + path_string(*given) + " is not an AnyPSP source tree");
        return fs::absolute(*given);
    }
    if (const auto exe = executable_path()) {
        const fs::path installed = exe->parent_path().parent_path() / "share" / "anypsp" / "source";
        if (ok(installed)) return installed;
    }
#ifdef ANYPSP_SOURCE_DIR
    if (ok(ANYPSP_SOURCE_DIR)) return fs::path(ANYPSP_SOURCE_DIR);
#endif
    throw std::runtime_error("cannot find the AnyPSP sources; pass --source <AnyPSP source tree>");
}

// SDL3 sources for the game build, so it needs no download: the archive an installed package
// carries (unpacked once into the work folder), or the copy the build tree fetched.
std::optional<fs::path> find_sdl3(const fs::path& source, const fs::path& work, const std::string& cmake) {
    const fs::path archive = source / "third_party" / "SDL3-src.tar.gz";
    std::error_code ec;
    if (fs::is_regular_file(archive, ec)) {
        const fs::path dir = work / "sdl3";
        if (!fs::is_regular_file(dir / "sdl3-src" / "CMakeLists.txt", ec)) {
            fs::create_directories(dir);
            const fs::path script = dir / "extract.cmake";
            std::ofstream out(script, std::ios::binary);
            std::string a = path_string(fs::absolute(archive)), d = path_string(fs::absolute(dir));
            std::replace(a.begin(), a.end(), '\\', '/');
            std::replace(d.begin(), d.end(), '\\', '/');
            out << "file(ARCHIVE_EXTRACT INPUT \"" << a << "\" DESTINATION \"" << d << "\")\n";
            out.close();
            if (!out) throw std::runtime_error("cannot write " + path_string(script));
            run_checked({cmake, "-P", path_string(script)}, "unpacking SDL3");
        }
        return dir / "sdl3-src";
    }
#ifdef ANYPSP_SDL3_SOURCE_DIR
    if (fs::is_regular_file(fs::path(ANYPSP_SDL3_SOURCE_DIR) / "CMakeLists.txt", ec)) return fs::path(ANYPSP_SDL3_SOURCE_DIR);
#endif
    return std::nullopt;  // the game build downloads the pinned release
}

bool has_ninja() {
#if defined(_WIN32)
    const char* separator = ";";
    const char* names[] = {"ninja.exe"};
#else
    const char* separator = ":";
    const char* names[] = {"ninja"};
#endif
    const char* path = std::getenv("PATH");
    if (path == nullptr) return false;
    std::string list = path;
    std::size_t start = 0;
    while (start <= list.size()) {
        const auto end = std::min(list.find(separator, start), list.size());
        const fs::path dir = list.substr(start, end - start);
        std::error_code ec;
        for (const char* n : names)
            if (!dir.empty() && fs::is_regular_file(dir / n, ec)) return true;
        start = end + 1;
    }
    return false;
}

void copy_tree(const fs::path& from, const fs::path& to) {
    fs::create_directories(to);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
}

void write_text(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    if (!out) throw std::runtime_error("cannot write " + path_string(p));
}

}  // namespace

BuildOptions::BuildOptions() : target(host_target()) {}

Target host_target() {
#if defined(_WIN32)
    return Target::Windows;
#elif defined(__APPLE__)
    return Target::MacOS;
#else
    return Target::Linux;
#endif
}

std::string target_name(Target target) {
    switch (target) {
    case Target::Linux: return "linux";
    case Target::Windows: return "windows";
    case Target::MacOS: return "macos";
    case Target::Android: return "android";
    }
    return "?";
}

std::string path_string(const fs::path& p) {
    const std::u8string u = p.u8string();
    return {u.begin(), u.end()};
}

std::optional<fs::path> executable_path() {
#if defined(_WIN32)
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return std::nullopt;
    buf.resize(n);
    return fs::path(buf);
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return std::nullopt;
    return fs::path(buf.c_str());
#else
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return std::nullopt;
    return p;
#endif
}

void run_checked(const std::vector<std::string>& argv, const std::string& what) {
    std::string line;
    for (const auto& a : argv) line += (line.empty() ? "" : " ") + a;
    std::printf("anypsp: %s\n", line.c_str());
    std::fflush(stdout);
    const int rc = run_process(argv);
    if (rc != 0) throw std::runtime_error(what + " failed (exit code " + std::to_string(rc) + ")");
}

int run_process(const std::vector<std::string>& argv) {
    if (argv.empty()) throw std::invalid_argument("run_process: empty command");
#if defined(_WIN32)
    std::wstring line;
    for (const auto& a : argv) line += (line.empty() ? L"" : L" ") + quote(widen(a));
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        throw std::runtime_error("cannot start " + argv[0] + " (error " + std::to_string(GetLastError()) + ")");
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
#else
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, args[0], nullptr, nullptr, args.data(), environ) != 0)
        throw std::runtime_error("cannot start " + argv[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) throw std::runtime_error("waiting for " + argv[0] + " failed");
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
#endif
}

std::string program_name(std::string_view title) {
    std::string out;
    for (const char ch : title) {
        const bool keep = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' ||
                          ch == '_' || ch == ' ';
        if (keep && !(ch == ' ' && !out.empty() && out.back() == ' ')) out += ch;
    }
    const auto first = out.find_first_not_of(' ');
    if (first == std::string::npos) return "game";
    return out.substr(first, out.find_last_not_of(' ') - first + 1);
}

std::string sfo_string(const std::vector<std::uint8_t>& sfo, std::string_view key) {
    if (sfo.size() < 0x14 || std::memcmp(sfo.data(), "\0PSF", 4) != 0) return {};
    const std::uint32_t keys = le32(sfo, 0x08), data = le32(sfo, 0x0C), count = le32(sfo, 0x10);
    for (std::uint32_t i = 0; i < count && i < 4096; ++i) {
        const std::size_t entry = 0x14 + static_cast<std::size_t>(i) * 16;
        if (entry + 16 > sfo.size()) break;
        const std::size_t key_at = keys + (sfo[entry] | (sfo[entry + 1] << 8));
        const std::uint16_t format = static_cast<std::uint16_t>(sfo[entry + 2] | (sfo[entry + 3] << 8));
        const std::uint32_t length = le32(sfo, entry + 4);
        const std::size_t value_at = static_cast<std::size_t>(data) + le32(sfo, entry + 12);
        if (key_at >= sfo.size()) continue;
        const char* name = reinterpret_cast<const char*>(sfo.data() + key_at);
        const std::size_t name_len = strnlen(name, sfo.size() - key_at);
        if (std::string_view(name, name_len) != key) continue;
        if ((format != 0x0204 && format != 0x0004) || value_at + length > sfo.size()) return {};
        std::string value(reinterpret_cast<const char*>(sfo.data() + value_at), length);
        value.resize(strnlen(value.c_str(), value.size()));
        return value;
    }
    return {};
}

GameInput resolve_input(const fs::path& input, const fs::path& cache) {
    GameInput g;
    g.path = input;
    std::error_code ec;
    if (!fs::exists(input, ec)) throw std::runtime_error(path_string(input) + " does not exist");
    try {
        if (fs::is_directory(input, ec) || looks_like_iso(input)) {
            g.kind = fs::is_directory(input, ec) ? GameInput::Kind::DiscFolder : GameInput::Kind::Iso;
            const disc::BootTarget boot = disc::resolve(fs::absolute(input), cache);
            g.module = boot.module;
            g.disc_root = boot.disc_root;
            g.guest_path = boot.guest_path;
            const fs::path sfo = g.disc_root / "PSP_GAME" / "PARAM.SFO";
            if (fs::is_regular_file(sfo, ec)) {
                const auto bytes = read_file(sfo);
                g.title = sfo_string(bytes, "TITLE");
                g.disc_id = sfo_string(bytes, "DISC_ID");
            }
            return g;
        }
    } catch (const std::runtime_error&) {
        throw;
    } catch (const std::exception& e) {
        throw std::runtime_error(e.what());
    }
    switch (disc::classify_executable(input)) {
    case disc::ExecutableKind::Encrypted:
        throw std::runtime_error(path_string(input) + " is encrypted (~PSP); AnyPSP decrypts nothing, supply a decrypted executable");
    case disc::ExecutableKind::Other:
        throw std::runtime_error(path_string(input) + " is not an ISO image, a disc folder, an ELF/PRX or an EBOOT.PBP");
    case disc::ExecutableKind::Elf:
    case disc::ExecutableKind::Pbp:
        break;
    }
    g.kind = GameInput::Kind::Module;
    g.module = fs::absolute(input);
    g.guest_path = "ms0:/PSP/GAME/ANYPSP/EBOOT.PBP";
    const auto sfo = pbp_sfo(input);
    g.title = sfo_string(sfo, "TITLE");
    g.disc_id = sfo_string(sfo, "DISC_ID");
    return g;
}

std::string build_usage() {
    return "usage: anypsp build <game.iso | disc folder | EBOOT.BIN | EBOOT.PBP | module.prx> [options]\n"
           "\n"
           "Analyses and translates the game's executable, compiles the result with the AnyPSP runtime and\n"
           "writes a program that plays it: <output>/<Name>/<Name>[.exe] next to a copy of the game files\n"
           "(Windows, Linux), <output>/<Name>.app (macOS) or <output>/<Name>.apk (Android). Executables must\n"
           "be decrypted; AnyPSP ships no keys and decrypts nothing.\n"
           "\n"
           "  -o, --output DIR    where the result goes (default: current folder)\n"
           "  --target T          linux, windows, macos (the host's own; default) or android\n"
           "  --name NAME         program name (default: the disc title, else the module name)\n"
           "  --work DIR          analysis, generated code and build trees (default: <output>/<input name>-build)\n"
           "  --feedback FILE     trace feedback from earlier runs (runners' --feedback output)\n"
           "  --opt d|1|2         optimisation of the generated code (default 1; d builds fastest)\n"
           "  --jobs N            parallel compile jobs (default: the build tool's choice)\n"
           "  --no-game           do not copy the game files; the program then takes the game as its argument\n"
           "  --source DIR        AnyPSP source tree (default: <prefix>/share/anypsp/source or the built-from tree)\n"
           "  --tools DIR         folder with anypsp-analyze and psp_recomp (default: next to this program)\n"
           "  --toolchain FILE    CMake toolchain file for the game build (cross builds, e.g. Linux arm64)\n"
           "  --cmake PATH        cmake executable (default: cmake on PATH)\n"
           "  --generator NAME    CMake generator (default: Ninja when on PATH, else CMake's choice)\n"
           "  --build-type TYPE   CMake configuration (default: Release)\n"
           "  --codesign ID       macOS signing identity (default: ad-hoc)\n"
           "\n" + android::usage();
}

std::optional<std::string> parse_build_args(std::span<const std::string_view> args, BuildOptions& out) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view a = args[i];
        const bool has_value = i + 1 < args.size();
        const auto value = [&]() { return std::string(args[++i]); };
        if (const auto android_error = android::parse_option(args, i, out.android)) {
            if (!android_error->empty()) return *android_error;
        } else if (a == "--no-game") {
            out.copy_game = false;
        } else if (a.starts_with("-") && !has_value) {
            return std::string(a) + " needs a value or is unknown";
        } else if (a == "--target") {
            const std::string t = value();
            if (t == "linux") out.target = Target::Linux;
            else if (t == "windows") out.target = Target::Windows;
            else if (t == "macos") out.target = Target::MacOS;
            else if (t == "android") out.target = Target::Android;
            else return "unknown target '" + t + "' (linux, windows, macos, android)";
        } else if (a == "-o" || a == "--output") {
            out.output_dir = value();
        } else if (a == "--work") {
            out.work_dir = fs::path(value());
        } else if (a == "--name") {
            out.name = program_name(value());
        } else if (a == "--feedback") {
            out.feedback = fs::path(value());
        } else if (a == "--opt") {
            out.opt = value();
            if (out.opt != "d" && out.opt != "1" && out.opt != "2") return "--opt takes d, 1 or 2";
        } else if (a == "--jobs") {
            const std::string v = value();
            char* end = nullptr;
            const long n = std::strtol(v.c_str(), &end, 10);
            if (*end != '\0' || n < 1 || n > 256) return "--jobs takes 1-256";
            out.jobs = static_cast<int>(n);
        } else if (a == "--source") {
            out.source = fs::path(value());
        } else if (a == "--tools") {
            out.tools = fs::path(value());
        } else if (a == "--toolchain") {
            out.toolchain = fs::path(value());
        } else if (a == "--cmake") {
            out.cmake = value();
        } else if (a == "--generator") {
            out.generator = value();
        } else if (a == "--build-type") {
            out.build_type = value();
        } else if (a == "--codesign") {
            out.codesign_identity = value();
        } else if (a.starts_with("-")) {
            return "unknown option " + std::string(a);
        } else if (out.input.empty()) {
            out.input = fs::path(std::string(a));
        } else {
            return "unexpected argument " + std::string(a);
        }
    }
    if (out.input.empty()) return std::string("no game given");
    if (out.target != Target::Android && out.target != host_target() && !out.toolchain) {
        return "a " + target_name(out.target) + " program is built on " + target_name(out.target) +
               " (this is " + target_name(host_target()) + "); only android, or --toolchain cross builds, work from here";
    }
    return std::nullopt;
}

fs::path build(const BuildOptions& o) {
    const fs::path work = fs::absolute(o.work_dir ? *o.work_dir : o.output_dir / (o.input.stem().string() + "-build"));
    fs::create_directories(work);
    const GameInput game = resolve_input(o.input, work / "disc");
    const fs::path source = find_source(o.source);
    // Child tools and the game build find the NID database here wherever they were built.
    const std::string nids = path_string(source / "data" / "nids.toml");
#if defined(_WIN32)
    _putenv_s("ANYPSP_NID_DATABASE", nids.c_str());
#else
    setenv("ANYPSP_NID_DATABASE", nids.c_str(), 1);
#endif
    const fs::path analyze = find_tool(o.tools, "anypsp-analyze");
    const fs::path recomp = find_tool(o.tools, "psp_recomp");
    const std::string module_hash = psprecomp::sha256_file(game.module);
    std::printf("anypsp: %s (%s), %s, SHA-256 %s\n", game.title.empty() ? "(no title)" : game.title.c_str(),
                game.disc_id.empty() ? "no disc ID" : game.disc_id.c_str(), path_string(game.module).c_str(), module_hash.c_str());

    // 1. Analysis: function discovery, with the feedback of earlier runs.
    const fs::path analysis = work / "analysis";
    fs::remove_all(analysis);
    std::vector<std::string> analyze_cmd = {path_string(analyze), "--out", path_string(analysis), "--nids", nids};
    if (o.feedback) {
        if (!fs::is_regular_file(*o.feedback)) throw std::runtime_error("--feedback " + path_string(*o.feedback) + " does not exist");
        analyze_cmd.insert(analyze_cmd.end(), {"--feedback", path_string(fs::absolute(*o.feedback))});
    }
    analyze_cmd.push_back(path_string(game.module));
    run_checked(analyze_cmd, "anypsp-analyze");
    fs::path seeds;
    for (const auto& e : fs::directory_iterator(analysis)) {
        const std::string n = e.path().filename().string();
        if (n.starts_with("0_") && n.ends_with("_seeds.csv")) seeds = e.path();
    }
    if (seeds.empty()) throw std::runtime_error("anypsp-analyze wrote no seeds under " + path_string(analysis));

    // 2. Translation.
    const fs::path gen = work / "gen";
    fs::remove_all(gen);
    run_checked({path_string(recomp), path_string(game.module), "--auto", path_string(gen), "--seeds", path_string(seeds)},
                "psp_recomp");

    std::string name = o.name ? *o.name : program_name(game.title.empty() ? game.module.stem().string() : game.title);
    const std::optional<fs::path> sdl3 = find_sdl3(source, work, o.cmake);

    if (o.target == Target::Android) {
        android::Game ag;
        ag.gen_dir = gen;
        ag.name = name;
        ag.title = game.title.empty() ? name : game.title;
        ag.disc_id = game.disc_id;
        ag.module_sha256 = module_hash;
        ag.guest_path = game.guest_path;
        if (game.kind == GameInput::Kind::Module) {
            ag.files.emplace_back(game.module, game.module.filename().string());
        } else if (o.android.embed_game) {
            for (const auto& e : fs::recursive_directory_iterator(game.disc_root)) {
                if (!e.is_regular_file()) continue;
                std::string rel = path_string(fs::relative(e.path(), game.disc_root));
                std::replace(rel.begin(), rel.end(), '\\', '/');
                ag.files.emplace_back(e.path(), "disc/" + rel);
            }
        } else {
            ag.needs_disc = true;
        }
        const android::Paths paths{source, sdl3, work, fs::absolute(o.output_dir), o.cmake, o.generator, o.build_type, o.opt, o.jobs};
        return android::build_apk(ag, paths, o.android);
    }

    // 3. The program: the generated code linked with the runtime and the desktop host.
    const fs::path bin = work / ("out-" + target_name(o.target));
    std::vector<std::string> configure = {o.cmake, "-S", path_string(source), "-B", path_string(bin),
                                          "-DCMAKE_BUILD_TYPE=" + o.build_type,
                                          "-DANYPSP_RUNNER_NAME=game",
                                          "-DANYPSP_RUNNER_GENERATED=" + path_string(gen),
                                          "-DANYPSP_RUNNER_OPT=" + o.opt,
                                          "-DANYPSP_GAME_SHA256=" + module_hash,
                                          "-DANYPSP_BUILD_DRIVER=OFF"};
    if (sdl3) configure.push_back("-DFETCHCONTENT_SOURCE_DIR_SDL3=" + path_string(*sdl3));
    if (o.toolchain) configure.push_back("-DCMAKE_TOOLCHAIN_FILE=" + path_string(fs::absolute(*o.toolchain)));
    if (o.generator) {
        configure.insert(configure.end(), {"-G", *o.generator});
    } else if (has_ninja() && !fs::exists(bin / "CMakeCache.txt")) {
        configure.insert(configure.end(), {"-G", "Ninja"});
    }
    run_checked(configure, "CMake configure");
    std::vector<std::string> build_cmd = {o.cmake, "--build", path_string(bin), "--config", o.build_type, "--target", "anypsp-game"};
    if (o.jobs) build_cmd.insert(build_cmd.end(), {"--parallel", std::to_string(*o.jobs)});
    run_checked(build_cmd, "CMake build");

    const std::string suffix = exe_suffix(o.target);
    fs::path built;
    for (const auto& c : {bin / o.build_type / ("anypsp-game" + suffix), bin / ("anypsp-game" + suffix)})
        if (fs::is_regular_file(c)) built = c;
    if (built.empty()) throw std::runtime_error("the build produced no anypsp-game program under " + path_string(bin));

    // 4. Packaging: the program, the NID database, the licence, and the game files.
    fs::create_directories(o.output_dir);
    const auto copy_game = [&](const fs::path& dir) {
        if (!o.copy_game) return;
        switch (game.kind) {
        case GameInput::Kind::Iso:
            fs::copy_file(game.path, dir / "game.iso", fs::copy_options::overwrite_existing);
            break;
        case GameInput::Kind::DiscFolder:
            fs::remove_all(dir / "game");
            copy_tree(game.disc_root, dir / "game");
            break;
        case GameInput::Kind::Module:
            fs::copy_file(game.module, dir / (disc::classify_executable(game.module) == disc::ExecutableKind::Pbp ? "EBOOT.PBP" : "EBOOT.BIN"),
                          fs::copy_options::overwrite_existing);
            break;
        }
    };
    // The NID database goes next to the program (anypsp::loader::default_nid_database).
    const auto copy_support = [&](const fs::path& program_dir, const fs::path& dir) {
        fs::copy_file(source / "data" / "nids.toml", program_dir / "nids.toml", fs::copy_options::overwrite_existing);
        fs::copy_file(source / "LICENSE", dir / "LICENSE.txt", fs::copy_options::overwrite_existing);
        write_text(dir / "README.txt",
                   name + ", translated from the PSP by AnyPSP " ANYPSP_VERSION " (GPL-2.0-or-later, see LICENSE.txt;\n"
                   "source: https://github.com/hoanglt1223/anypsp).\n\n"
                   "The program plays the game files next to it (game.iso, the game/ folder or EBOOT.BIN), or the\n"
                   "game given as its first argument. It only runs the exact executable it was built from\n"
                   "(SHA-256 " + module_hash + ").\n"
                   "Saves, settings and the extracted disc live in the per-user AnyPSP data folder.\n"
                   "Options: run it with --help; keys, menu and settings: docs/user/desktop.md in the AnyPSP sources.\n");
    };
    if (o.target == Target::MacOS) {
        const fs::path app = o.output_dir / (name + ".app");
        fs::remove_all(app);
        fs::create_directories(app / "Contents" / "MacOS");
        fs::create_directories(app / "Contents" / "Resources");
        fs::copy_file(built, app / "Contents" / "MacOS" / name);
        copy_support(app / "Contents" / "MacOS", app / "Contents" / "Resources");
        copy_game(app / "Contents" / "Resources");  // SDL_GetBasePath() of a bundle
        write_text(app / "Contents" / "Info.plist",
                   "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                   "<plist version=\"1.0\"><dict>\n"
                   "  <key>CFBundleExecutable</key><string>" + android::xml_escape(name) + "</string>\n"
                   "  <key>CFBundleIdentifier</key><string>io.anyport.anypsp.g" + module_hash.substr(0, 12) + "</string>\n"
                   "  <key>CFBundleName</key><string>" + android::xml_escape(name) + "</string>\n"
                   "  <key>CFBundlePackageType</key><string>APPL</string>\n"
                   "  <key>CFBundleShortVersionString</key><string>" ANYPSP_VERSION "</string>\n"
                   "  <key>NSHighResolutionCapable</key><true/>\n"
                   "</dict></plist>\n");
        run_checked({"codesign", "--force", "--deep", "--sign", o.codesign_identity.value_or("-"), path_string(app)}, "codesign");
        return app;
    }
    const fs::path dir = o.output_dir / name;
    fs::create_directories(dir);
    const fs::path exe = dir / (name + suffix);
    fs::copy_file(built, exe, fs::copy_options::overwrite_existing);
    copy_support(dir, dir);
    copy_game(dir);
    return exe;
}

}  // namespace anypsp::driver
