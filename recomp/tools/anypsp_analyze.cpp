// SPDX-License-Identifier: GPL-2.0-or-later
//
// anypsp-analyze: loads a game's main executable and its PRX modules at a fixed
// layout, resolves imports, discovers functions and writes a report plus the
// inputs `psp_recomp --auto --seeds` needs.

#include "elf_prx.hpp"
#include "functions.hpp"
#include "nid_db.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>

namespace {

using psprecomp::hex32;
namespace loader = anypsp::loader;
namespace analysis = anypsp::analysis;

const char *const kUsage =
    "Usage: anypsp-analyze --out <dir> [--nids nids.toml] [--layout layout.txt] [--feedback file]...\n"
    "                      <EBOOT.BIN or ELF/PRX> [more PRX modules]...\n"
    "Inputs must be decrypted. Writes <dir>/report.json, <dir>/imports.csv, <dir>/layout.txt\n"
    "and <dir>/<n>_<module>_seeds.csv (input for psp_recomp --auto --seeds).\n";

struct Options {
    std::filesystem::path out;
    std::filesystem::path nids = loader::default_nid_database();
    std::filesystem::path layout;
    std::vector<std::filesystem::path> feedback;
    std::vector<std::filesystem::path> inputs;
};

std::string json(const std::string &text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20u) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
                out += buffer;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

std::string jhex(std::uint32_t value) { return json(hex32(value)); }

std::string safe_name(std::string name) {
    for (auto &c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') c = '_';
    return name;
}

const char *target_name(loader::ImportTarget target) {
    switch (target) {
    case loader::ImportTarget::Hle: return "hle";
    case loader::ImportTarget::GameModule: return "game_module";
    case loader::ImportTarget::Unknown: return "unknown";
    }
    return "unknown";
}

Options parse(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) throw psprecomp::Error(arg + " needs a value");
            return argv[++i];
        };
        if (arg == "--out") options.out = value();
        else if (arg == "--nids") options.nids = value();
        else if (arg == "--layout") options.layout = value();
        else if (arg == "--feedback") options.feedback.emplace_back(value());
        else if (arg == "--help" || arg == "-h") { std::cout << kUsage; std::exit(0); }
        else if (arg.starts_with("--")) throw psprecomp::Error("Unknown option " + arg);
        else options.inputs.emplace_back(arg);
    }
    if (options.out.empty() || options.inputs.empty()) throw psprecomp::Error("--out and at least one input are required");
    return options;
}

} // namespace

