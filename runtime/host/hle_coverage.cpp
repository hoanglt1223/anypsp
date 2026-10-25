// SPDX-License-Identifier: GPL-2.0-or-later
//
// anypsp-hle-coverage: how many functions of each PSP system library the HLE kernel implements,
// against the NIDs in data/nids.toml. Run after every build of the host (see CMakeLists.txt); it
// writes a JSON report and a Markdown table.
//
//   anypsp-hle-coverage [--json file] [--markdown file] [--nids data/nids.toml]

#include "kernel.hpp"

#include "nid_db.hpp"

#include "psprecomp/runtime.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>

namespace {

struct Library {
    std::set<std::uint32_t> known;        // NIDs data/nids.toml lists
    std::set<std::uint32_t> implemented;  // NIDs the kernel registers
};

std::string json_escape(const std::string &text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

} // namespace

int main(int argc, char **argv) {
    std::string json_path, markdown_path, nids_path = anypsp::loader::default_nid_database().string();
    for (int i = 1; i < argc; i += 2) {
        const std::string flag = argv[i];
        if (i + 1 >= argc) {
            std::cerr << flag << " needs a value\n";
            return 2;
        }
        if (flag == "--json") json_path = argv[i + 1];
        else if (flag == "--markdown") markdown_path = argv[i + 1];
        else if (flag == "--nids") nids_path = argv[i + 1];
        else {
            std::cerr << "usage: anypsp-hle-coverage [--json file] [--markdown file] [--nids data/nids.toml]\n";
            return 2;
        }
    }
    try {
        std::map<std::string, Library> libraries;
        psprecomp::NidRegistry known;
        anypsp::loader::load_nid_toml(nids_path, known);
        for (const auto &symbol : known.all()) libraries[symbol.library].known.insert(symbol.nid);

        psprecomp::Runtime runtime;
        pspweb::Kernel kernel(runtime);
        kernel.install({});
        for (const std::string &key : kernel.hle_functions()) {
            const auto colon = key.rfind(':');
            libraries[key.substr(0, colon)].implemented.insert(static_cast<std::uint32_t>(std::stoul(key.substr(colon + 1), nullptr, 16)));
        }

        std::size_t total_known = 0, total_done = 0;
        for (auto &[name, lib] : libraries) {
            lib.known.insert(lib.implemented.begin(), lib.implemented.end());
            total_known += lib.known.size();
            total_done += lib.implemented.size();
        }
        const auto percent = [](std::size_t done, std::size_t all) { return all == 0 ? 0.0 : 100.0 * static_cast<double>(done) / static_cast<double>(all); };

        if (!json_path.empty()) {
            std::ofstream out(json_path, std::ios::binary);
            out << "{\n  \"implemented\": " << total_done << ",\n  \"known\": " << total_known << ",\n  \"libraries\": {\n";
            bool first = true;
            for (const auto &[name, lib] : libraries) {
                if (lib.implemented.empty()) continue;
                out << (first ? "" : ",\n") << "    \"" << json_escape(name) << "\": {\"implemented\": " << lib.implemented.size()
                    << ", \"known\": " << lib.known.size() << "}";
                first = false;
            }
            out << "\n  }\n}\n";
            if (!out) throw std::runtime_error("cannot write " + json_path);
        }
        if (!markdown_path.empty()) {
            std::ofstream out(markdown_path, std::ios::binary);
            char line[160];
            std::snprintf(line, sizeof line, "%zu of %zu known functions (%.1f%%)", total_done, total_known, percent(total_done, total_known));
            out << "# HLE coverage\n\n" << line << " in the libraries below. Libraries with no implemented "
                << "function are left out.\n\n| Library | Implemented | Known | % |\n|---|---:|---:|---:|\n";
            for (const auto &[name, lib] : libraries) {
                if (lib.implemented.empty()) continue;
                std::snprintf(line, sizeof line, "| %s | %zu | %zu | %.1f |\n", name.c_str(), lib.implemented.size(), lib.known.size(),
                              percent(lib.implemented.size(), lib.known.size()));
                out << line;
            }
            if (!out) throw std::runtime_error("cannot write " + markdown_path);
        }
        std::printf("[hle-coverage] %zu of %zu known functions (%.1f%%) in %zu libraries\n", total_done, total_known,
                    percent(total_done, total_known), libraries.size());
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "[hle-coverage] " << e.what() << "\n";
        return 1;
    }
}
