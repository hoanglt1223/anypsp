// SPDX-License-Identifier: GPL-2.0-or-later
#include "build.h"

#include <cstdio>
#include <exception>
#include <span>
#include <string_view>
#include <vector>

namespace {

const char* const kUsage =
    "usage: anypsp <command> [arguments]\n"
    "\n"
    "Commands:\n"
    "  build <game> [options]   translate a PSP game into a native program or an Android app\n"
    "  help [command]           show help\n"
    "\n"
    "Run `anypsp help build` for the options. The steps alone are anypsp-analyze and psp_recomp.\n";

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + (argc > 0 ? 1 : 0), argv + argc);
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        if (args.size() > 1 && args[1] == "build") {
            std::fputs(anypsp::driver::build_usage().c_str(), stdout);
        } else {
            std::fputs(kUsage, args.empty() ? stderr : stdout);
        }
        return args.empty() ? 2 : 0;
    }
    if (args[0] == "--version") {
        std::puts("anypsp " ANYPSP_VERSION);
        return 0;
    }
    if (args[0] != "build") {
        std::fprintf(stderr, "anypsp: unknown command '%.*s'\n\n%s", static_cast<int>(args[0].size()), args[0].data(), kUsage);
        return 2;
    }
    const std::span<const std::string_view> rest(args.data() + 1, args.size() - 1);
    if (rest.size() == 1 && (rest[0] == "--help" || rest[0] == "-h")) {
        std::fputs(anypsp::driver::build_usage().c_str(), stdout);
        return 0;
    }
    anypsp::driver::BuildOptions options;
    if (const auto error = anypsp::driver::parse_build_args(rest, options)) {
        std::fprintf(stderr, "anypsp build: %s\n\n%s", error->c_str(), anypsp::driver::build_usage().c_str());
        return 2;
    }
    try {
        const auto out = anypsp::driver::build(options);
        std::printf("anypsp: wrote %s\n", anypsp::driver::path_string(out).c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "anypsp build: %s\n", e.what());
        return 1;
    }
}
