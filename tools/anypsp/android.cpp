// SPDX-License-Identifier: GPL-2.0-or-later
// APK assembly follows AnyGBA's tools/anygba/android.cpp (MIT, same author).
#include "android.h"

#include "build.h"
#include "zip.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace anypsp::driver::android {

namespace fs = std::filesystem;

namespace {

constexpr int kMinSdk = 24;
constexpr int kTargetSdk = 35;

#if defined(_WIN32)
constexpr const char* kExe = ".exe";
constexpr const char* kNdkHost = "windows-x86_64";
#elif defined(__APPLE__)
constexpr const char* kExe = "";
constexpr const char* kNdkHost = "darwin-x86_64";
#else
constexpr const char* kExe = "";
constexpr const char* kNdkHost = "linux-x86_64";
#endif

std::string str(const fs::path& p) { return path_string(p); }

std::optional<fs::path> env_path(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) return std::nullopt;
    return fs::path(v);
}

std::vector<int> version_numbers(const std::string& s) {
    std::vector<int> out;
    int n = 0;
    bool digits = false;
    for (const char ch : s) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            n = n * 10 + (ch - '0');
            digits = true;
        } else if (digits) {
            out.push_back(n);
            n = 0;
            digits = false;
        }
    }
    if (digits) out.push_back(n);
    return out;
}

// Subfolders of `dir` for which `ok` holds, newest version first.
std::vector<fs::path> versioned_dirs(const fs::path& dir, const auto& ok) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_directory(ec) && ok(e.path())) out.push_back(e.path());
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
        return version_numbers(a.filename().string()) > version_numbers(b.filename().string());
    });
    return out;
}

struct Toolchain {
    fs::path sdk, build_tools, android_jar, java, javac, keytool;
};

fs::path find_sdk(const Options& o) {
    std::vector<fs::path> candidates;
    if (o.sdk) candidates.push_back(*o.sdk);
    for (const char* var : {"ANDROID_HOME", "ANDROID_SDK_ROOT"})
        if (auto p = env_path(var)) candidates.push_back(*p);
#if defined(_WIN32)
    if (auto p = env_path("LOCALAPPDATA")) candidates.push_back(*p / "Android" / "Sdk");
#elif defined(__APPLE__)
    if (auto p = env_path("HOME")) candidates.push_back(*p / "Library" / "Android" / "sdk");
#else
    if (auto p = env_path("HOME")) {
        candidates.push_back(*p / "Android" / "Sdk");
        candidates.push_back(*p / "android-sdk");
    }
#endif
    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::is_directory(c / "platforms", ec)) return c;
        if (o.sdk) break;  // an explicit --android-sdk must be right
    }
    throw std::runtime_error("no Android SDK found; pass --android-sdk or set ANDROID_HOME (docs/user/android.md)");
}

fs::path find_ndk(const Options& o, const fs::path& sdk) {
    const auto ok = [](const fs::path& p) { return fs::exists(p / "build" / "cmake" / "android.toolchain.cmake"); };
    if (o.ndk) {
        if (!ok(*o.ndk)) throw std::runtime_error("--android-ndk " + str(*o.ndk) + " is not an NDK");
        return *o.ndk;
    }
    for (const char* var : {"ANDROID_NDK_HOME", "ANDROID_NDK_ROOT"})
        if (auto p = env_path(var); p && ok(*p)) return *p;
    const auto found = versioned_dirs(sdk / "ndk", ok);
    if (found.empty())
        throw std::runtime_error("no Android NDK under " + str(sdk / "ndk") + "; install one with sdkmanager or pass --android-ndk");
    return found.front();
}

fs::path jdk_tool(const char* name) {
    if (auto home = env_path("JAVA_HOME")) {
        const fs::path p = *home / "bin" / (std::string(name) + kExe);
        if (fs::exists(p)) return p;
    }
    return fs::path(name);  // from PATH
}

