// SPDX-License-Identifier: GPL-2.0-or-later

#include "display.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <cmath>
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


int parse_choice(const std::string &value, std::span<const int> allowed, const char *zero, const std::string &where,
                 const std::string &name) {
    int n = -1;
    if (zero != nullptr && lower(value) == zero) {
        n = 0;
    } else {
        try {
            std::size_t used = 0;
            n = std::stoi(value, &used);
            if (used != value.size()) n = -1;
        } catch (const std::logic_error &) {
            n = -1;
        }
    }
    if (std::find(allowed.begin(), allowed.end(), n) == allowed.end()) {
        std::string list;
        for (const int a : allowed) list += (list.empty() ? "" : ", ") + (a == 0 ? std::string(zero) : std::to_string(a));
        throw psprecomp::Error(where + name + " is one of " + list + ", not '" + value + "'");
    }
    return n;
}

std::string choice_name(int value, const char *zero) { return value == 0 ? zero : std::to_string(value); }

} // namespace

void apply_setting(Settings &settings, const std::string &name, const std::string &value, const std::string &where) {
    const std::string key = lower(name);
    if (key == "smoothing") settings.smoothing = parse_bool(value, where, "Smoothing");
    else if (key == "interpolate") settings.interpolate = parse_bool(value, where, "Interpolate");
    else if (key == "refresh") settings.refresh = parse_choice(value, kRefreshRates, "auto", where, "Refresh");
    else if (key == "vrr") settings.vrr = parse_bool(value, where, "VRR");
    else if (key == "synctomonitor") settings.sync_to_monitor = parse_bool(value, where, "SyncToMonitor");
    else if (key == "pausefps") settings.menu_fps = parse_choice(value, kPauseFps, nullptr, where, "PauseFps");
    else if (key == "fpscap") settings.fps_cap = parse_choice(value, kFpsCaps, "off", where, "FpsCap");
    else
        throw psprecomp::Error(where + "unknown display setting " + name +
                               " (Smoothing, Interpolate, Refresh, VRR, SyncToMonitor, PauseFps, FpsCap)");
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
           std::string(s.smoothing ? "on" : "off") +
           "\n# Frame interpolation for 30 fps games: draws the in-between vblank with everything moved half way.\n"
           "# Costs a second GPU picture per frame and one vblank of latency; read when the game starts.\nInterpolate = " +
           (s.interpolate ? "on" : "off") +
           "\n# Monitor refresh: auto (detected per monitor) or 60, 75, 90, 120, 144, 165, 240.\nRefresh = " +
           choice_name(s.refresh, "auto") +
           "\n# Variable refresh monitor (G-Sync, FreeSync): present each vblank as soon as it is ready.\nVRR = " +
           (s.vrr ? "on" : "off") +
           "\n# Run the PSP's vblanks at the monitor's rate when within 1% (sound resampled to match).\nSyncToMonitor = " +
           (s.sync_to_monitor ? "on" : "off") + "\n# Pictures per second while paused: 30 or 60.\nPauseFps = " +
           std::to_string(s.menu_fps) + "\n# Most pictures per second: off, 30, 60, 120.\nFpsCap = " +
           choice_name(s.fps_cap, "off") + "\n";
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

int next_value(std::span<const int> allowed, int value) {
    const auto it = std::find(allowed.begin(), allowed.end(), value);
    return it == allowed.end() || it + 1 == allowed.end() ? allowed.front() : *(it + 1);
}

Cadence::Cadence(double game_hz, double display_hz) : game_hz_(game_hz), display_hz_(display_hz) {
    if (!(game_hz > 0.0) || !(display_hz > 0.0) || !std::isfinite(game_hz) || !std::isfinite(display_hz))
        throw psprecomp::Error("frame rates must be positive");
    game_uhz_ = static_cast<std::uint64_t>(std::llround(game_hz * 1e6));
    display_uhz_ = static_cast<std::uint64_t>(std::llround(display_hz * 1e6));
}

std::uint64_t Cadence::first_vblank(std::uint64_t frame) const { return (frame * display_uhz_ + game_uhz_ - 1u) / game_uhz_; }

double synced_game_hz(double game_hz, double display_hz, double tolerance) {
    if (!(display_hz > 0.0)) return game_hz;
    for (int divisor = 1; divisor <= 4; ++divisor) {
        const double rate = display_hz / divisor;
        if (std::abs(rate / game_hz - 1.0) <= tolerance) return rate;
    }
    return game_hz;
}

int vsync_interval(double display_hz, int cap) {
    if (cap <= 0 || !(display_hz > 0.0)) return 1;
    return std::max(1, static_cast<int>(std::ceil(display_hz / cap - 0.01)));
}

bool same_rate(double a, double b) { return a > 0.0 && b > 0.0 && std::abs(a / b - 1.0) <= 0.01; }

double audio_ratio(double sync_factor, std::size_t queued, std::size_t target) {
    if (target == 0u) return sync_factor;
    const double error = (static_cast<double>(queued) - static_cast<double>(target)) / static_cast<double>(target);
    return sync_factor * (1.0 + std::clamp(0.005 * error, -0.005, 0.005));
}

bool blend_on_new_frame(bool smoothing, unsigned previous_frame_vblanks, double difference) {
    return smoothing && previous_frame_vblanks == 2u && difference <= kSceneCut;
}

bool in_between_on_new_frame(bool interpolate, unsigned previous_frame_vblanks, double difference) {
    return interpolate && previous_frame_vblanks == 2u && difference <= kSceneCut;
}

unsigned FrameTracker::after_vblank(std::uint64_t buffer_changes) {
    if (buffer_changes == changes_) {
        ++vblanks_;
        return 0u;
    }
    const unsigned previous = vblanks_;
    changes_ = buffer_changes;
    vblanks_ = 1u;
    return previous;
}

} // namespace anypsp::display
