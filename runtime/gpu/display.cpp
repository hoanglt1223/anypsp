// SPDX-License-Identifier: GPL-2.0-or-later

#include "display.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace anypsp::display {
namespace {

std::string trim(const std::string &s) {
    const auto first = s.find_first_not_of(" \t\r");
    const auto last = s.find_last_not_of(" \t\r");
    return first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool parse_bool(const std::string &value, const std::string &where, const std::string &name) {
    const std::string v = lower(value);
    if (v == "on" || v == "true" || v == "1" || v == "yes") return true;
    if (v == "off" || v == "false" || v == "0" || v == "no") return false;
    throw psprecomp::Error(where + name + " is on or off, not '" + value + "'");
}

} // namespace

void apply_setting(Settings &settings, const std::string &name, const std::string &value, const std::string &where) {
    const std::string key = lower(name);
    if (key == "smoothing") settings.smoothing = parse_bool(value, where, "Smoothing");
    else throw psprecomp::Error(where + "unknown display setting " + name + " (Smoothing)");
}

void apply_ini(Settings &settings, const std::string &text, const std::string &source) {
    std::istringstream in(text);
    std::string line, section;
    for (int number = 1; std::getline(in, line); ++number) {
        const std::string where = source + ":" + std::to_string(number) + ": ";
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[') {
            if (line.back() != ']') throw psprecomp::Error(where + "unterminated section");
            section = lower(trim(line.substr(1, line.size() - 2)));
            if (section != "display") throw psprecomp::Error(where + "unknown section [" + section + "]");
            continue;
        }
        if (section.empty()) throw psprecomp::Error(where + "setting outside [display]");
        const auto equals = line.find('=');
        if (equals == std::string::npos) throw psprecomp::Error(where + "expected name = value");
        apply_setting(settings, trim(line.substr(0, equals)), trim(line.substr(equals + 1)), where);
    }
}

std::string to_ini(const Settings &s) {
    return "# AnyPSP display settings (docs/user/desktop.md). Changes made while playing are saved here.\n"
           "[display]\n"
           "# 50/50 blend on the vblank between two frames of a game drawing at 30 fps (adds one vblank of latency).\n"
           "Smoothing = " +
           std::string(s.smoothing ? "on" : "off") + "\n";
}

std::filesystem::path find_file(const std::filesystem::path &program_dir, const std::filesystem::path &user_dir) {
    std::error_code ec;
    if (!program_dir.empty() && std::filesystem::exists(program_dir / kFileName, ec)) return program_dir / kFileName;
    return user_dir / kFileName;
}

Settings load(const std::filesystem::path &path) {
    Settings settings;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return settings;
    std::ifstream in(path, std::ios::binary);
    if (!in) throw psprecomp::Error("Cannot open " + path.string());
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    apply_ini(settings, text, path.string());
    return settings;
}

void save(const std::filesystem::path &path, const Settings &settings) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << to_ini(settings);
        out.close();
        if (!out) throw psprecomp::Error("Cannot write " + tmp.string());
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) throw psprecomp::Error("Cannot replace " + path.string() + ": " + ec.message());
}

double mean_difference(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) {
    if (a.size() != b.size() || a.size() < 4u) return 1.0;
    std::uint64_t sum = 0, count = 0;
    for (std::size_t i = 0; i + 3u < a.size(); i += 4u) {
        for (std::size_t c = 0; c < 3u; ++c) sum += static_cast<std::uint64_t>(std::abs(int(a[i + c]) - int(b[i + c])));
        count += 3u;
    }
    return static_cast<double>(sum) / (255.0 * static_cast<double>(count));
}

bool blend_on_new_frame(bool smoothing, unsigned previous_frame_vblanks, double difference) {
    return smoothing && previous_frame_vblanks == 2u && difference <= kSceneCut;
}

} // namespace anypsp::display