Toolchain find_toolchain(const Options& o) {
    Toolchain t;
    t.sdk = find_sdk(o);
    const auto tools = versioned_dirs(t.sdk / "build-tools", [](const fs::path& p) {
        return fs::exists(p / "lib" / "d8.jar") && fs::exists(p / "lib" / "apksigner.jar") &&
               fs::exists(p / (std::string("aapt2") + kExe)) && fs::exists(p / (std::string("zipalign") + kExe));
    });
    if (tools.empty()) throw std::runtime_error("no build-tools under " + str(t.sdk) + "; install them with sdkmanager");
    t.build_tools = tools.front();
    const auto platforms = versioned_dirs(t.sdk / "platforms", [](const fs::path& p) {
        const auto v = version_numbers(p.filename().string());
        return fs::exists(p / "android.jar") && !v.empty() && v.front() >= kTargetSdk;
    });
    if (platforms.empty())
        throw std::runtime_error("no platforms/android-" + std::to_string(kTargetSdk) + " (or newer) under " + str(t.sdk) +
                                 "; install it with sdkmanager");
    t.android_jar = platforms.front() / "android.jar";
    t.java = jdk_tool("java");
    t.javac = jdk_tool("javac");
    t.keytool = jdk_tool("keytool");
    return t;
}

std::vector<std::uint8_t> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + str(p));
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_text(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    if (!out) throw std::runtime_error("cannot write " + str(p));
}

std::string read_text(const fs::path& p) {
    const auto bytes = read_file(p);
    return {bytes.begin(), bytes.end()};
}

fs::path template_dir(const Paths& paths) {
    const fs::path dir = paths.source / "platform" / "android";
    if (!fs::exists(dir / "AndroidManifest.xml"))
        throw std::runtime_error("no Android template under " + str(dir) + " (is " + str(paths.source) + " an AnyPSP source tree?)");
    return dir;
}