int main(int argc, char **argv) {
    try {
        const Options options = parse(argc, argv);
        psprecomp::NidRegistry nids;
        const std::size_t nid_count = loader::load_nid_toml(options.nids, nids);

        std::vector<psprecomp::Elf32Image> images;
        std::vector<loader::LayoutRequest> requests;
        for (const auto &input : options.inputs) {
            images.push_back(psprecomp::Elf32Image::from_bytes(loader::read_executable(input), input.string()));
            const auto &elf = images.back();
            std::uint32_t low = 0xFFFFFFFFu;
            for (const auto &segment : elf.segments())
                if (segment.type == 1u) low = std::min(low, segment.vaddr);
            requests.push_back({input.generic_string(), loader::image_extent(elf), elf.is_psp_prx(), low});
        }
        const auto fixed = options.layout.empty() ? std::map<std::string, std::uint32_t>{} : loader::read_layout_file(options.layout);
        const auto bases = loader::plan_layout(requests, fixed);

        auto memory = std::make_unique<psprecomp::GuestMemory>();
        std::vector<loader::LoadedModule> modules;
        for (std::size_t i = 0; i < images.size(); ++i)
            modules.push_back(loader::load_module(*memory, std::move(images[i]), requests[i].path, bases[i]));
        const auto links = loader::link_imports(modules, nids);

        analysis::DiscoveryOptions discovery_options;
        for (const auto &path : options.feedback) {
            const auto addresses = analysis::read_feedback(path);
            discovery_options.feedback.insert(discovery_options.feedback.end(), addresses.begin(), addresses.end());
        }

        std::filesystem::create_directories(options.out);
        {
            std::ofstream layout(options.out / "layout.txt");
            layout << "# base path (anypsp-analyze --layout input)\n";
            for (const auto &m : modules) layout << hex32(m.base) << ' ' << m.path << '\n';
            if (!layout) throw psprecomp::Error("Cannot write layout.txt");
        }
        {
            std::ofstream csv(options.out / "imports.csv");
            csv << "module,library,nid,name,kind,target,export_address\n";
            for (const auto &link : links)
                csv << modules[link.module].path << ',' << link.import.library << ',' << hex32(link.import.nid) << ','
                    << link.name << ',' << (link.import.variable ? "variable" : "function") << ',' << target_name(link.target)
                    << ',' << (link.target == loader::ImportTarget::GameModule ? hex32(link.export_address) : "") << '\n';
            if (!csv) throw psprecomp::Error("Cannot write imports.csv");
        }

        std::ostringstream report;
        report << "{\n  \"nid_database\": {\"path\": " << json(options.nids.generic_string()) << ", \"entries\": " << nid_count
               << "},\n  \"modules\": [\n";
        std::size_t total_unknown = 0;
        bool all_symbols_found = true;
        for (std::size_t i = 0; i < modules.size(); ++i) {
            const auto &m = modules[i];
            const auto discovery = analysis::discover_functions(m, *memory, discovery_options);
            const auto &program = discovery.program;

            const auto seeds_name = std::to_string(i) + "_" + safe_name(m.path.substr(m.path.find_last_of("/\\") + 1)) + "_seeds.csv";
            {
                std::ofstream csv(options.out / seeds_name);
                csv << "name,address,source\n";
                for (const auto &[address, source] : program.seeds)
                    csv << "sub_" << hex32(address).substr(2) << ',' << hex32(address) << ',' << source << '\n';
                if (!csv) throw psprecomp::Error("Cannot write " + seeds_name);
            }

            std::map<std::string, std::size_t> by_source;
            for (const auto &[address, source] : program.seeds) ++by_source[source];
            std::size_t hle = 0, game = 0, unknown = 0;
            std::vector<const loader::ImportLink *> unknown_links;
            for (const auto &link : links) {
                if (link.module != i) continue;
                if (link.target == loader::ImportTarget::Hle) ++hle;
                else if (link.target == loader::ImportTarget::GameModule) ++game;
                else { ++unknown; unknown_links.push_back(&link); }
            }
            total_unknown += unknown;
            std::size_t table_targets = 0, bounded = 0;
            for (const auto &table : discovery.jump_tables) {
                table_targets += table.targets.size();
                bounded += table.bounded ? 1u : 0u;
            }
            std::size_t delay_slot_targets = 0, branches_in_slots = 0, blocks = 0, unresolved_jumps = 0;
            for (const auto &[entry, cfg] : discovery.cfgs) {
                delay_slot_targets += cfg.delay_slot_targets.size();
                branches_in_slots += cfg.branches_in_delay_slots.size();
                blocks += cfg.blocks.size();
                unresolved_jumps += cfg.unresolved_indirect_jumps.size();
            }

            const auto symbols = loader::read_function_symbols(m.elf, m.base);
            std::vector<std::pair<std::uint32_t, std::string>> missing;
            std::size_t code_symbols = 0;
            for (const auto &[address, name] : symbols) {
                if (!psprecomp::is_executable_address(program.executable_ranges, address)) continue;
                ++code_symbols;
                if (!program.seeds.contains(address)) missing.emplace_back(address, name);
            }
            if (!missing.empty()) all_symbols_found = false;

            const auto &r = m.relocations;
            report << "    {\n      \"path\": " << json(m.path) << ",\n      \"name\": " << json(m.info ? m.info->name : "")
                   << ",\n      \"base\": " << jhex(m.base) << ", \"end\": " << jhex(m.end) << ", \"entry\": " << jhex(m.entry)
                   << ", \"gp\": " << jhex(m.info ? m.info->gp : 0u) << ",\n      \"relocations\": {\"format\": "
                   << json(loader::format_name(r.format)) << ", \"applied\": " << r.applied << ", \"ignored\": " << r.ignored
                   << ", \"unsupported\": " << r.unsupported << ", \"invalid\": " << r.invalid
                   << ", \"truncated_tables\": " << r.truncated_tables << ", \"problems\": [";
            for (std::size_t p = 0; p < r.problems.size(); ++p) report << (p ? ", " : "") << json(r.problems[p]);
            report << "]},\n      \"problems\": [";
            for (std::size_t p = 0; p < m.problems.size(); ++p) report << (p ? ", " : "") << json(m.problems[p]);
            report << "],\n      \"imports\": {\"total\": " << (hle + game + unknown) << ", \"hle\": " << hle
                   << ", \"game_module\": " << game << ", \"unknown\": " << unknown << ", \"unknown_list\": [";
            for (std::size_t u = 0; u < unknown_links.size(); ++u)
                report << (u ? ", " : "") << "{\"library\": " << json(unknown_links[u]->import.library)
                       << ", \"nid\": " << jhex(unknown_links[u]->import.nid) << "}";
            report << "]},\n      \"exports\": " << m.exports.size() << ",\n      \"functions\": {\"count\": "
                   << program.seeds.size() << ", \"instructions\": " << program.covered_labels.size()
                   << ", \"blocks\": " << blocks << ", \"iterations\": " << discovery.iterations << ", \"by_source\": {";
            std::size_t s = 0;
            for (const auto &[source, count] : by_source) report << (s++ ? ", " : "") << json(source) << ": " << count;
            report << "}},\n      \"jump_tables\": {\"count\": " << discovery.jump_tables.size() << ", \"bounded\": " << bounded
                   << ", \"targets\": " << table_targets << ", \"unresolved_indirect_jumps\": " << unresolved_jumps
                   << "},\n      \"delay_slots\": {\"branch_targets_in_slots\": " << delay_slot_targets
                   << ", \"branches_in_slots\": " << branches_in_slots << "},\n      \"code_pointers\": [";
            for (std::size_t c = 0; c < discovery.code_pointers.size(); ++c) {
                const auto &cp = discovery.code_pointers[c];
                report << (c ? ", " : "") << "{\"call\": " << jhex(cp.call_pc) << ", \"function\": " << json(cp.function)
                       << ", \"address\": " << jhex(cp.address) << ", \"code\": " << (cp.executable ? "true" : "false") << "}";
            }
            report << "],\n      \"feedback\": {\"used\": " << discovery.feedback_used.size()
                   << ", \"outside_module\": " << discovery.feedback_outside.size() << "},\n      \"symbols\": {\"functions\": "
                   << code_symbols << ", \"found\": " << (code_symbols - missing.size()) << ", \"missing\": [";
            for (std::size_t k = 0; k < missing.size(); ++k)
                report << (k ? ", " : "") << "{\"address\": " << jhex(missing[k].first) << ", \"name\": " << json(missing[k].second) << "}";
            report << "]},\n      \"seeds_csv\": " << json(seeds_name) << "\n    }" << (i + 1 < modules.size() ? "," : "") << "\n";

            std::cout << m.path << ": base " << hex32(m.base) << ", relocations " << r.applied << " (" << loader::format_name(r.format)
                      << ", invalid " << r.invalid << ", unsupported " << r.unsupported << ", cut tables " << r.truncated_tables
                      << "), imports " << (hle + game + unknown) << " (hle " << hle << ", game " << game << ", unknown " << unknown
                      << "), functions " << program.seeds.size() << ", jump tables " << discovery.jump_tables.size()
                      << ", code pointers " << discovery.code_pointers.size();
            for (const auto &problem : m.problems) std::cout << "\n  problem: " << problem;
            if (code_symbols != 0u)
                std::cout << ", symbols found " << (code_symbols - missing.size()) << "/" << code_symbols;
            std::cout << "\n";
        }
        report << "  ],\n  \"unknown_imports\": " << total_unknown << ",\n  \"all_symbols_found\": "
               << (all_symbols_found ? "true" : "false") << "\n}\n";
        std::ofstream out(options.out / "report.json");
        out << report.str();
        if (!out) throw psprecomp::Error("Cannot write report.json");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "anypsp-analyze: " << e.what() << "\n" << kUsage;
        return 1;
    }
}