int default_version_code() {
    const auto minutes = (static_cast<long long>(std::time(nullptr)) - 1704067200LL) / 60;  // since 2024-01-01
    return static_cast<int>(std::clamp<long long>(minutes, 1, 2'100'000'000));
}

struct Native {
    std::vector<std::pair<std::string, fs::path>> libs;  // ABI -> stripped libmain.so
    fs::path sdl_java;
};

// One libmain.so per ABI: the generated code, the runtime and the Android host
// (platform/android/android.cmake) in the shared library SDLActivity loads.
Native build_native(const Game& game, const Paths& paths, const Options& o) {
    const Toolchain t = find_toolchain(o);
    const fs::path ndk = find_ndk(o, t.sdk);
    const fs::path strip = ndk / "toolchains" / "llvm" / "prebuilt" / kNdkHost / "bin" / (std::string("llvm-strip") + kExe);
    Native out;
    for (const auto& abi : o.abis) {
        const fs::path bin = paths.work / ("out-android-" + abi);
        std::vector<std::string> configure = {
            paths.cmake, "-Wno-deprecated", "-S", str(paths.source), "-B", str(bin), "-G", paths.generator.value_or("Ninja"),
            "-DCMAKE_TOOLCHAIN_FILE=" + str(ndk / "build" / "cmake" / "android.toolchain.cmake"),
            "-DANDROID_ABI=" + abi, "-DANDROID_PLATFORM=android-" + std::to_string(kMinSdk), "-DANDROID_STL=c++_static",
            "-DCMAKE_BUILD_TYPE=" + paths.build_type, "-DCMAKE_CXX_SCAN_FOR_MODULES=OFF",
            "-DANYPSP_RUNNER_NAME=game", "-DANYPSP_RUNNER_GENERATED=" + str(game.gen_dir), "-DANYPSP_RUNNER_OPT=" + paths.opt,
            "-DANYPSP_GAME_SHA256=" + game.module_sha256, "-DANYPSP_BUILD_DRIVER=OFF"};
        if (paths.sdl3_source) configure.push_back("-DFETCHCONTENT_SOURCE_DIR_SDL3=" + str(*paths.sdl3_source));
        run_checked(configure, "CMake configure (" + abi + ")");
        std::vector<std::string> build = {paths.cmake, "--build", str(bin), "--config", paths.build_type, "--target", "main"};
        if (paths.jobs) build.insert(build.end(), {"--parallel", std::to_string(*paths.jobs)});
        run_checked(build, "CMake build (" + abi + ")");
        fs::path lib;
        for (const auto& c : {bin / "libmain.so", bin / paths.build_type / "libmain.so"})
            if (fs::exists(c)) lib = c;
        if (lib.empty()) throw std::runtime_error("the " + abi + " build produced no libmain.so under " + str(bin));
        const fs::path stripped = bin / "libmain.stripped.so";
        if (fs::exists(strip)) {
            run_checked({str(strip), "--strip-unneeded", "-o", str(stripped), str(lib)}, "llvm-strip");
        } else {
            fs::copy_file(lib, stripped, fs::copy_options::overwrite_existing);
        }
        out.libs.emplace_back(abi, stripped);
        if (out.sdl_java.empty()) {
            const fs::path sdl = paths.sdl3_source ? *paths.sdl3_source : bin / "_deps" / "sdl3-src";
            out.sdl_java = sdl / "android-project" / "app" / "src" / "main" / "java";
        }
    }
    if (!fs::exists(out.sdl_java / "org" / "libsdl" / "app" / "SDLActivity.java"))
        throw std::runtime_error("no SDL Java sources under " + str(out.sdl_java));
    return out;
}

void ensure_debug_keystore(const Toolchain& t, const fs::path& keystore) {
    if (fs::exists(keystore)) return;
    fs::create_directories(keystore.parent_path());
    run_checked({str(t.keytool), "-genkeypair", "-keystore", str(keystore), "-storepass", "android", "-alias", "androiddebugkey",
                 "-keypass", "android", "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000", "-dname",
                 "CN=Android Debug,O=Android,C=US"},
                "creating the debug keystore");
}

fs::path debug_keystore_path() {
    auto home = env_path("ANDROID_USER_HOME");
    if (home) return *home / "debug.keystore";
#if defined(_WIN32)
    home = env_path("USERPROFILE");
#else
    home = env_path("HOME");
#endif
    if (!home) throw std::runtime_error("no home folder for the debug keystore; pass --keystore");
    return *home / ".android" / "debug.keystore";
}

// "key=value" lines; values are plain ASCII/UTF-8 without newlines.
std::string properties_line(const std::string& key, std::string value) {
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    return key + "=" + value + "\n";
}

}  // namespace

std::optional<std::string> parse_option(std::span<const std::string_view> args, std::size_t& i, Options& out) {
    const std::string_view a = args[i];
    if (a == "--embed-game") {
        out.embed_game = true;
        return std::string();
    }
    static constexpr std::string_view kNames[] = {"--abi",      "--android-sdk", "--android-ndk",       "--package-id", "--icon",
                                                  "--keystore", "--key-alias",   "--keystore-pass-env", "--version-code"};
    if (std::find(std::begin(kNames), std::end(kNames), a) == std::end(kNames)) return std::nullopt;
    if (i + 1 >= args.size()) return std::string(a) + " needs a value";
    const std::string v(args[++i]);
    if (a == "--abi") {
        out.abis.clear();
        std::size_t start = 0;
        while (start <= v.size()) {
            const auto comma = v.find(',', start);
            const std::string abi = v.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (std::find(std::begin(kAbis), std::end(kAbis), abi) == std::end(kAbis))
                return "unknown ABI '" + abi + "' (arm64-v8a, armeabi-v7a, x86_64, x86)";
            if (std::find(out.abis.begin(), out.abis.end(), abi) == out.abis.end()) out.abis.push_back(abi);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    } else if (a == "--android-sdk") {
        out.sdk = fs::path(v);
    } else if (a == "--android-ndk") {
        out.ndk = fs::path(v);
    } else if (a == "--package-id") {
        if (!valid_package_id(v)) return "'" + v + "' is not a valid Android package id";
        out.package_id = v;
    } else if (a == "--icon") {
        out.icon = fs::path(v);
    } else if (a == "--keystore") {
        out.keystore = fs::path(v);
    } else if (a == "--keystore-pass-env") {
        out.keystore_pass_env = v;
    } else if (a == "--key-alias") {
        out.key_alias = v;
    } else if (a == "--version-code") {
        char* end = nullptr;
        const long n = std::strtol(v.c_str(), &end, 10);
        if (*end != '\0' || n < 1 || n > 2'100'000'000) return "--version-code must be 1-2100000000";
        out.version_code = static_cast<int>(n);
    }
    return std::string();
}

std::string usage() {
    return "Android (--target android; docs/user/android.md):\n"
           "  --abi LIST              comma-separated ABIs (default: arm64-v8a; x86_64 for emulators)\n"
           "  --embed-game            put the disc files into the APK (default: the player picks the ISO or\n"
           "                          disc folder on first start; a lone EBOOT/module is always embedded)\n"
           "  --android-sdk DIR       Android SDK (default: ANDROID_HOME, ANDROID_SDK_ROOT, the usual folders)\n"
           "  --android-ndk DIR       NDK (default: ANDROID_NDK_HOME, newest <sdk>/ndk/*)\n"
           "  --package-id ID         application id (default: io.anyport.anypsp.g<executable SHA-256 prefix>)\n"
           "  --icon FILE.png         launcher icon (192x192 recommended)\n"
           "  --keystore FILE         sign with this keystore instead of the debug key\n"
           "  --keystore-pass-env VAR environment variable holding the keystore password\n"
           "  --key-alias NAME        key in the keystore (default: androiddebugkey)\n"
           "  --version-code N        Android versionCode (default: minutes since 2024, so rebuilds update)\n";
}

bool valid_package_id(std::string_view id) {
    int segments = 0;
    std::size_t start = 0;
    while (start <= id.size()) {
        const auto dot = id.find('.', start);
        const std::string_view seg = id.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (seg.empty() || !std::isalpha(static_cast<unsigned char>(seg[0]))) return false;
        for (const char ch : seg)
            if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') return false;
        ++segments;
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return segments >= 2;
}

std::string default_package_id(std::string_view module_sha256) {
    return "io.anyport.anypsp.g" + std::string(module_sha256.substr(0, 12));
}

std::string xml_escape(std::string_view text) {
    std::string out;
    for (const char ch : text) {
        switch (ch) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += ch;
        }
    }
    return out;
}

std::string fill_template(std::string text, const std::vector<std::pair<std::string, std::string>>& values) {
    for (const auto& [key, value] : values) {
        const std::string marker = "@" + key + "@";
        for (auto at = text.find(marker); at != std::string::npos; at = text.find(marker, at + value.size()))
            text.replace(at, marker.size(), value);
    }
    for (auto at = text.find('@'); at != std::string::npos; at = text.find('@', at + 1)) {
        const auto end = text.find('@', at + 1);
        if (end == std::string::npos) break;
        const std::string_view key(text.data() + at + 1, end - at - 1);
        if (!key.empty() &&
            std::all_of(key.begin(), key.end(), [](char c) { return std::isupper(static_cast<unsigned char>(c)) || c == '_'; }))
            throw std::runtime_error("template placeholder @" + std::string(key) + "@ has no value");
    }
    return text;
}

// aapt2 for the manifest, resources and assets, javac + d8 for the code, then the native
// libraries stored uncompressed, zipalign (16 KB pages for .so) and apksigner.
fs::path build_apk(const Game& game, const Paths& paths, const Options& o) {
    const fs::path templ = template_dir(paths);
    const std::string id = o.package_id.value_or(default_package_id(game.module_sha256));
    if (!valid_package_id(id)) throw std::runtime_error("'" + id + "' is not a valid Android package id");
    const Native native = build_native(game, paths, o);
    const Toolchain t = find_toolchain(o);
    const fs::path dir = paths.work / "apk";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto tool = [&t](const char* name) { return str(t.build_tools / (std::string(name) + kExe)); };

    const fs::path manifest = dir / "AndroidManifest.xml";
    write_text(manifest, fill_template(read_text(templ / "AndroidManifest.xml"), {{"PACKAGE", id}, {"LABEL", xml_escape(game.name)}}));

    const fs::path res = dir / "res";
    fs::create_directories(res);
    fs::copy(templ / "res", res, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    if (o.icon) {
        const auto png = read_file(*o.icon);
        if (png.size() < 8 || png[0] != 0x89 || png[1] != 'P' || png[2] != 'N' || png[3] != 'G')
            throw std::runtime_error("--icon " + str(*o.icon) + " is not a PNG file");
        fs::copy_file(*o.icon, res / "mipmap-xxxhdpi" / "ic_launcher.png", fs::copy_options::overwrite_existing);
    }

    // assets/anypsp: what the app needs to start the game; assets/game: embedded game files.
    const fs::path assets = dir / "assets";
    const int version_code = o.version_code.value_or(default_version_code());
    std::string props = "# AnyPSP game\nformat=1\n";
    props += properties_line("title", game.title);
    props += properties_line("disc_id", game.disc_id);
    props += properties_line("sha256", game.module_sha256);
    props += properties_line("guest_path", game.guest_path);
    props += properties_line("needs_disc", game.needs_disc ? "1" : "0");
    std::string listing;
    for (const auto& [from, name] : game.files) {
        fs::create_directories((assets / "game" / name).parent_path());
        fs::copy_file(from, assets / "game" / name, fs::copy_options::overwrite_existing);
        listing += name + "\n";
        if (name.find('/') == std::string::npos) props += properties_line("module", name);
    }
    props += properties_line("version", std::to_string(version_code));
    write_text(assets / "anypsp" / "game.properties", props);
    write_text(assets / "anypsp" / "files.txt", listing);  // AssetManager cannot list recursively in one call
    fs::copy_file(paths.source / "data" / "nids.toml", assets / "anypsp" / "nids.toml", fs::copy_options::overwrite_existing);

    run_checked({tool("aapt2"), "compile", "--dir", str(res), "-o", str(dir / "res.zip")}, "aapt2 compile");
    std::vector<std::string> link = {tool("aapt2"), "link", "-o", str(dir / "base.apk"), "-I", str(t.android_jar), "--manifest",
                                     str(manifest), "--min-sdk-version", std::to_string(kMinSdk), "--target-sdk-version",
                                     std::to_string(kTargetSdk), "--version-code", std::to_string(version_code), "--version-name",
                                     ANYPSP_VERSION, "-A", str(assets)};
    // Debug-key builds are debuggable, so tests can read the app's files (platform/android/emulator-smoke.sh).
    if (!o.keystore) link.push_back("--debug-mode");
    link.push_back(str(dir / "res.zip"));
    run_checked(link, "aapt2 link");

    std::string sources;
    for (const auto& root : {native.sdl_java, templ / "java"}) {
        for (const auto& e : fs::recursive_directory_iterator(root)) {
            if (e.path().extension() != ".java") continue;
            std::string p = str(e.path());
            std::replace(p.begin(), p.end(), '\\', '/');
            sources += "\"" + p + "\"\n";
        }
    }
    write_text(dir / "sources.txt", sources);
#if defined(_WIN32)
    const std::string separator = ";";
#else
    const std::string separator = ":";
#endif
    // core-lambda-stubs.jar supplies LambdaMetafactory, which android.jar lacks; d8 desugars it.
    std::string boot = str(t.android_jar);
    if (fs::exists(t.build_tools / "core-lambda-stubs.jar")) boot += separator + str(t.build_tools / "core-lambda-stubs.jar");
    run_checked({str(t.javac), "-encoding", "UTF-8", "-source", "8", "-target", "8", "-bootclasspath", boot, "-nowarn",
                 "-Xlint:-options", "-d", str(dir / "classes"), "@" + str(dir / "sources.txt")},
                "javac");
    zip::write(dir / "classes.jar", std::nullopt, zip::directory_entries(dir / "classes"));
    fs::create_directories(dir / "dex");
    run_checked({str(t.java), "-cp", str(t.build_tools / "lib" / "d8.jar"), "com.android.tools.r8.D8", "--release", "--min-api",
                 std::to_string(kMinSdk), "--lib", str(t.android_jar), "--output", str(dir / "dex"), str(dir / "classes.jar")},
                "d8");

    std::vector<zip::Entry> entries;
    for (const auto& [abi, lib] : native.libs) entries.push_back({"lib/" + abi + "/libmain.so", read_file(lib)});
    entries.push_back({"classes.dex", read_file(dir / "dex" / "classes.dex")});
    zip::write(dir / "unaligned.apk", dir / "base.apk", entries);
    run_checked({tool("zipalign"), "-f", "-P", "16", "4", str(dir / "unaligned.apk"), str(dir / "aligned.apk")}, "zipalign");

    const fs::path output = paths.output_dir / (game.name + ".apk");
    std::vector<std::string> sign = {str(t.java), "-jar", str(t.build_tools / "lib" / "apksigner.jar"), "sign"};
    if (o.keystore) {
        if (!fs::exists(*o.keystore)) throw std::runtime_error("keystore " + str(*o.keystore) + " does not exist");
        if (!o.keystore_pass_env) throw std::runtime_error("--keystore needs --keystore-pass-env");
        if (!std::getenv(o.keystore_pass_env->c_str()))
            throw std::runtime_error("environment variable " + *o.keystore_pass_env + " is not set");
        const std::string pass = "env:" + *o.keystore_pass_env;
        sign.insert(sign.end(), {"--ks", str(*o.keystore), "--ks-key-alias", o.key_alias, "--ks-pass", pass, "--key-pass", pass});
    } else {
        const fs::path ks = debug_keystore_path();
        ensure_debug_keystore(t, ks);
        sign.insert(sign.end(), {"--ks", str(ks), "--ks-key-alias", "androiddebugkey", "--ks-pass", "pass:android", "--key-pass",
                                 "pass:android"});
    }
    fs::create_directories(output.parent_path());
    sign.insert(sign.end(), {"--out", str(output), str(dir / "aligned.apk")});
    run_checked(sign, "apksigner");
    return output;
}

}  // namespace anypsp::driver::android
